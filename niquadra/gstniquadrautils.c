/*******************************************************************************
 *
 * Copyright (C) 2023 NETINT Technologies
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 ******************************************************************************/

/*!*****************************************************************************
 *  \file   gstniquadrautils.c
 *
 *  \brief  Implement of NetInt Quadra common function.
 ******************************************************************************/

#include "config.h"

#include <string.h>
#include <malloc.h>
#include <glib.h>
#include <math.h>

#include "gstniquadrautils.h"
#include "gst/gst.h"

enum GSTRounding
{
  GST_ROUND_ZERO = 0,           // Round toward zero.
  GST_ROUND_INF = 1,            // Round away from zero.
  GST_ROUND_DOWN = 2,           // Round toward -infinity.
  GST_ROUND_UP = 3,             // Round toward +infinity.
  GST_ROUND_NEAR_INF = 5,       // Round to nearest and halfway cases away from zero.
  GST_ROUND_PASS_MINMAX = 8192, // Flag to pass INT64_MIN/MAX through instead of rescaling, this avoids special cases for GST_NOPTS_VALUE
};

typedef struct _ColorEntry
{
  const char *name;             // a string representing the name of the color
  uint8_t rgb_color[3];         // RGB values for the color
} ColorEntry;

static const ColorEntry color_table[] = {
  {"AliceBlue", {0xF0, 0xF8, 0xFF}},
  {"AntiqueWhite", {0xFA, 0xEB, 0xD7}},
  {"Aqua", {0x00, 0xFF, 0xFF}},
  {"Aquamarine", {0x7F, 0xFF, 0xD4}},
  {"Azure", {0xF0, 0xFF, 0xFF}},
  {"Beige", {0xF5, 0xF5, 0xDC}},
  {"Bisque", {0xFF, 0xE4, 0xC4}},
  {"Black", {0x00, 0x00, 0x00}},
  {"BlanchedAlmond", {0xFF, 0xEB, 0xCD}},
  {"Blue", {0x00, 0x00, 0xFF}},
  {"BlueViolet", {0x8A, 0x2B, 0xE2}},
  {"Brown", {0xA5, 0x2A, 0x2A}},
  {"BurlyWood", {0xDE, 0xB8, 0x87}},
  {"CadetBlue", {0x5F, 0x9E, 0xA0}},
  {"Chartreuse", {0x7F, 0xFF, 0x00}},
  {"Chocolate", {0xD2, 0x69, 0x1E}},
  {"Coral", {0xFF, 0x7F, 0x50}},
  {"CornflowerBlue", {0x64, 0x95, 0xED}},
  {"Cornsilk", {0xFF, 0xF8, 0xDC}},
  {"Crimson", {0xDC, 0x14, 0x3C}},
  {"Cyan", {0x00, 0xFF, 0xFF}},
  {"DarkBlue", {0x00, 0x00, 0x8B}},
  {"DarkCyan", {0x00, 0x8B, 0x8B}},
  {"DarkGoldenRod", {0xB8, 0x86, 0x0B}},
  {"DarkGray", {0xA9, 0xA9, 0xA9}},
  {"DarkGreen", {0x00, 0x64, 0x00}},
  {"DarkKhaki", {0xBD, 0xB7, 0x6B}},
  {"DarkMagenta", {0x8B, 0x00, 0x8B}},
  {"DarkOliveGreen", {0x55, 0x6B, 0x2F}},
  {"Darkorange", {0xFF, 0x8C, 0x00}},
  {"DarkOrchid", {0x99, 0x32, 0xCC}},
  {"DarkRed", {0x8B, 0x00, 0x00}},
  {"DarkSalmon", {0xE9, 0x96, 0x7A}},
  {"DarkSeaGreen", {0x8F, 0xBC, 0x8F}},
  {"DarkSlateBlue", {0x48, 0x3D, 0x8B}},
  {"DarkSlateGray", {0x2F, 0x4F, 0x4F}},
  {"DarkTurquoise", {0x00, 0xCE, 0xD1}},
  {"DarkViolet", {0x94, 0x00, 0xD3}},
  {"DeepPink", {0xFF, 0x14, 0x93}},
  {"DeepSkyBlue", {0x00, 0xBF, 0xFF}},
  {"DimGray", {0x69, 0x69, 0x69}},
  {"DodgerBlue", {0x1E, 0x90, 0xFF}},
  {"FireBrick", {0xB2, 0x22, 0x22}},
  {"FloralWhite", {0xFF, 0xFA, 0xF0}},
  {"ForestGreen", {0x22, 0x8B, 0x22}},
  {"Fuchsia", {0xFF, 0x00, 0xFF}},
  {"Gainsboro", {0xDC, 0xDC, 0xDC}},
  {"GhostWhite", {0xF8, 0xF8, 0xFF}},
  {"Gold", {0xFF, 0xD7, 0x00}},
  {"GoldenRod", {0xDA, 0xA5, 0x20}},
  {"Gray", {0x80, 0x80, 0x80}},
  {"Green", {0x00, 0x80, 0x00}},
  {"GreenYellow", {0xAD, 0xFF, 0x2F}},
  {"HoneyDew", {0xF0, 0xFF, 0xF0}},
  {"HotPink", {0xFF, 0x69, 0xB4}},
  {"IndianRed", {0xCD, 0x5C, 0x5C}},
  {"Indigo", {0x4B, 0x00, 0x82}},
  {"Ivory", {0xFF, 0xFF, 0xF0}},
  {"Khaki", {0xF0, 0xE6, 0x8C}},
  {"Lavender", {0xE6, 0xE6, 0xFA}},
  {"LavenderBlush", {0xFF, 0xF0, 0xF5}},
  {"LawnGreen", {0x7C, 0xFC, 0x00}},
  {"LemonChiffon", {0xFF, 0xFA, 0xCD}},
  {"LightBlue", {0xAD, 0xD8, 0xE6}},
  {"LightCoral", {0xF0, 0x80, 0x80}},
  {"LightCyan", {0xE0, 0xFF, 0xFF}},
  {"LightGoldenRodYellow", {0xFA, 0xFA, 0xD2}},
  {"LightGreen", {0x90, 0xEE, 0x90}},
  {"LightGrey", {0xD3, 0xD3, 0xD3}},
  {"LightPink", {0xFF, 0xB6, 0xC1}},
  {"LightSalmon", {0xFF, 0xA0, 0x7A}},
  {"LightSeaGreen", {0x20, 0xB2, 0xAA}},
  {"LightSkyBlue", {0x87, 0xCE, 0xFA}},
  {"LightSlateGray", {0x77, 0x88, 0x99}},
  {"LightSteelBlue", {0xB0, 0xC4, 0xDE}},
  {"LightYellow", {0xFF, 0xFF, 0xE0}},
  {"Lime", {0x00, 0xFF, 0x00}},
  {"LimeGreen", {0x32, 0xCD, 0x32}},
  {"Linen", {0xFA, 0xF0, 0xE6}},
  {"Magenta", {0xFF, 0x00, 0xFF}},
  {"Maroon", {0x80, 0x00, 0x00}},
  {"MediumAquaMarine", {0x66, 0xCD, 0xAA}},
  {"MediumBlue", {0x00, 0x00, 0xCD}},
  {"MediumOrchid", {0xBA, 0x55, 0xD3}},
  {"MediumPurple", {0x93, 0x70, 0xD8}},
  {"MediumSeaGreen", {0x3C, 0xB3, 0x71}},
  {"MediumSlateBlue", {0x7B, 0x68, 0xEE}},
  {"MediumSpringGreen", {0x00, 0xFA, 0x9A}},
  {"MediumTurquoise", {0x48, 0xD1, 0xCC}},
  {"MediumVioletRed", {0xC7, 0x15, 0x85}},
  {"MidnightBlue", {0x19, 0x19, 0x70}},
  {"MintCream", {0xF5, 0xFF, 0xFA}},
  {"MistyRose", {0xFF, 0xE4, 0xE1}},
  {"Moccasin", {0xFF, 0xE4, 0xB5}},
  {"NavajoWhite", {0xFF, 0xDE, 0xAD}},
  {"Navy", {0x00, 0x00, 0x80}},
  {"OldLace", {0xFD, 0xF5, 0xE6}},
  {"Olive", {0x80, 0x80, 0x00}},
  {"OliveDrab", {0x6B, 0x8E, 0x23}},
  {"Orange", {0xFF, 0xA5, 0x00}},
  {"OrangeRed", {0xFF, 0x45, 0x00}},
  {"Orchid", {0xDA, 0x70, 0xD6}},
  {"PaleGoldenRod", {0xEE, 0xE8, 0xAA}},
  {"PaleGreen", {0x98, 0xFB, 0x98}},
  {"PaleTurquoise", {0xAF, 0xEE, 0xEE}},
  {"PaleVioletRed", {0xD8, 0x70, 0x93}},
  {"PapayaWhip", {0xFF, 0xEF, 0xD5}},
  {"PeachPuff", {0xFF, 0xDA, 0xB9}},
  {"Peru", {0xCD, 0x85, 0x3F}},
  {"Pink", {0xFF, 0xC0, 0xCB}},
  {"Plum", {0xDD, 0xA0, 0xDD}},
  {"PowderBlue", {0xB0, 0xE0, 0xE6}},
  {"Purple", {0x80, 0x00, 0x80}},
  {"Red", {0xFF, 0x00, 0x00}},
  {"RosyBrown", {0xBC, 0x8F, 0x8F}},
  {"RoyalBlue", {0x41, 0x69, 0xE1}},
  {"SaddleBrown", {0x8B, 0x45, 0x13}},
  {"Salmon", {0xFA, 0x80, 0x72}},
  {"SandyBrown", {0xF4, 0xA4, 0x60}},
  {"SeaGreen", {0x2E, 0x8B, 0x57}},
  {"SeaShell", {0xFF, 0xF5, 0xEE}},
  {"Sienna", {0xA0, 0x52, 0x2D}},
  {"Silver", {0xC0, 0xC0, 0xC0}},
  {"SkyBlue", {0x87, 0xCE, 0xEB}},
  {"SlateBlue", {0x6A, 0x5A, 0xCD}},
  {"SlateGray", {0x70, 0x80, 0x90}},
  {"Snow", {0xFF, 0xFA, 0xFA}},
  {"SpringGreen", {0x00, 0xFF, 0x7F}},
  {"SteelBlue", {0x46, 0x82, 0xB4}},
  {"Tan", {0xD2, 0xB4, 0x8C}},
  {"Teal", {0x00, 0x80, 0x80}},
  {"Thistle", {0xD8, 0xBF, 0xD8}},
  {"Tomato", {0xFF, 0x63, 0x47}},
  {"Turquoise", {0x40, 0xE0, 0xD0}},
  {"Violet", {0xEE, 0x82, 0xEE}},
  {"Wheat", {0xF5, 0xDE, 0xB3}},
  {"White", {0xFF, 0xFF, 0xFF}},
  {"WhiteSmoke", {0xF5, 0xF5, 0xF5}},
  {"Yellow", {0xFF, 0xFF, 0x00}},
  {"YellowGreen", {0x9A, 0xCD, 0x32}},
};

NiPluginError
gst_ni_retrieve_gop_params (char gopParams[], ni_xcoder_params_t * params)
{
  char key[64], value[64];
  char *curr = gopParams, *colon_pos;
  NiPluginError ret = NI_PLUGIN_OK;
  ni_retcode_t res = NI_RETCODE_SUCCESS;

  while (*curr) {
    colon_pos = strchr (curr, ':');

    if (colon_pos) {
      *colon_pos = '\0';
    }

    if (strlen (curr) > sizeof (key) + sizeof (value) - 1 ||
        ni_param_get_key_value (curr, key, value)) {
      GST_ERROR ("Error: xcoder-params p_config key/value not "
          "retrieved: %s\n", curr);
      ret = NI_PLUGIN_EINVAL;
      break;
    }
    res = ni_encoder_gop_params_set_value (params, key, value);
    switch (res) {
      case NI_RETCODE_PARAM_INVALID_NAME:
        GST_ERROR ("Unknown option: %s.\n", key);
        break;
      case NI_RETCODE_PARAM_ERROR_TOO_BIG:
        GST_ERROR ("Invalid custom GOP parameters: %s too big\n", key);
        break;
      case NI_RETCODE_PARAM_ERROR_TOO_SMALL:
        GST_ERROR ("Invalid custom GOP parameters: %s too small\n", key);
        break;
      case NI_RETCODE_PARAM_ERROR_OOR:
        GST_ERROR ("Invalid custom GOP parameters: %s out of range \n", key);
        break;
      case NI_RETCODE_PARAM_ERROR_ZERO:
        GST_ERROR ("Invalid custom GOP paramaters: Error setting option %s "
            "to value 0 \n", key);
        break;
      case NI_RETCODE_PARAM_INVALID_VALUE:
        GST_ERROR ("Invalid value for GOP param %s: %s.\n", key, value);
        break;
      case NI_RETCODE_PARAM_WARNING_DEPRECATED:
        GST_WARNING ("Parameter %s is deprecated\n", key);
        break;
      default:
        break;
    }

    if (NI_RETCODE_SUCCESS != res) {
      GST_ERROR ("Error: config parsing failed %d: %s\n", res,
          ni_get_rc_txt (res));
      ret = NI_PLUGIN_EINVAL;
      break;
    }

    if (colon_pos) {
      curr = colon_pos + 1;
    } else {
      curr += strlen (curr);
    }
  }
  return ret;
}

int
ni_build_frame_pool (ni_session_context_t * ctx, int width, int height,
    GstVideoFormat out_format, int pool_size)
{
  int rc;
  int scaler_format;
  int options;

  scaler_format = convertGstVideoFormatToGC620Format (out_format);
  options = NI_SCALER_FLAG_IO | NI_SCALER_FLAG_PC;
  if (ctx->isP2P) {
    options |= NI_SCALER_FLAG_P2;
  }

  /* Allocate a pool of frames by the scaler */
  /* *INDENT-OFF* */
  rc = ni_device_alloc_frame (ctx,
      NI_ALIGN (width, 2),
      NI_ALIGN (height, 2),
      scaler_format,
      options,
      0,                        // rec width
      0,                        // rec height
      0,                        // rec X pos
      0,                        // rec Y pos
      pool_size,                // rgba color/pool size
      0,                        // frame index
      NI_DEVICE_TYPE_SCALER);
  /* *INDENT-ON* */

  return rc;
}

GstVideoFormat
convertNIPixToGstVideoFormat (ni_pix_fmt_t ni_pix_fmt)
{
  GstVideoFormat gstVideoFormat = GST_VIDEO_FORMAT_UNKNOWN;
  switch (ni_pix_fmt) {
    case NI_PIX_FMT_YUV420P:
      gstVideoFormat = GST_VIDEO_FORMAT_I420;
      break;
    case NI_PIX_FMT_YUV420P10LE:
      gstVideoFormat = GST_VIDEO_FORMAT_I420_10LE;
      break;
    case NI_PIX_FMT_NV12:
      gstVideoFormat = GST_VIDEO_FORMAT_NV12;
      break;
    case NI_PIX_FMT_P010LE:
      gstVideoFormat = GST_VIDEO_FORMAT_P010_10LE;
      break;
    case NI_PIX_FMT_RGBA:
      gstVideoFormat = GST_VIDEO_FORMAT_RGBA;
      break;
    case NI_PIX_FMT_BGRA:
      gstVideoFormat = GST_VIDEO_FORMAT_BGRA;
      break;
    case NI_PIX_FMT_ARGB:
      gstVideoFormat = GST_VIDEO_FORMAT_ARGB;
      break;
    case NI_PIX_FMT_ABGR:
      gstVideoFormat = GST_VIDEO_FORMAT_ABGR;
      break;
    case NI_PIX_FMT_BGR0:
      gstVideoFormat = GST_VIDEO_FORMAT_BGR;
      break;
    case NI_PIX_FMT_BGRP:
      gstVideoFormat = GST_VIDEO_FORMAT_BGRP;
      break;
    case NI_PIX_FMT_NV16:
      gstVideoFormat = GST_VIDEO_FORMAT_NV16;
      break;
    case NI_PIX_FMT_YUYV422:
      gstVideoFormat = GST_VIDEO_FORMAT_YUY2;
      break;
    case NI_PIX_FMT_UYVY422:
      gstVideoFormat = GST_VIDEO_FORMAT_UYVY;
      break;
    case NI_PIX_FMT_8_TILED4X4:
    case NI_PIX_FMT_10_TILED4X4:
    case NI_PIX_FMT_NONE:
      GST_ERROR ("Unsupported ni_pix_fmt [%d]\n", ni_pix_fmt);
      break;
  }
  return gstVideoFormat;
}

typedef struct GC620PixelFmts
{
  GstVideoFormat pix_fmt_gs;
  int pix_fmt_gc620;
  ni_pix_fmt_t pix_fmt_libxcoder;
} GC620PixelFmts_t;

static struct GC620PixelFmts gc620_pixel_fmt_list[] = {
  {GST_VIDEO_FORMAT_NV12, GC620_NV12, NI_PIX_FMT_NV12},
  {GST_VIDEO_FORMAT_NV21, GC620_NV21, NI_PIX_FMT_NONE},
  {GST_VIDEO_FORMAT_I420, GC620_I420, NI_PIX_FMT_YUV420P},
  {GST_VIDEO_FORMAT_P010_10LE, GC620_P010_MSB, NI_PIX_FMT_P010LE},
  {GST_VIDEO_FORMAT_I420_10LE, GC620_I010, NI_PIX_FMT_YUV420P10LE},
  {GST_VIDEO_FORMAT_YUY2, GC620_YUYV, NI_PIX_FMT_YUYV422},
  {GST_VIDEO_FORMAT_UYVY, GC620_UYVY, NI_PIX_FMT_UYVY422},
  {GST_VIDEO_FORMAT_NV16, GC620_NV16, NI_PIX_FMT_NONE},
  {GST_VIDEO_FORMAT_RGBA, GC620_RGBA8888, NI_PIX_FMT_RGBA},
  {GST_VIDEO_FORMAT_BGRx, GC620_BGRX8888, NI_PIX_FMT_BGR0},
  {GST_VIDEO_FORMAT_BGRA, GC620_BGRA8888, NI_PIX_FMT_BGRA},
  {GST_VIDEO_FORMAT_ABGR, GC620_ABGR8888, NI_PIX_FMT_ABGR},
  {GST_VIDEO_FORMAT_ARGB, GC620_ARGB8888, NI_PIX_FMT_ARGB},
  {GST_VIDEO_FORMAT_RGB15, GC620_RGB565, NI_PIX_FMT_NONE},
  {GST_VIDEO_FORMAT_BGR15, GC620_BGR565, NI_PIX_FMT_NONE},
  {GST_VIDEO_FORMAT_BGR16, GC620_B5G5R5X1, NI_PIX_FMT_NONE},
  {GST_VIDEO_FORMAT_BGRP, GC620_RGB888_PLANAR, NI_PIX_FMT_BGRP},
#if ENABLE_NI_GST_FRAMEWORK_PATCH
  {GST_VIDEO_FORMAT_NI_QUAD_8_4L4, GC620_NV12, NI_PIX_FMT_NV12}
#endif
};

gint
convertGstVideoFormatToGC620Format (GstVideoFormat pix_fmt)
{
  int i, tablesz;

  tablesz = sizeof (gc620_pixel_fmt_list) / sizeof (struct GC620PixelFmts);

  /* linear search through table to find if the pixel format is supported */
  for (i = 0; i < tablesz; i++) {
    if (gc620_pixel_fmt_list[i].pix_fmt_gs == pix_fmt) {
      return gc620_pixel_fmt_list[i].pix_fmt_gc620;
    }
  }

  GST_ERROR ("Unsupported gst video format [%d]\n", pix_fmt);
  return -1;
}

ni_pix_fmt_t
convertGstVideoFormatToNIPix (GstVideoFormat pix_fmt)
{
  int i, tablesz;

  tablesz = sizeof (gc620_pixel_fmt_list) / sizeof (struct GC620PixelFmts);

  /* linear search through table to find if the pixel format is supported */
  for (i = 0; i < tablesz; i++) {
    if (gc620_pixel_fmt_list[i].pix_fmt_gs == pix_fmt) {
      return gc620_pixel_fmt_list[i].pix_fmt_libxcoder;
    }
  }

  GST_ERROR ("Unsupported gst video format [%d]\n", pix_fmt);
  return -1;
}

gint
convertNIPixToGC620Format (ni_pix_fmt_t ni_pix_fmt)
{
  int i, tablesz;

  tablesz = sizeof (gc620_pixel_fmt_list) / sizeof (struct GC620PixelFmts);

  /* linear search through table to find if the pixel format is supported */
  for (i = 0; i < tablesz; i++) {
    if (gc620_pixel_fmt_list[i].pix_fmt_libxcoder == ni_pix_fmt) {
      return gc620_pixel_fmt_list[i].pix_fmt_gc620;
    }
  }

  GST_ERROR ("Unsupported ni pixel format [%d]\n", ni_pix_fmt);
  return -1;
}

typedef struct GstNiPixelFmts
{
  GstVideoFormat pix_fmt_gs;
  ni_pix_fmt_t pix_fmt_libxcoder;
} GstNiPixelFmts_t;

static struct GstNiPixelFmts ni_pixel_fmt_list[] = {
  {GST_VIDEO_FORMAT_NV12, NI_PIX_FMT_NV12},
  {GST_VIDEO_FORMAT_I420, NI_PIX_FMT_YUV420P},
  {GST_VIDEO_FORMAT_P010_10LE, NI_PIX_FMT_P010LE},
  {GST_VIDEO_FORMAT_I420_10LE, NI_PIX_FMT_YUV420P10LE},
  {GST_VIDEO_FORMAT_YUY2, NI_PIX_FMT_YUYV422},
  {GST_VIDEO_FORMAT_UYVY, NI_PIX_FMT_UYVY422},
  {GST_VIDEO_FORMAT_RGBA, NI_PIX_FMT_RGBA},
  {GST_VIDEO_FORMAT_BGRx, NI_PIX_FMT_BGR0},
  {GST_VIDEO_FORMAT_BGRA, NI_PIX_FMT_BGRA},
  {GST_VIDEO_FORMAT_ABGR, NI_PIX_FMT_ABGR},
  {GST_VIDEO_FORMAT_ARGB, NI_PIX_FMT_ARGB},
  {GST_VIDEO_FORMAT_BGRP, NI_PIX_FMT_BGRP},
  {GST_VIDEO_FORMAT_NV16, NI_PIX_FMT_NV16},
#if ENABLE_NI_GST_FRAMEWORK_PATCH
  {GST_VIDEO_FORMAT_NI_QUAD_8_4L4, NI_PIX_FMT_8_TILED4X4},
  {GST_VIDEO_FORMAT_NI_QUAD_10_4L4, NI_PIX_FMT_10_TILED4X4}
#endif
};

/**
 * convertGstVideoFormatToXcoderPixFmt
 * Note that this function only works with codec. For filter, use convertGstVideoFormatToNIPix
 */
ni_pix_fmt_t
convertGstVideoFormatToXcoderPixFmt (GstVideoFormat pix_fmt)
{
  int i, tablesz;

  tablesz = sizeof (ni_pixel_fmt_list) / sizeof (struct GstNiPixelFmts);

  /* linear search through table to find if the pixel format is supported */
  for (i = 0; i < tablesz; i++) {
    if (ni_pixel_fmt_list[i].pix_fmt_gs == pix_fmt) {
      return ni_pixel_fmt_list[i].pix_fmt_libxcoder;
    }
  }

  GST_ERROR ("Unsupported gst video format [%d]\n", pix_fmt);
  return NI_PIX_FMT_NONE;
}

gboolean
gst_image_fill_linesizes (int linesize[4], ni_pix_fmt_t format, int width,
    int align)
{
  switch (format) {
    case NI_PIX_FMT_YUV420P:
      linesize[0] = NI_ALIGN (width, align);
      linesize[1] = NI_ALIGN (width / 2, align);
      linesize[2] = linesize[1];
      linesize[3] = 0;
      break;
    case NI_PIX_FMT_YUV420P10LE:
      linesize[0] = NI_ALIGN (width * 2, align);
      linesize[1] = NI_ALIGN (width, align);
      linesize[2] = linesize[1];
      linesize[3] = 0;
      break;
    case NI_PIX_FMT_NV12:
      linesize[0] = NI_ALIGN (width, align);
      linesize[1] = NI_ALIGN (width, align);
      linesize[2] = 0;
      linesize[3] = 0;
      break;
    case NI_PIX_FMT_P010LE:
      linesize[0] = NI_ALIGN (width * 2, align);
      linesize[1] = NI_ALIGN (width * 2, align);
      linesize[2] = 0;
      linesize[3] = 0;
      break;
    case NI_PIX_FMT_RGBA:
    case NI_PIX_FMT_BGRA:
    case NI_PIX_FMT_ARGB:
    case NI_PIX_FMT_ABGR:
    case NI_PIX_FMT_BGR0:
      linesize[0] = NI_ALIGN (width, align) * 4;
      linesize[1] = 0;
      linesize[2] = 0;
      linesize[3] = 0;
      break;
    case NI_PIX_FMT_BGRP:
      linesize[0] = NI_ALIGN (width, align);
      linesize[1] = NI_ALIGN (width, align);
      linesize[2] = NI_ALIGN (width, align);
      linesize[3] = 0;
      break;
    case NI_PIX_FMT_NV16:
      linesize[0] = NI_ALIGN (width, align);
      linesize[1] = NI_ALIGN (width, align);
      linesize[2] = 0;
      linesize[3] = 0;
      break;
    case NI_PIX_FMT_YUYV422:
    case NI_PIX_FMT_UYVY422:
      linesize[0] = NI_ALIGN (width, align) * 2;
      linesize[1] = 0;
      linesize[2] = 0;
      linesize[3] = 0;
      break;
    case NI_PIX_FMT_8_TILED4X4:
    case NI_PIX_FMT_10_TILED4X4:
    case NI_PIX_FMT_NONE:
      GST_ERROR ("Unsupported fmt:%d\n", format);
      return FALSE;
  }

  return TRUE;
}

static int
gst_ni_plane_row_bytes (const GstVideoInfo * info, int plane)
{
  return GST_VIDEO_INFO_COMP_WIDTH (info, plane) *
      GST_VIDEO_INFO_COMP_PSTRIDE (info, plane);
}

static int
gst_ni_plane_row_bytes_from_frame (GstVideoFrame * frame, int plane)
{
  return GST_VIDEO_FRAME_COMP_WIDTH (frame, plane) *
      GST_VIDEO_FRAME_COMP_PSTRIDE (frame, plane);
}

static void
gst_ni_fill_stride_tail (uint8_t * row, int row_bytes, int dst_stride,
    ni_pix_fmt_t pix_fmt, int plane)
{
  int tail, sample_size = 1;
  uint8_t *dest;
  const uint8_t *sample;

  if (dst_stride <= row_bytes || row_bytes <= 0)
    return;

  tail = dst_stride - row_bytes;
  dest = row + row_bytes;

  switch (pix_fmt) {
    case NI_PIX_FMT_YUV420P10LE:
      sample_size = 2;
      break;
    case NI_PIX_FMT_NV12:
      sample_size = (plane == 1) ? 2 : 1;
      break;
    case NI_PIX_FMT_P010LE:
      /* The chroma plane stores interleaved 16-bit U/V samples. */
      sample_size = (plane == 1) ? 4 : 2;
      break;
    default:
      memset (dest, row[row_bytes - 1], tail);
      return;
  }

  if (row_bytes < sample_size)
    sample_size = 1;

  sample = row + row_bytes - sample_size;
  while (tail >= sample_size) {
    memcpy (dest, sample, sample_size);
    dest += sample_size;
    tail -= sample_size;
  }

  if (tail > 0)
    memcpy (dest, sample, tail);
}

NiPluginError
copy_ni_to_gst_memory (const ni_frame_t * src, uint8_t * dst,
    GstVideoInfo * info)
{
  int src_linesize[4], src_height[4];
  int i, h, nb_planes;
  uint8_t *src_line, *dst_line;
  ni_pix_fmt_t niPixFmt;

  nb_planes = info->finfo->n_planes;
  niPixFmt = convertGstVideoFormatToNIPix (info->finfo->format);

  switch (niPixFmt) {
    case NI_PIX_FMT_YUV420P:
      src_linesize[0] = NI_ALIGN (info->width, 128);
      src_linesize[1] = NI_ALIGN (info->width / 2, 128);
      src_linesize[2] = src_linesize[1];
      src_linesize[3] = 0;

      src_height[0] = info->height;
      src_height[1] = NI_ALIGN (info->height, 2) / 2;
      src_height[2] = src_height[1];
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_YUV420P10LE:
      src_linesize[0] = NI_ALIGN (info->width * 2, 128);
      src_linesize[1] = NI_ALIGN (info->width, 128);
      src_linesize[2] = src_linesize[1];
      src_linesize[3] = 0;

      src_height[0] = info->height;
      src_height[1] = NI_ALIGN (info->height, 2) / 2;
      src_height[2] = src_height[1];
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_NV12:
      src_linesize[0] = NI_ALIGN (info->width, 128);
      src_linesize[1] = NI_ALIGN (info->width, 128);
      src_linesize[2] = 0;
      src_linesize[3] = 0;

      src_height[0] = info->height;
      src_height[1] = NI_ALIGN (info->height, 2) / 2;
      src_height[2] = 0;
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_P010LE:
      src_linesize[0] = NI_ALIGN (info->width * 2, 128);
      src_linesize[1] = NI_ALIGN (info->width * 2, 128);
      src_linesize[2] = 0;
      src_linesize[3] = 0;

      src_height[0] = info->height;
      src_height[1] = NI_ALIGN (info->height, 2) / 2;
      src_height[2] = 0;
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_RGBA:
    case NI_PIX_FMT_BGRA:
    case NI_PIX_FMT_ARGB:
    case NI_PIX_FMT_ABGR:
    case NI_PIX_FMT_BGR0:
      src_linesize[0] = NI_ALIGN (info->width, 16) * 4;
      src_linesize[1] = 0;
      src_linesize[2] = 0;
      src_linesize[3] = 0;

      src_height[0] = info->height;
      src_height[1] = 0;
      src_height[2] = 0;
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_BGRP:
      src_linesize[0] = NI_ALIGN (info->width, 32);
      src_linesize[1] = NI_ALIGN (info->width, 32);
      src_linesize[2] = NI_ALIGN (info->width, 32);
      src_linesize[3] = 0;

      src_height[0] = info->height;
      src_height[1] = info->height;
      src_height[2] = info->height;
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_NV16:
      src_linesize[0] = info->width;
      src_linesize[1] = info->width;
      src_linesize[2] = 0;
      src_linesize[3] = 0;

      src_height[0] = info->height;
      src_height[1] = info->height;
      src_height[2] = 0;
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_YUYV422:
    case NI_PIX_FMT_UYVY422:
      src_linesize[0] = NI_ALIGN (info->width, 16) * 2;
      src_linesize[1] = 0;
      src_linesize[2] = 0;
      src_linesize[3] = 0;

      src_height[0] = info->height;
      src_height[1] = 0;
      src_height[2] = 0;
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_8_TILED4X4:
    case NI_PIX_FMT_10_TILED4X4:
    case NI_PIX_FMT_NONE:
      GST_ERROR ("Unsupported fmt:%d\n", niPixFmt);
      return NI_PLUGIN_UNSUPPORTED;
  }

  int line_sizes[4];
  int ret = gst_image_fill_linesizes (line_sizes, niPixFmt, info->width, 1);
  if (ret == FALSE)
    return NI_PLUGIN_FAILURE;

  for (i = 0; i < nb_planes; i++) {
    int dst_stride = info->stride[i];
    int row_bytes = gst_ni_plane_row_bytes (info, i);
    int copy_size = src_linesize[i];

    if (copy_size > row_bytes)
      copy_size = row_bytes;
    if (copy_size > dst_stride)
      copy_size = dst_stride;

    dst_line = dst + info->offset[i];
    src_line = src->p_data[i];

    for (h = 0; h < src_height[i]; h++) {
      memcpy (dst_line, src_line, copy_size);
      gst_ni_fill_stride_tail (dst_line, row_bytes, dst_stride, niPixFmt, i);
      dst_line += dst_stride;
      src_line += src_linesize[i];
    }
  }

  return NI_PLUGIN_OK;
}

NiPluginError
copy_ni_to_gst_frame (const ni_frame_t * src, GstVideoFrame * dst,
    ni_pix_fmt_t niPixFmt)
{
  int src_linesize[4], src_height[4];
  int i, h, nb_planes;
  uint8_t *src_line, *dst_line;

  nb_planes = dst->info.finfo->n_planes;

  switch (niPixFmt) {
    case NI_PIX_FMT_YUV420P:
      src_linesize[0] = NI_ALIGN (dst->info.width, 128);
      src_linesize[1] = NI_ALIGN (dst->info.width / 2, 128);
      src_linesize[2] = src_linesize[1];
      src_linesize[3] = 0;

      src_height[0] = dst->info.height;
      src_height[1] = NI_ALIGN (dst->info.height, 2) / 2;
      src_height[2] = src_height[1];
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_YUV420P10LE:
      src_linesize[0] = NI_ALIGN (dst->info.width * 2, 128);
      src_linesize[1] = NI_ALIGN (dst->info.width, 128);
      src_linesize[2] = src_linesize[1];
      src_linesize[3] = 0;

      src_height[0] = dst->info.height;
      src_height[1] = NI_ALIGN (dst->info.height, 2) / 2;
      src_height[2] = src_height[1];
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_NV12:
      src_linesize[0] = NI_ALIGN (dst->info.width, 128);
      src_linesize[1] = NI_ALIGN (dst->info.width, 128);
      src_linesize[2] = 0;
      src_linesize[3] = 0;

      src_height[0] = dst->info.height;
      src_height[1] = NI_ALIGN (dst->info.height, 2) / 2;
      src_height[2] = 0;
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_P010LE:
      src_linesize[0] = NI_ALIGN (dst->info.width * 2, 128);
      src_linesize[1] = NI_ALIGN (dst->info.width * 2, 128);
      src_linesize[2] = 0;
      src_linesize[3] = 0;

      src_height[0] = dst->info.height;
      src_height[1] = NI_ALIGN (dst->info.height, 2) / 2;
      src_height[2] = 0;
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_RGBA:
    case NI_PIX_FMT_BGRA:
    case NI_PIX_FMT_ARGB:
    case NI_PIX_FMT_ABGR:
    case NI_PIX_FMT_BGR0:
      src_linesize[0] = NI_ALIGN (dst->info.width, 16) * 4;
      src_linesize[1] = 0;
      src_linesize[2] = 0;
      src_linesize[3] = 0;

      src_height[0] = dst->info.height;
      src_height[1] = 0;
      src_height[2] = 0;
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_BGRP:
      src_linesize[0] = NI_ALIGN (dst->info.width, 32);
      src_linesize[1] = NI_ALIGN (dst->info.width, 32);
      src_linesize[2] = NI_ALIGN (dst->info.width, 32);
      src_linesize[3] = 0;

      src_height[0] = dst->info.height;
      src_height[1] = dst->info.height;
      src_height[2] = dst->info.height;
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_NV16:
      src_linesize[0] = dst->info.width;
      src_linesize[1] = dst->info.width;
      src_linesize[2] = 0;
      src_linesize[3] = 0;

      src_height[0] = dst->info.height;
      src_height[1] = dst->info.height;
      src_height[2] = 0;
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_YUYV422:
    case NI_PIX_FMT_UYVY422:
      src_linesize[0] = NI_ALIGN (dst->info.width, 16) * 2;
      src_linesize[1] = 0;
      src_linesize[2] = 0;
      src_linesize[3] = 0;

      src_height[0] = dst->info.height;
      src_height[1] = 0;
      src_height[2] = 0;
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_8_TILED4X4:
    case NI_PIX_FMT_10_TILED4X4:
    case NI_PIX_FMT_NONE:
      GST_ERROR ("Unsupported fmt:%d\n", niPixFmt);
      return NI_PLUGIN_UNSUPPORTED;
  }

  int line_sizes[4];
  int ret = gst_image_fill_linesizes (line_sizes, niPixFmt, dst->info.width, 1);
  if (ret == FALSE)
    return NI_PLUGIN_FAILURE;

  for (i = 0; i < nb_planes; i++) {
    int dst_stride = GST_VIDEO_FRAME_PLANE_STRIDE (dst, i);
    int row_bytes = gst_ni_plane_row_bytes_from_frame (dst, i);
    int copy_size = src_linesize[i];

    if (copy_size > row_bytes)
      copy_size = row_bytes;
    if (copy_size > dst_stride)
      copy_size = dst_stride;

    dst_line = dst->data[i];
    src_line = src->p_data[i];

    for (h = 0; h < src_height[i]; h++) {
      memcpy (dst_line, src_line, copy_size);
      gst_ni_fill_stride_tail (dst_line, row_bytes, dst_stride, niPixFmt, i);
      dst_line += dst_stride;
      src_line += src_linesize[i];
    }
  }

  return NI_PLUGIN_OK;
}

int
copy_gst_to_ni_frame (const int dst_stride[4], ni_frame_t * dst,
    GstVideoFrame * frame)
{
  int src_height[4], hpad[4], vpad[4];
  int i, j, h, nb_planes;
  uint8_t *src_line, *dst_line, YUVsample, *sample, *dest;
  uint16_t lastidx;
  bool tenBit = false;
  ni_pix_fmt_t niPixFmt = dst->pixel_format;

  nb_planes = frame->info.finfo->n_planes;
  switch (niPixFmt) {
    case NI_PIX_FMT_YUV420P:
      hpad[0] =
          ni_max (dst_stride[0] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0), 0);
      hpad[1] =
          ni_max (dst_stride[1] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 1), 0);
      hpad[2] =
          ni_max (dst_stride[2] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 2), 0);
      hpad[3] = 0;

      src_height[0] = frame->info.height;
      src_height[1] = NI_ALIGN (frame->info.height, 2) / 2;
      src_height[2] = NI_ALIGN (frame->info.height, 2) / 2;
      src_height[3] = 0;

      vpad[0] = NI_ALIGN (src_height[0], 2) - src_height[0];
      vpad[1] = NI_ALIGN (src_height[1], 2) - src_height[1];
      vpad[2] = NI_ALIGN (src_height[2], 2) - src_height[2];
      vpad[3] = 0;

      tenBit = false;
      break;
    case NI_PIX_FMT_YUV420P10LE:
      hpad[0] =
          ni_max (dst_stride[0] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0), 0);
      hpad[1] =
          ni_max (dst_stride[1] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 1), 0);
      hpad[2] =
          ni_max (dst_stride[2] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 2), 0);
      hpad[3] = 0;

      src_height[0] = frame->info.height;
      src_height[1] = NI_ALIGN (frame->info.height, 2) / 2;
      src_height[2] = NI_ALIGN (frame->info.height, 2) / 2;
      src_height[3] = 0;

      vpad[0] = NI_ALIGN (src_height[0], 2) - src_height[0];
      vpad[1] = NI_ALIGN (src_height[1], 2) - src_height[1];
      vpad[2] = NI_ALIGN (src_height[2], 2) - src_height[2];
      vpad[3] = 0;

      tenBit = true;
      break;
    case NI_PIX_FMT_NV12:
      hpad[0] =
          ni_max (dst_stride[0] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0), 0);
      hpad[1] =
          ni_max (dst_stride[1] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 1), 0);
      hpad[2] = 0;
      hpad[3] = 0;

      src_height[0] = frame->info.height;
      src_height[1] = NI_ALIGN (frame->info.height, 2) / 2;
      src_height[2] = 0;
      src_height[3] = 0;

      vpad[0] = NI_ALIGN (src_height[0], 2) - src_height[0];
      vpad[1] = NI_ALIGN (src_height[1], 2) - src_height[1];
      vpad[2] = 0;
      vpad[3] = 0;

      tenBit = false;
      break;
    case NI_PIX_FMT_P010LE:
      hpad[0] =
          ni_max (dst_stride[0] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0), 0);
      hpad[1] =
          ni_max (dst_stride[1] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 1), 0);
      hpad[2] = 0;
      hpad[3] = 0;

      src_height[0] = frame->info.height;
      src_height[1] = NI_ALIGN (frame->info.height, 2) / 2;
      src_height[2] = 0;
      src_height[3] = 0;

      vpad[0] = NI_ALIGN (src_height[0], 2) - src_height[0];
      vpad[1] = NI_ALIGN (src_height[1], 2) - src_height[1];
      vpad[2] = 0;
      vpad[3] = 0;

      tenBit = true;
      break;
    case NI_PIX_FMT_RGBA:
    case NI_PIX_FMT_BGRA:
    case NI_PIX_FMT_ARGB:
    case NI_PIX_FMT_ABGR:
    case NI_PIX_FMT_BGR0:
      hpad[0] =
          ni_max (dst_stride[0] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0), 0);
      hpad[1] = 0;
      hpad[2] = 0;
      hpad[3] = 0;

      src_height[0] = frame->info.height;
      src_height[1] = 0;
      src_height[2] = 0;
      src_height[3] = 0;

      vpad[0] = 0;
      vpad[1] = 0;
      vpad[2] = 0;
      vpad[3] = 0;

      tenBit = false;
      break;
    case NI_PIX_FMT_BGRP:
      hpad[0] =
          ni_max (dst_stride[0] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0), 0);
      hpad[1] =
          ni_max (dst_stride[1] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 1), 0);
      hpad[2] =
          ni_max (dst_stride[2] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 2), 0);
      hpad[3] = 0;

      src_height[0] = frame->info.height;
      src_height[1] = frame->info.height;
      src_height[2] = frame->info.height;
      src_height[3] = 0;

      vpad[0] = 0;
      vpad[1] = 0;
      vpad[2] = 0;
      vpad[3] = 0;

      tenBit = false;
      break;
    case NI_PIX_FMT_NV16:
      hpad[0] = 0;
      hpad[1] = 0;
      hpad[2] = 0;
      hpad[3] = 0;

      src_height[0] = frame->info.height;
      src_height[1] = frame->info.height;
      src_height[2] = 0;
      src_height[3] = 0;

      vpad[0] = NI_ALIGN (src_height[0], 2) - src_height[0];
      vpad[1] = NI_ALIGN (src_height[1], 2) - src_height[1];
      vpad[2] = 0;
      vpad[3] = 0;

      tenBit = false;
      break;
    case NI_PIX_FMT_YUYV422:
    case NI_PIX_FMT_UYVY422:
      hpad[0] =
          ni_max (dst_stride[0] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0), 0);;
      hpad[1] = 0;
      hpad[2] = 0;
      hpad[3] = 0;

      src_height[0] = frame->info.height;
      src_height[1] = 0;
      src_height[2] = 0;
      src_height[3] = 0;

      vpad[0] = 0;
      vpad[1] = 0;
      vpad[2] = 0;
      vpad[3] = 0;

      tenBit = false;
      break;
    case NI_PIX_FMT_8_TILED4X4:
    case NI_PIX_FMT_10_TILED4X4:
    case NI_PIX_FMT_NONE:
      break;
  }
  for (i = 0; i < nb_planes; i++) {
    dst_line = dst->p_data[i];
    src_line = GST_VIDEO_FRAME_PLANE_DATA (frame, i);

    for (h = 0; h < src_height[i]; h++) {
      memcpy (dst_line, src_line, ni_min (GST_VIDEO_FRAME_PLANE_STRIDE (frame,
                  i), dst_stride[i]));
      if (hpad[i]) {
        lastidx = GST_VIDEO_FRAME_PLANE_STRIDE (frame, i);

        if (tenBit) {
          sample = &src_line[lastidx - 2];
          dest = &dst_line[lastidx];

          /* two bytes per sample */
          for (j = 0; j < hpad[i] / 2; j++) {
            memcpy (dest, sample, 2);
            dest += 2;
          }
        } else {
          YUVsample = dst_line[lastidx - 1];
          memset (&dst_line[lastidx], YUVsample, hpad[i]);
        }
      }

      src_line += GST_VIDEO_FRAME_PLANE_STRIDE (frame, i);
      dst_line += dst_stride[i];
    }

    /* Extend the height by cloning the last line */
    src_line = dst_line - dst_stride[i];
    for (h = 0; h < vpad[i]; h++) {
      memcpy (dst_line, src_line, dst_stride[i]);
      dst_line += dst_stride[i];
    }
  }

  return 0;
}

void
gst_set_bit_depth_and_encoding_type (int8_t * p_bit_depth,
    int8_t * p_enc_type, GstVideoFormat pix_fmt)
{
  switch (pix_fmt) {
    case GST_VIDEO_FORMAT_I420:
      *p_bit_depth = 1;         // 8-bits per component
      *p_enc_type = 1;          // planar
      break;
    case GST_VIDEO_FORMAT_I420_10LE:
      *p_bit_depth = 2;         // 10-bits per component
      *p_enc_type = 1;          // planar
      break;
    case GST_VIDEO_FORMAT_NV12:
      *p_bit_depth = 1;         // 8-bits per component
      *p_enc_type = 0;          // semi-planar
      break;
    case GST_VIDEO_FORMAT_P010_10LE:
      *p_bit_depth = 2;         // 10-bits per component
      *p_enc_type = 0;          // semi-planar
      break;
    case GST_VIDEO_FORMAT_YUY2:
    case GST_VIDEO_FORMAT_UYVY:
      *p_bit_depth = 1;         // 8-bits per component
      *p_enc_type = 1;          // packed
      break;
    case GST_VIDEO_FORMAT_NV16:
      *p_bit_depth = 1;         // 8-bits per component
      *p_enc_type = 0;          // semi-planar
      break;
    case GST_VIDEO_FORMAT_BGRP:
    case GST_VIDEO_FORMAT_RGBA:
    case GST_VIDEO_FORMAT_BGRA:
    case GST_VIDEO_FORMAT_ABGR:
    case GST_VIDEO_FORMAT_ARGB:
    case GST_VIDEO_FORMAT_BGR:
      *p_bit_depth = 1;         // 8-bits per component
      *p_enc_type = 1;          // packed or planar
      break;
#if ENABLE_NI_GST_FRAMEWORK_PATCH
    case GST_VIDEO_FORMAT_NI_QUAD_8_4L4:
      *p_bit_depth = 1;         // 8-bits per component
      *p_enc_type = NI_PIXEL_PLANAR_FORMAT_TILED4X4;
      break;
#endif
    default:
      *p_bit_depth = 1;         // 8-bits per component
      *p_enc_type = 1;          // planar or packed
      break;
  }
}

void
ni_set_bit_depth_and_encoding_type (int8_t * p_bit_depth,
    int8_t * p_enc_type, ni_pix_fmt_t pix_fmt)
{
  switch (pix_fmt) {
    case NI_PIX_FMT_YUV420P:
      *p_bit_depth = 1;         // 8-bits per component
      *p_enc_type = 1;          // planar
      break;

    case NI_PIX_FMT_YUV420P10LE:
      *p_bit_depth = 2;         // 10-bits per component
      *p_enc_type = 1;          // planar
      break;

    case NI_PIX_FMT_NV12:
      *p_bit_depth = 1;         // 8-bits per component
      *p_enc_type = 0;          // semi-planar
      break;

    case NI_PIX_FMT_P010LE:
      *p_bit_depth = 2;         // 10-bits per component
      *p_enc_type = 0;          // semi-planar
      break;

    case NI_PIX_FMT_YUYV422:
    case NI_PIX_FMT_UYVY422:
      *p_bit_depth = 1;         // 8-bits per component
      *p_enc_type = 1;          // packed
      break;

    case NI_PIX_FMT_NV16:
      *p_bit_depth = 1;         // 8-bits per component
      *p_enc_type = 0;          // semi-planar
      break;

    case NI_PIX_FMT_BGRP:
    case NI_PIX_FMT_RGBA:
    case NI_PIX_FMT_BGRA:
    case NI_PIX_FMT_ABGR:
    case NI_PIX_FMT_ARGB:
    case NI_PIX_FMT_BGR0:
      *p_bit_depth = 1;         // 8-bits per component
      *p_enc_type = 1;          // packed or planar
      break;

    default:
      *p_bit_depth = 1;         // 8-bits per component
      *p_enc_type = 1;          // planar or packed
      break;
  }
}

gint32
calculateSwFrameSize (int width, int height, ni_pix_fmt_t pix_fmt)
{
  int32_t frameSize = 0;
  switch (pix_fmt) {
    case NI_PIX_FMT_YUV420P:
    case NI_PIX_FMT_NV12:
      frameSize = width * height * 3 / 2;
      break;
    case NI_PIX_FMT_YUV420P10LE:
    case NI_PIX_FMT_P010LE:
      frameSize = width * height * 3;
      break;
    case NI_PIX_FMT_RGBA:
    case NI_PIX_FMT_BGRA:
    case NI_PIX_FMT_ARGB:
    case NI_PIX_FMT_ABGR:
    case NI_PIX_FMT_BGR0:
      frameSize = width * height * 4;
      break;
    case NI_PIX_FMT_BGRP:
      frameSize = width * height * 3;
      break;
    case NI_PIX_FMT_NV16:
    case NI_PIX_FMT_YUYV422:
    case NI_PIX_FMT_UYVY422:
      frameSize = width * height * 2;
      break;
    case NI_PIX_FMT_8_TILED4X4:
    case NI_PIX_FMT_10_TILED4X4:
    case NI_PIX_FMT_NONE:
      break;
  }
  return frameSize;
}

static size_t
gst_strlcpy (char *dst, const char *src, size_t size)
{
  size_t len = 0;

  while (++len < size && *src)
    *dst++ = *src++;

  if (len <= size)
    *dst = 0;

  return len + strlen (src) - 1;
}

static inline int
gst_tolower (int c)
{
  if (c >= 'A' && c <= 'Z')
    c ^= 0x20;
  return c;
}

static int
gst_strcasecmp (const char *a, const char *b)
{
  uint8_t c1, c2;

  do {
    c1 = gst_tolower (*a++);
    c2 = gst_tolower (*b++);
  } while (c1 && c1 == c2);

  return c1 - c2;
}

static int
gst_color_table_compare (const void *lhs, const void *rhs)
{
  return gst_strcasecmp (lhs, ((const ColorEntry *) rhs)->name);
}

#define ALPHA_SEP '@'
NiPluginError
gst_parse_color (uint8_t * rgba_color, const char *color_string, int slen)
{
  char *tail, color_string2[128];
  const ColorEntry *entry;
  int len, hex_offset = 0;

  if (color_string[0] == '#') {
    hex_offset = 1;
  } else if (!strncmp (color_string, "0x", 2)) {
    hex_offset = 2;
  }

  if (slen < 0)
    slen = strlen (color_string);
  gst_strlcpy (color_string2, color_string + hex_offset,
      ni_min (slen - hex_offset + 1, sizeof (color_string2)));

  if ((tail = strchr (color_string2, ALPHA_SEP))) {
    *tail++ = 0;
  }

  len = strlen (color_string2);
  rgba_color[3] = 255;

  if (hex_offset || strspn (color_string2, "0123456789ABCDEFabcdef") == len) {
    char *tail;
    unsigned int rgba = strtoul (color_string2, &tail, 16);

    if (*tail || (len != 6 && len != 8)) {
      return NI_PLUGIN_EINVAL;
    }

    if (len == 8) {
      rgba_color[3] = rgba;
      rgba >>= 8;
    }

    rgba_color[0] = rgba >> 16;
    rgba_color[1] = rgba >> 8;
    rgba_color[2] = rgba;
  } else {
    entry = bsearch (color_string2,
        color_table,
        NI_ARRAY_ELEMS (color_table),
        sizeof (ColorEntry), gst_color_table_compare);
    if (!entry) {
      return NI_PLUGIN_EINVAL;
    }

    memcpy (rgba_color, entry->rgb_color, 3);
  }

  if (tail) {
    double alpha;
    const char *alpha_string = tail;

    if (!strncmp (alpha_string, "0x", 2)) {
      alpha = strtoul (alpha_string, &tail, 16);
    } else {
      double norm_alpha = strtod (alpha_string, &tail);
      if (norm_alpha < 0.0 || norm_alpha > 1.0) {
        alpha = 256;
      } else {
        alpha = 255 * norm_alpha;
      }
    }

    if (tail == alpha_string || *tail || alpha > 255 || alpha < 0) {
      return NI_PLUGIN_EINVAL;
    }

    rgba_color[3] = alpha;
  }

  return NI_PLUGIN_OK;
}

int64_t
gst_rescale_rnd (int64_t a, int64_t b, int64_t c, enum GSTRounding rnd)
{
  int64_t r = 0;
  assert (c > 0);
  assert (b >= 0);
  assert ((unsigned) (rnd & ~GST_ROUND_PASS_MINMAX) <= 5
      && (rnd & ~GST_ROUND_PASS_MINMAX) != 4);

  if (c <= 0 || b < 0 || !((unsigned) (rnd & ~GST_ROUND_PASS_MINMAX) <= 5
          && (rnd & ~GST_ROUND_PASS_MINMAX) != 4))
    return INT64_MIN;

  if (rnd & GST_ROUND_PASS_MINMAX) {
    if (a == INT64_MIN || a == INT64_MAX)
      return a;
    rnd -= GST_ROUND_PASS_MINMAX;
  }

  if (a < 0) {
    int64_t max_val = (a > -INT64_MAX) ? a : -INT64_MAX;
    return -(uint64_t) gst_rescale_rnd (-max_val, b, c, rnd ^ ((rnd >> 1) & 1));
  }

  if (rnd == GST_ROUND_NEAR_INF)
    r = c / 2;
  else if (rnd & 1)
    r = c - 1;

  if (b <= INT_MAX && c <= INT_MAX) {
    if (a <= INT_MAX)
      return (a * b + r) / c;
    else {
      int64_t ad = a / c;
      int64_t a2 = (a % c * b + r) / c;
      if (ad >= INT32_MAX && b && ad > (INT64_MAX - a2) / b)
        return INT64_MIN;
      return ad * b + a2;
    }
  } else {
    uint64_t a0 = a & 0xFFFFFFFF;
    uint64_t a1 = a >> 32;
    uint64_t b0 = b & 0xFFFFFFFF;
    uint64_t b1 = b >> 32;
    uint64_t t1 = a0 * b1 + a1 * b0;
    uint64_t t1a = t1 << 32;
    int i;

    a0 = a0 * b0 + t1a;
    a1 = a1 * b1 + (t1 >> 32) + (a0 < t1a);
    a0 += r;
    a1 += a0 < r;

    for (i = 63; i >= 0; i--) {
      a1 += a1 + ((a0 >> i) & 1);
      t1 += t1;
      if (c <= a1) {
        a1 -= c;
        t1++;
      }
    }
    if (t1 > INT64_MAX)
      return INT64_MIN;
    return t1;
  }
}

int64_t
gst_rescale (int64_t a, int64_t b, int64_t c)
{
  return gst_rescale_rnd (a, b, c, GST_ROUND_NEAR_INF);
}

int
gst_parse_aspect (gchar * aspect_str, gst_rational * aspect)
{
  int num, den;

  if (sscanf (aspect_str, "%d%*1[:/]%d", &num, &den) == 2) {
    aspect->num = num;
    aspect->den = den;

    return 0;
  }

  return -1;
}

int
gst_reduce (int *dst_num, int *dst_den, int64_t num, int64_t den, int64_t max)
{
  gst_rational a0 = { 0, 1 }, a1 = { 1, 0 };
  int sign = (num < 0) ^ (den < 0);

  int64_t gcd = gst_util_greatest_common_divisor (ABS (num), ABS (den));
  if (gcd) {
    num = ABS (num) / gcd;
    den = ABS (den) / gcd;
  }

  if (num <= max && den <= max) {
    a1 = (gst_rational) {
    num, den};
    den = 0;
  }

  while (den) {
    uint64_t x = num / den;
    int64_t next_den = num - den * x;
    int64_t a2n = x * a1.num + a0.num;
    int64_t a2d = x * a1.den + a0.den;

    if (a2n > max || a2d > max) {
      if (a1.num)
        x = (max - a0.num) / a1.num;
      if (a1.den)
        x = ni_min (x, (max - a0.den) / a1.den);

      if (den * (2 * x * a1.den + a0.den) > num * a1.den)
        a1 = (gst_rational) {
        x *a1.num + a0.num, x * a1.den + a0.den};
      break;
    }

    a0 = a1;
    a1 = (gst_rational) {
    a2n, a2d};
    num = den;
    den = next_den;
  }

  assert (gst_util_greatest_common_divisor (a1.num, a1.den) <= 1U);
  assert (a1.num <= max && a1.den <= max);

  *dst_num = sign ? -a1.num : a1.num;
  *dst_den = a1.den;

  return den == 0;
}

gst_rational
gst_mul_q (gst_rational b, gst_rational c)
{
  gst_reduce (&b.num, &b.den, b.num * (int64_t) c.num, b.den * (int64_t) c.den,
      INT_MAX);
  return b;
}

gst_rational
gst_div_q (gst_rational b, gst_rational c)
{
  return gst_mul_q (b, (gst_rational) {
      c.den, c.num}
  );
}

/* For color hdr metadata all the UNKNOWN case should make it to 2
 * which means UNSPECIFIED on libxcoder & ffmpeg
 * */

int
map_gst_color_primaries (GstVideoColorPrimaries primaries)
{
  switch (primaries) {
    case GST_VIDEO_COLOR_PRIMARIES_UNKNOWN:
      return 2;
    case GST_VIDEO_COLOR_PRIMARIES_BT709:
      return 1;
    case GST_VIDEO_COLOR_PRIMARIES_BT470M:
      return 4;
    case GST_VIDEO_COLOR_PRIMARIES_BT470BG:
      return 5;
    case GST_VIDEO_COLOR_PRIMARIES_SMPTE170M:
      return 6;
    case GST_VIDEO_COLOR_PRIMARIES_SMPTE240M:
      return 7;
    case GST_VIDEO_COLOR_PRIMARIES_FILM:
      return 8;
    case GST_VIDEO_COLOR_PRIMARIES_BT2020:
      return 9;
    case GST_VIDEO_COLOR_PRIMARIES_ADOBERGB:
      return 13;
    case GST_VIDEO_COLOR_PRIMARIES_SMPTEST428:
      return 10;
    case GST_VIDEO_COLOR_PRIMARIES_SMPTERP431:
      return 11;
    case GST_VIDEO_COLOR_PRIMARIES_SMPTEEG432:
      return 12;
    case GST_VIDEO_COLOR_PRIMARIES_EBU3213:
      return 22;
    default:
      return 2;
  }
}

int
map_gst_color_trc (GstVideoTransferFunction transferFunction)
{
  switch (transferFunction) {
    case GST_VIDEO_TRANSFER_UNKNOWN:
      return 2;
    case GST_VIDEO_TRANSFER_GAMMA10:
    case GST_VIDEO_TRANSFER_GAMMA18:
    case GST_VIDEO_TRANSFER_GAMMA20:
    case GST_VIDEO_TRANSFER_GAMMA22:
      return 4;
    case GST_VIDEO_TRANSFER_BT709:
      return 1;
    case GST_VIDEO_TRANSFER_SMPTE240M:
      return 7;
    case GST_VIDEO_TRANSFER_LOG100:
      return 9;
    case GST_VIDEO_TRANSFER_LOG316:
      return 10;
    case GST_VIDEO_TRANSFER_BT2020_12:
      return 15;
    case GST_VIDEO_TRANSFER_BT2020_10:
      return 14;
    case GST_VIDEO_TRANSFER_SMPTE2084:
      return 16;
    case GST_VIDEO_TRANSFER_ARIB_STD_B67:
      return 18;
    default:
      return 2;
  }
}

int
map_gst_color_space (GstVideoColorMatrix matrix)
{
  switch (matrix) {
    case GST_VIDEO_COLOR_MATRIX_UNKNOWN:
      return 2;
    case GST_VIDEO_COLOR_MATRIX_FCC:
      return 4;
    case GST_VIDEO_COLOR_MATRIX_BT709:
      return 1;
    case GST_VIDEO_COLOR_MATRIX_SMPTE240M:
      return 7;
    case GST_VIDEO_COLOR_MATRIX_BT2020:
      return 9;
    default:
      return 2;
  }
}

GstCaps *
remove_structures_from_caps (GstCaps * original_caps,
    const gchar * structure_name, guint start)
{
  guint caps_size = gst_caps_get_size (original_caps);
  if (start >= caps_size) {
    g_warning
        ("Start index %u is out of bounds (caps size: %u). No removal performed.",
        start, caps_size);
    return original_caps;
  }

  GstCaps *writable_caps = gst_caps_make_writable (original_caps);
  if (!writable_caps) {
    g_warning ("Failed to make GstCaps writable.");
    return original_caps;
  }

  for (guint i = start; i < gst_caps_get_size (writable_caps);) {
    const GstStructure *structure = gst_caps_get_structure (writable_caps, i);

    if (g_strcmp0 (gst_structure_get_name (structure), structure_name) == 0) {
      gst_caps_remove_structure (writable_caps, i);
      g_print ("Removed structure: %s at index %u\n", structure_name, i);
    } else {
      i++;
    }
  }

  return writable_caps;
}
