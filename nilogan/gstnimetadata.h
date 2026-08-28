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


#ifndef __GST_NI_METADATA_META_H__
#define __GST_NI_METADATA_META_H__
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

typedef struct
{
  GstMeta meta;

  guint8 *data;
  gsize size;
} GstVideoHdrPlusMeta;

GType gst_video_hdr_plus_meta_api_get_type (void);
#define GST_VIDEO_HDR_PLUS_META_API_TYPE (gst_video_hdr_plus_meta_api_get_type())

const GstMetaInfo *gst_video_hdr_plus_meta_get_info (void);
#define GST_VIDEO_HDR_PLUS_META_INFO (gst_video_hdr_plus_meta_get_info())

#define gst_buffer_get_video_hdr_plus_meta(b) \
        ((GstHdrPlusCaptionMeta*)gst_buffer_get_meta((b),GST_VIDEO_HDR_PLUS_META_API_TYPE))

GstVideoHdrPlusMeta *gst_buffer_add_video_hdr_plus_meta (GstBuffer * buffer,
    const guint8 * data, gsize size);

typedef struct
{
  GstMeta meta;

  guint8 *data;
  gsize size;
} GstVideoUDUMeta;

GType gst_video_user_data_unregistered_meta_api_get_type (void);
#define GST_VIDEO_UDU_META_API_TYPE (gst_video_user_data_unregistered_meta_api_get_type())

const GstMetaInfo *gst_video_user_data_unregistered_meta_get_info (void);
#define GST_VIDEO_UDU_META_INFO (gst_video_user_data_unregistered_meta_get_info())

#define gst_buffer_get_video_user_data_unregistered_meta(b) \
        ((GstVideoUDUMeta*)gst_buffer_get_meta((b),GST_VIDEO_UDU_META_API_TYPE))

GstVideoUDUMeta *gst_buffer_add_video_user_data_unregistered_meta (GstBuffer *
    buffer, const guint8 * data, gsize size);

#endif /* __GST_NI_METADATA_META_H__ */
