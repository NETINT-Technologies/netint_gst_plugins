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
 *  \file   gstniquadrautils.h
 *
 *  \brief  Header of NetInt Quadra common function.
 ******************************************************************************/
#ifndef _GST_NIQUADRA_UTILS_H
#define _GST_NIQUADRA_UTILS_H

#include <gst/gst.h>
#include "ni_device_api.h"
#include "ni_util.h"
#include "gst/video/video-format.h"
#include "gst/video/video-frame.h"

#define NI_ALIGN(x, a) (((x)+(a)-1)&~((a)-1))
#define NI_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))

#define PIX_FMT_NI_QUADRA 118
#define DEFAULT_NI_FILTER_POOL_SIZE 4

#define NI_PREALLOCATE_STRUCTURE_NAME   "Ni-reallocate"
#define NI_VIDEO_META_BUFCNT            "buffercnt"
#define NI_VIDEO_META_CARDNO            "cardno"

typedef enum {
  NI_PLUGIN_OK          = 0,
  NI_PLUGIN_FAILURE     = -1,
  NI_PLUGIN_EAGAIN      = -2,
  NI_PLUGIN_EOF         = -3,
  NI_PLUGIN_ENOMEM      = -4,
  NI_PLUGIN_EIO         = -5,
  NI_PLUGIN_EINVAL      = -6,
  NI_PLUGIN_UNSUPPORTED = -7,
  NI_PLUGIN_UNKNOWN_ERR = -8,
} NiPluginError;

#define NISWAP(type, a, b) do{type SWAP_tmp= b; b= a; a= SWAP_tmp;}while(0)

typedef struct _gst_rational
{
  int num;
  int den;
} gst_rational;

typedef struct NIFramesContext
{
  niFrameSurface1_t *surfaces_internal;
  int nb_surfaces_used;
  niFrameSurface1_t **surface_ptrs;
  ni_session_context_t api_ctx;                 //for down/uploading frames
  ni_session_data_io_t src_session_io_data;     //for upload frame to be sent up
  ni_split_context_t split_ctx;
  ni_device_handle_t suspended_device_handle;
  int uploader_device_id;                       //same one passed to libxcoder session open
} NIFramesContext;

typedef struct NIDeviceContext
{
  int uploader_ID;

  ni_device_handle_t cards[NI_MAX_DEVICE_CNT];
} NIDeviceContext;

NiPluginError gst_ni_retrieve_gop_params (char gopParams[],
                                          ni_xcoder_params_t *params);

int ni_build_frame_pool (ni_session_context_t *ctx, int width, int height,
                        GstVideoFormat out_format, int pool_size);

GstVideoFormat convertNIPixToGstVideoFormat (ni_pix_fmt_t ni_pix_fmt);

ni_pix_fmt_t convertGstVideoFormatToNIPix (GstVideoFormat gstVideoFormat);

gint convertNIPixToGC620Format (ni_pix_fmt_t ni_pix_fmt);

gint convertGstVideoFormatToGC620Format (GstVideoFormat gstVideoFormat);

ni_pix_fmt_t convertGstVideoFormatToXcoderPixFmt (GstVideoFormat gstVideoFormat);

gboolean gst_image_fill_linesizes (int linesize[4], ni_pix_fmt_t format, int width, int align);

NiPluginError copy_ni_to_gst_memory (const ni_frame_t * src, uint8_t * dst,
    GstVideoInfo *info);

NiPluginError copy_ni_to_gst_frame (const ni_frame_t *src, GstVideoFrame *dst,
                                    ni_pix_fmt_t niPixFmt);

int copy_gst_to_ni_frame (const int dst_stride[4], ni_frame_t *dst,
                          GstVideoFrame *frame);

void gst_set_bit_depth_and_encoding_type (int8_t *p_bit_depth, int8_t *p_enc_type,
                                          GstVideoFormat pix_fmt);

void ni_set_bit_depth_and_encoding_type (int8_t *p_bit_depth, int8_t *p_enc_type,
                                         ni_pix_fmt_t pix_fmt);

gint32 calculateSwFrameSize (int width, int height, ni_pix_fmt_t pix_fmt);

NiPluginError gst_parse_color (uint8_t *rgba_color, const char *color_string,
                               int slen);

int64_t gst_rescale (int64_t a, int64_t b, int64_t c);

int gst_parse_aspect (gchar *aspect_str, gst_rational *aspect);

gst_rational gst_div_q (gst_rational b, gst_rational c);

int map_gst_color_primaries (GstVideoColorPrimaries primaries);

int map_gst_color_trc (GstVideoTransferFunction transferFunction);

int map_gst_color_space (GstVideoColorMatrix matrix);

GstCaps * remove_structures_from_caps (GstCaps *original_caps,
                                       const gchar *structure_name,
                                       guint start);

#endif //_GST_NIQUADRA_UTILS_H
