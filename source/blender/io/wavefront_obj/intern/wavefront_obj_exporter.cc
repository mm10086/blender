/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2008 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup obj
 */

#include "MEM_guardedalloc.h"

#include <stdio.h>
#include <vector>

#include "BKE_curve.h"
#include "BKE_lib_id.h"
#include "BKE_mesh.h"
#include "BKE_mesh_mapping.h"
#include "BKE_mesh_runtime.h"
#include "BKE_scene.h"

#include "BLI_math.h"
#include "BLI_path_util.h"
#include "BLI_vector.hh"

#include "bmesh.h"
#include "bmesh_tools.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "DNA_curve_types.h"
#include "DNA_layer_types.h"
#include "DNA_modifier_types.h"
#include "DNA_scene_types.h"

#include "ED_object.h"

#include "IO_wavefront_obj.h"

#include "wavefront_obj.hh"
#include "wavefront_obj_exporter.hh"
#include "wavefront_obj_file_handler.hh"

namespace io {
namespace obj {

/**
 * Store the mesh vertex coordinates in obmesh, in world coordinates.
 */
static void store_transformed_mesh_vertices(const Mesh *me_eval,
                                            const Object *ob_eval,
                                            OBJ_obmesh_to_export &ob_mesh)
{
  uint num_verts = ob_mesh.tot_vertices = me_eval->totvert;
  float axes_transform[3][3];
  unit_m3(axes_transform);
  mat3_from_axis_conversion(DEFAULT_AXIS_FORWARD,
                            DEFAULT_AXIS_UP,
                            ob_mesh.forward_axis,
                            ob_mesh.up_axis,
                            axes_transform);

  float world_transform[4][4];
  copy_m4_m4(world_transform, ob_eval->obmat);
  mul_m4_m3m4(world_transform, axes_transform, world_transform);
  for (uint i = 0; i < num_verts; i++) {
    copy_v3_v3(ob_mesh.mvert[i].co, me_eval->mvert[i].co);
    mul_m4_v3(world_transform, ob_mesh.mvert[i].co);
    mul_v3_fl(ob_mesh.mvert[i].co, ob_mesh.scaling_factor);
  }
}

/**
 * Store the mesh's per-face per-vertex normals in ob_mesh, in world coordinates.
 */
static void store_transformed_vertex_normals(Mesh *me_eval,
                                             const Object *ob_eval,
                                             OBJ_obmesh_to_export &ob_mesh)
{
  BKE_mesh_ensure_normals(me_eval);
  float transformed_normal[3];
  for (uint i = 0; i < me_eval->totvert; i++) {
    normal_short_to_float_v3(transformed_normal, me_eval->mvert[i].no);
    mul_mat3_m4_v3(ob_eval->obmat, transformed_normal);
    normal_float_to_short_v3(ob_mesh.mvert[i].no, transformed_normal);
  }
}

/**
 * Store the vertex indices of all loops in all polygons of a mesh.
 */
static void store_polygon_vert_indices(const Mesh *me_eval, OBJ_obmesh_to_export &ob_mesh)
{
  const MLoop *mloop;
  const MPoly *mpoly = me_eval->mpoly;

  ob_mesh.tot_poly = me_eval->totpoly;
  ob_mesh.polygon_list.resize(me_eval->totpoly);

  for (uint poly_index = 0; poly_index < me_eval->totpoly; poly_index++, mpoly++) {
    mloop = &me_eval->mloop[mpoly->loopstart];
    Polygon &poly = ob_mesh.polygon_list[poly_index];

    poly.total_vertices_per_poly = mpoly->totloop;
    poly.vertex_index.resize(mpoly->totloop);

    for (int loop_index = 0; loop_index < mpoly->totloop; loop_index++) {
      /* mloop->v is 0-based index. Indices in OBJ start from 1. */
      poly.vertex_index[loop_index] = (mloop + loop_index)->v + 1;
    }
  }
}

/**
 * Store UV vertex coordinates in ob_mesh.uv_coords as well as their indices, in
 * a polygon[i].uv_vertex_index.
 */
static void store_uv_coordinates(Mesh *me_eval, OBJ_obmesh_to_export &ob_mesh)
{
  const CustomData *ldata = &me_eval->ldata;

  for (int layer_idx = 0; layer_idx < ldata->totlayer; layer_idx++) {
    const CustomDataLayer *layer = &ldata->layers[layer_idx];
    if (layer->type != CD_MLOOPUV) {
      continue;
    }

    const MPoly *mpoly = me_eval->mpoly;
    const MLoop *mloop = me_eval->mloop;
    const MLoopUV *mloopuv = static_cast<MLoopUV *>(layer->data);
    const float limit[2] = {STD_UV_CONNECT_LIMIT, STD_UV_CONNECT_LIMIT};

    UvVertMap *uv_vert_map = BKE_mesh_uv_vert_map_create(
        mpoly, mloop, mloopuv, me_eval->totpoly, me_eval->totvert, limit, false, false);

    ob_mesh.tot_uv_vertices = -1;
    for (int vertex_index = 0; vertex_index < me_eval->totvert; vertex_index++) {
      const UvMapVert *uv_vert = BKE_mesh_uv_vert_map_get_vert(uv_vert_map, vertex_index);
      while (uv_vert != NULL) {
        if (uv_vert->separate) {
          ob_mesh.tot_uv_vertices++;
        }
        Polygon &polygon_of_uv_vert = ob_mesh.polygon_list[uv_vert->poly_index];
        const uint vertices_in_poly = polygon_of_uv_vert.total_vertices_per_poly;
        /* Resize UV vertices index list. */
        polygon_of_uv_vert.uv_vertex_index.resize(vertices_in_poly);

        /* Fill up UV vertex index for current polygon's one vertex. */
        polygon_of_uv_vert.uv_vertex_index[uv_vert->loop_of_poly_index] = ob_mesh.tot_uv_vertices;

        /* Fill up UV vertices' coordinates. We don't know how many unique vertices are there, so
         * need to push back everytime. */
        ob_mesh.uv_coords.push_back(std::array<float, 2>());
        ob_mesh.uv_coords[ob_mesh.tot_uv_vertices][0] =
            mloopuv[mpoly[uv_vert->poly_index].loopstart + uv_vert->loop_of_poly_index].uv[0];
        ob_mesh.uv_coords[ob_mesh.tot_uv_vertices][1] =
            mloopuv[mpoly[uv_vert->poly_index].loopstart + uv_vert->loop_of_poly_index].uv[1];

        uv_vert = uv_vert->next;
      }
    }
    /* Actual number of total UV vertices is 1-based, as opposed to the index: 0-based. */
    ob_mesh.tot_uv_vertices += 1;
    BKE_mesh_uv_vert_map_free(uv_vert_map);
    /* No need to go over other layers. */
    break;
  }
}

static void store_curve_vertices(const OBJExportParams *export_params,
                                 OBJ_obcurve_to_export &ob_curve)
{
  /* Mesh representation of the curve object. Only vertex coordinates and edge indices, indexing
   * into those coordinates are needed.
   */
  Mesh *curve_mesh = BKE_mesh_new_from_object(ob_curve.depsgraph, ob_curve.object, true);
  ob_curve.mvert = (MVert *)MEM_callocN(curve_mesh->totvert * sizeof(MVert),
                                        "OBJ curve object vertex coordinates");
  ob_curve.edge_vert_indices.resize(curve_mesh->totedge);
  ob_curve.tot_vertices = curve_mesh->totvert;
  ob_curve.tot_edges = curve_mesh->totedge;

  float axes_transform[3][3];
  unit_m3(axes_transform);
  mat3_from_axis_conversion(DEFAULT_AXIS_FORWARD,
                            DEFAULT_AXIS_UP,
                            ob_curve.forward_axis,
                            ob_curve.up_axis,
                            axes_transform);
  float world_transform[4][4];
  copy_m4_m4(world_transform, ob_curve.object->obmat);
  mul_m4_m3m4(world_transform, axes_transform, world_transform);
  for (int i = 0; i < curve_mesh->totvert; i++) {
    copy_v3_v3(ob_curve.mvert[i].co, curve_mesh->mvert[i].co);
    mul_m4_v3(world_transform, ob_curve.mvert[i].co);
    mul_v3_fl(ob_curve.mvert[i].co, ob_curve.scaling_factor);
  }

  for (int i = 0; i < curve_mesh->totedge; i++) {
    ob_curve.edge_vert_indices[i][0] = i + 1;
    ob_curve.edge_vert_indices[i][1] = i + 2;
  }
  /* Last edge's second vertex depends on whether curve is cyclic or not. */
  ob_curve.edge_vert_indices[curve_mesh->totedge - 1][1] = curve_mesh->totvert ==
                                                                   curve_mesh->totedge ?
                                                               1 :
                                                               curve_mesh->totvert;

  BKE_id_free(NULL, curve_mesh);
}

static void triangulate_mesh(Mesh *&me_eval, Mesh *&triangulated)
{
  struct BMeshCreateParams bm_create_params {
    .use_toolflags = false
  };
  struct BMeshFromMeshParams bm_convert_params {
    /* If false, calc_face_normal triggers BLI_assert(BM_face_is_normal_valid(f)).
     * We don't need the face normals it calculates.
     */
    .calc_face_normal = true, 0, 0, 0
  };
  int triangulate_min_verts = 4;
  BMesh *bmesh = BKE_mesh_to_bmesh_ex(me_eval, &bm_create_params, &bm_convert_params);

  BM_mesh_triangulate(bmesh,
                      MOD_TRIANGULATE_NGON_BEAUTY,
                      MOD_TRIANGULATE_QUAD_SHORTEDGE,
                      triangulate_min_verts,
                      false,
                      NULL,
                      NULL,
                      NULL);
  triangulated = BKE_mesh_from_bmesh_for_eval_nomain(bmesh, NULL, me_eval);
  me_eval = triangulated;
  BM_mesh_free(bmesh);
}

static void store_geometry_per_obmesh(const OBJExportParams *export_params,
                                      OBJ_obmesh_to_export &ob_mesh)
{
  Depsgraph *depsgraph = ob_mesh.depsgraph;
  Object *ob = ob_mesh.object;
  Object *ob_eval = DEG_get_evaluated_object(depsgraph, ob);
  Mesh *me_eval = BKE_object_get_evaluated_mesh(ob_eval);

  Mesh *triangulated;
  /* Obtain triangulated mesh  */
  if (export_params->export_triangulated_mesh) {
    triangulate_mesh(me_eval, triangulated);
  }

  /* Allocate memory for all vertices' coordinates and normals. */
  ob_mesh.mvert = (MVert *)MEM_callocN(me_eval->totvert * sizeof(MVert),
                                       "OBJ mesh object vertex coordinates & normals");
  store_transformed_mesh_vertices(me_eval, ob_eval, ob_mesh);
  store_polygon_vert_indices(me_eval, ob_mesh);
  if (export_params->export_normals) {
    store_transformed_vertex_normals(me_eval, ob_eval, ob_mesh);
  }
  if (export_params->export_uv) {
    store_uv_coordinates(me_eval, ob_mesh);
  }

  if (export_params->export_triangulated_mesh) {
    BKE_id_free(NULL, triangulated);
  }
}

/**
 * Check object type to filter only exportable objects.
 */
static void check_object_type(Object *object,
                              std::vector<OBJ_obmesh_to_export> &meshes_to_export,
                              std::vector<OBJ_obcurve_to_export> &curves_to_export)
{
  switch (object->type) {
    case OB_MESH:
      meshes_to_export.push_back(OBJ_obmesh_to_export());
      meshes_to_export.back().object = object;
      break;
    case OB_CURVE:
    case OB_SURF:
      curves_to_export.push_back(OBJ_obcurve_to_export());
      curves_to_export.back().object = object;
      /* Do nothing for all other cases for now. */
    default:
      break;
  }
}

/**
 * Exports a single frame to a single file in an animation.
 */
static void export_frame(bContext *C, const OBJExportParams *export_params, const char *filepath)
{
  std::vector<OBJ_obmesh_to_export> exportable_meshes;
  std::vector<OBJ_obcurve_to_export> exportable_curves;

  ViewLayer *view_layer = CTX_data_view_layer(C);

  Base *base = static_cast<Base *>(view_layer->object_bases.first);
  for (; base; base = base->next) {
    Object *object_in_layer = base->object;
    check_object_type(object_in_layer, exportable_meshes, exportable_curves);
  }

  for (uint i = 0; i < exportable_meshes.size(); i++) {
    OBJ_obmesh_to_export &ob_mesh = exportable_meshes[i];

    ob_mesh.C = C;
    ob_mesh.depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    ob_mesh.forward_axis = export_params->forward_axis;
    ob_mesh.up_axis = export_params->up_axis;
    ob_mesh.scaling_factor = export_params->scaling_factor;

    store_geometry_per_obmesh(export_params, ob_mesh);
  }
  for (uint i = 0; i < exportable_curves.size(); i++) {
    OBJ_obcurve_to_export &ob_curve = exportable_curves[i];

    ob_curve.C = C;
    ob_curve.depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    ob_curve.forward_axis = export_params->forward_axis;
    ob_curve.up_axis = export_params->up_axis;
    ob_curve.scaling_factor = export_params->scaling_factor;
    ob_curve.export_curves_as_nurbs = export_params->export_curves_as_nurbs ||
                                      ob_curve.object->type == OB_SURF;

    if (ob_curve.export_curves_as_nurbs) {
      continue;
    }
    else {
      store_curve_vertices(export_params, ob_curve);
    }
  }

  write_mesh_objects(filepath, exportable_meshes, exportable_curves, export_params);

  for (uint i = 0; i < exportable_meshes.size(); i++) {
    MEM_freeN(exportable_meshes[i].mvert);
  }
  for (uint i = 0; i < exportable_curves.size(); i++) {
    MEM_freeN(exportable_curves[i].mvert);
  }
}

/**
 * Central internal function to call Scene update & writer functions.
 */
void exporter_main(bContext *C, const OBJExportParams *export_params)
{
  ED_object_editmode_exit(C, EM_FREEDATA);
  Scene *scene = CTX_data_scene(C);
  const char *filepath = export_params->filepath;

  /* Single frame export. */
  if (!export_params->export_animation) {
    export_frame(C, export_params, filepath);
    printf("Writing to %s\n", filepath);
    return;
  }

  int start_frame = export_params->start_frame;
  int end_frame = export_params->end_frame;
  char filepath_with_frames[FILE_MAX];
  /* To reset the Scene to its original state. */
  int original_frame = CFRA;

  for (int frame = start_frame; frame <= end_frame; frame++) {
    BLI_strncpy(filepath_with_frames, filepath, FILE_MAX);
    /* 1 _ + 11 digits for frame number (INT_MAX + sign) + 4 for extension + 1 null. */
    char frame_ext[17];
    BLI_snprintf(frame_ext, 17, "_%d.obj", frame);
    bool filepath_ok = BLI_path_extension_replace(filepath_with_frames, FILE_MAX, frame_ext);
    if (filepath_ok == false) {
      printf("Error: File Path too long.\n%s\n", filepath_with_frames);
      return;
    }

    CFRA = frame;
    BKE_scene_graph_update_for_newframe(CTX_data_ensure_evaluated_depsgraph(C), CTX_data_main(C));
    printf("Writing to %s\n", filepath_with_frames);
    export_frame(C, export_params, filepath_with_frames);
  }
  CFRA = original_frame;
}
}  // namespace obj
}  // namespace io
