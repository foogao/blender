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
 * The Original Code is Copyright (C) 2020 Blender Foundation
 * All rights reserved.
 */

/** \file
 * \ingroup bgpencil
 */
#include <iostream>
#include <list>
#include <string>

#include "MEM_guardedalloc.h"

#include "BKE_context.h"
#include "BKE_gpencil.h"
#include "BKE_gpencil_geom.h"
#include "BKE_main.h"
#include "BKE_material.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BLI_math_color.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "DNA_gpencil_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "ED_gpencil.h"

#include "UI_view2d.h"

#include "ED_view3d.h"

#include "gpencil_io_import_svg.h"
#include "gpencil_io_importer.h"

#define NANOSVG_ALL_COLOR_KEYWORDS
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"

namespace blender::io::gpencil {

/* Constructor. */
GpencilImporterSVG::GpencilImporterSVG(const char *filename,
                                       const struct GpencilImportParams *iparams)
    : GpencilImporter(iparams)
{
  set_filename(filename);
}

/* Destructor. */
GpencilImporterSVG::~GpencilImporterSVG(void)
{
  /* Nothing to do yet. */
}

bool GpencilImporterSVG::read(void)
{
  bool result = true;
  NSVGimage *svg_data = NULL;
  svg_data = nsvgParseFromFile(filename_, "mm", 96.0f);
  if (svg_data == NULL) {
    std::cout << " Could not open SVG.\n ";
    return false;
  }

  /* Create grease pencil object. */
  if (params_.ob_target == NULL) {
    params_.ob_target = create_object();
    object_created_ = true;
  }
  if (params_.ob_target == NULL) {
    std::cout << "Unable to create new object.\n";
    if (svg_data) {
      nsvgDelete(svg_data);
    }

    return false;
  }
  bGPdata *gpd = (bGPdata *)params_.ob_target->data;

  /* Loop all shapes. */
  char prv_id[70] = {"*"};
  int prefix = 0;
  for (NSVGshape *shape = svg_data->shapes; shape; shape = shape->next) {
    char *layer_id = BLI_sprintfN("%03d_%s", prefix, shape->id);
    if (!STREQ(prv_id, layer_id)) {
      prefix++;
      MEM_freeN(layer_id);
      layer_id = BLI_sprintfN("%03d_%s", prefix, shape->id);
      strcpy(prv_id, layer_id);
    }

    /* Check if the layer exist and create if needed. */
    bGPDlayer *gpl = (bGPDlayer *)BLI_findstring(
        &gpd->layers, layer_id, offsetof(bGPDlayer, info));
    if (gpl == NULL) {
      gpl = BKE_gpencil_layer_addnew(gpd, layer_id, true);
      /* Disable lights. */
      gpl->flag &= ~GP_LAYER_USE_LIGHTS;
    }
    MEM_freeN(layer_id);

    /* Check frame. */
    bGPDframe *gpf = BKE_gpencil_layer_frame_get(gpl, cfra_, GP_GETFRAME_ADD_NEW);
    /* Create materials. */
    bool is_stroke = (bool)shape->stroke.type;
    bool is_fill = (bool)shape->fill.type;
    if ((!is_stroke) && (!is_fill)) {
      is_stroke = true;
    }

    /* Create_shape materials. */
    const char *const mat_names[] = {"Stroke", "Fill"};
    int index = 0;
    if ((is_stroke) && (is_fill)) {
      index = 0;
      is_fill = false;
    }
    else if ((!is_stroke) && (is_fill)) {
      index = 1;
    }
    int32_t mat_index = create_material(mat_names[index], is_stroke, is_fill);

    /* Loop all paths to create the stroke data. */
    for (NSVGpath *path = shape->paths; path; path = path->next) {
      create_stroke(gpd, gpf, shape, path, mat_index);
    }
  }

  /* Free SVG memory. */
  nsvgDelete(svg_data);

  /* Calculate bounding box and move all points to new origin center. */
  float gp_center[3];
  BKE_gpencil_centroid_3d(gpd, gp_center);

  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        int i;
        bGPDspoint *pt;
        for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
          sub_v3_v3(&pt->x, gp_center);
        }
      }
    }
  }

  return result;
}

void GpencilImporterSVG::create_stroke(
    bGPdata *gpd, bGPDframe *gpf, NSVGshape *shape, NSVGpath *path, int32_t mat_index)
{
  const bool is_stroke = (bool)shape->stroke.type;
  const bool is_fill = (bool)shape->fill.type;

  const int edges = params_.resolution;
  const float step = 1.0f / (float)(edges - 1);

  int totpoints = (path->npts / 3) * params_.resolution;

  bGPDstroke *gps = BKE_gpencil_stroke_new(mat_index, totpoints, 1.0f);
  BLI_addtail(&gpf->strokes, gps);

  if (path->closed == '1') {
    gps->flag |= GP_STROKE_CYCLIC;
  }
  if (is_stroke) {
    gps->thickness = shape->strokeWidth * params_.scale;
  }
  /* Apply Fill vertex color. */
  if (is_fill) {
    NSVGpaint fill = shape->fill;
    convert_color(fill.color, gps->vert_color_fill);
    gps->fill_opacity_fac = gps->vert_color_fill[3];
    gps->vert_color_fill[3] = 1.0f;
  }

  /* Grease pencil is rotated 90 degrees in X axis by default. */
  float matrix[4][4];
  float scale[3] = {params_.scale, params_.scale, params_.scale};
  unit_m4(matrix);
  rotate_m4(matrix, 'X', DEG2RADF(-90.0f));
  rescale_m4(matrix, scale);

  int start_index = 0;
  for (int i = 0; i < path->npts - 1; i += 3) {
    float *p = &path->pts[i * 2];
    float a = 0.0f;
    for (int v = 0; v < edges; v++) {
      bGPDspoint *pt = &gps->points[start_index];
      pt->strength = shape->opacity;
      pt->pressure = 1.0f;
      pt->z = 0.0f;
      interp_v2_v2v2v2v2_cubic(&pt->x, &p[0], &p[2], &p[4], &p[6], a);

      /* Scale from milimeters. */
      mul_v3_fl(&pt->x, 0.001f);
      mul_m4_v3(matrix, &pt->x);

      /* Apply color to vertex color. */
      if (is_fill) {
        NSVGpaint fill = shape->fill;
        convert_color(fill.color, pt->vert_color);
      }
      if (is_stroke) {
        NSVGpaint stroke = shape->stroke;
        convert_color(stroke.color, pt->vert_color);
        gps->fill_opacity_fac = pt->vert_color[3];
      }
      pt->vert_color[3] = 1.0f;

      a += step;
      start_index++;
    }
  }

  /* Cleanup and recalculate geometry. */
  BKE_gpencil_stroke_merge_distance(gpd, gpf, gps, 0.001f, true);
  BKE_gpencil_stroke_geometry_update(gpd, gps);
}

static void unpack_nano_color(float r_col[4], const unsigned int pack)
{
  unsigned char rgb_u[4];

  rgb_u[0] = ((pack) >> 0) & 0xFF;
  rgb_u[1] = ((pack) >> 8) & 0xFF;
  rgb_u[2] = ((pack) >> 16) & 0xFF;
  rgb_u[3] = ((pack) >> 24) & 0xFF;

  r_col[0] = (float)rgb_u[0] / 255.0f;
  r_col[1] = (float)rgb_u[1] / 255.0f;
  r_col[2] = (float)rgb_u[2] / 255.0f;
  r_col[3] = (float)rgb_u[3] / 255.0f;
}

void GpencilImporterSVG::convert_color(unsigned int color, float r_linear_rgba[4])
{
  float rgba[4];
  unpack_nano_color(rgba, color);

  srgb_to_linearrgb_v3_v3(r_linear_rgba, rgba);
  r_linear_rgba[3] = rgba[3];
}

}  // namespace blender::io::gpencil
