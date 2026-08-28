/*******************************************************************************
 *
 * Copyright (C) 2024 NETINT Technologies
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
 *  \file   gstniloganutils.h
 *
 *  \brief  Header of NetInt Logan common function.
 ******************************************************************************/
#ifndef _GST_NILOGAN_UTILS_H
#define _GST_NILOGAN_UTILS_H

#include <gst/gst.h>
#include "ni_device_api_logan.h"
#include "ni_util_logan.h"
#include "gst/video/video-format.h"
#include "gst/video/video-frame.h"

#define NI_ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define NI_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))

#define PIX_FMT_NI_LOGAN 197


#define NI_EOF -3
#define NI_IO_ERR -8
#define NI_EAGAIN -11
#define NI_MEM_ERR -12
#define	NI_EINVAL -22           /* Invalid argument */

#define NISWAP(type,a,b) do{type SWAP_tmp= b; b= a; a= SWAP_tmp;}while(0)

#define gst_ni_max(a, b) ((a) > (b) ? (a) : (b))
#define gst_ni_min(a, b) ((a) < (b) ? (a) : (b))

gint gst_ni_retrieve_xcoder_params (ni_logan_session_context_t * ctx,
    const char xcoderParams[], ni_logan_encoder_params_t * params);

gint gst_ni_retrieve_gop_params (const char gopParams[],
    ni_logan_encoder_params_t * params);

GstVideoFormat convertNIPixToGstVideoFormat (ni_logan_pix_fmt_t ni_pix_fmt);

ni_logan_pix_fmt_t convertGstVideoFormatToNIPix (GstVideoFormat gstVideoFormat);

int copy_ni_to_gst_frame (const ni_logan_frame_t * src, GstVideoFrame * dst,
    ni_logan_pix_fmt_t niPixFmt);

int copy_gst_to_ni_frame (const int dst_stride[4], ni_logan_frame_t * dst,
    GstVideoFrame * frame);

void ni_set_bit_depth_and_encoding_type (int8_t * p_bit_depth,
    int8_t * p_enc_type, ni_logan_pix_fmt_t pix_fmt);

gint32 calculateSwFrameSize (int width, int height, ni_logan_pix_fmt_t pix_fmt);

int map_gst_color_primaries (GstVideoColorPrimaries primaries);

int map_gst_color_trc (GstVideoTransferFunction transferFunction);

int map_gst_color_space (GstVideoColorMatrix matrix);

#endif //_GST_NILOGAN_UTILS_H
