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

#include "gstnimetadata.h"

GType
gst_video_hdr_plus_meta_api_get_type (void)
{
  static volatile GType type = 0;

  if (g_once_init_enter (&type)) {
    static const gchar *tags[] = { NULL };
    GType _type = gst_meta_api_type_register ("GstVideoHdrPlusMetaAPI", tags);
    GST_INFO ("registering");
    g_once_init_leave (&type, _type);
  }
  return type;
}


static gboolean
gst_video_hdr_plus_meta_transform (GstBuffer * dest, GstMeta * meta,
    GstBuffer * buffer, GQuark type, gpointer data)
{
  GstVideoHdrPlusMeta *dmeta, *smeta;

  /* We always copy over the hdr_plus meta */
  smeta = (GstVideoHdrPlusMeta *) meta;

  dmeta = gst_buffer_add_video_hdr_plus_meta (dest, smeta->data, smeta->size);
  if (!dmeta)
    return FALSE;

  return TRUE;
}

static gboolean
gst_video_hdr_plus_meta_init (GstMeta * meta, gpointer params,
    GstBuffer * buffer)
{
  GstVideoHdrPlusMeta *emeta = (GstVideoHdrPlusMeta *) meta;

  emeta->data = NULL;
  emeta->size = 0;

  return TRUE;
}

static void
gst_video_hdr_plus_meta_free (GstMeta * meta, GstBuffer * buffer)
{
  GstVideoHdrPlusMeta *emeta = (GstVideoHdrPlusMeta *) meta;

  g_free (emeta->data);
}

const GstMetaInfo *
gst_video_hdr_plus_meta_get_info (void)
{
  static const GstMetaInfo *meta_info = NULL;

  if (g_once_init_enter ((GstMetaInfo **) & meta_info)) {
    const GstMetaInfo *mi = gst_meta_register (GST_VIDEO_HDR_PLUS_META_API_TYPE,
        "GstVideoHdrPlusMeta",
        sizeof (GstVideoHdrPlusMeta),
        gst_video_hdr_plus_meta_init,
        gst_video_hdr_plus_meta_free,
        gst_video_hdr_plus_meta_transform);
    g_once_init_leave ((GstMetaInfo **) & meta_info, (GstMetaInfo *) mi);
  }
  return meta_info;
}

GstVideoHdrPlusMeta *
gst_buffer_add_video_hdr_plus_meta (GstBuffer * buffer, const guint8 * data,
    gsize size)
{
  GstVideoHdrPlusMeta *meta;

  g_return_val_if_fail (GST_IS_BUFFER (buffer), NULL);
  g_return_val_if_fail (data != NULL, NULL);
  g_return_val_if_fail (size > 0, NULL);

  meta = (GstVideoHdrPlusMeta *) gst_buffer_add_meta (buffer,
      GST_VIDEO_HDR_PLUS_META_INFO, NULL);
  g_return_val_if_fail (meta != NULL, NULL);

  meta->data = g_memdup (data, size);
  meta->size = size;

  return meta;
}


GType
gst_video_user_data_unregistered_meta_api_get_type (void)
{
  static volatile GType type = 0;

  if (g_once_init_enter (&type)) {
    static const gchar *tags[] = { NULL };
    GType _type = gst_meta_api_type_register ("GstVideoUDUMetaAPI", tags);
    GST_INFO ("registering");
    g_once_init_leave (&type, _type);
  }
  return type;
}


static gboolean
gst_video_user_data_unregistered_meta_transform (GstBuffer * dest,
    GstMeta * meta, GstBuffer * buffer, GQuark type, gpointer data)
{
  GstVideoUDUMeta *dmeta, *smeta;

  /* We always copy over the user_data_unregistered meta */
  smeta = (GstVideoUDUMeta *) meta;

  dmeta =
      gst_buffer_add_video_user_data_unregistered_meta (dest, smeta->data,
      smeta->size);
  if (!dmeta)
    return FALSE;

  return TRUE;
}

static gboolean
gst_video_user_data_unregistered_meta_init (GstMeta * meta, gpointer params,
    GstBuffer * buffer)
{
  GstVideoUDUMeta *emeta = (GstVideoUDUMeta *) meta;

  emeta->data = NULL;
  emeta->size = 0;

  return TRUE;
}

static void
gst_video_user_data_unregistered_meta_free (GstMeta * meta, GstBuffer * buffer)
{
  GstVideoUDUMeta *emeta = (GstVideoUDUMeta *) meta;

  g_free (emeta->data);
}

const GstMetaInfo *
gst_video_user_data_unregistered_meta_get_info (void)
{
  static const GstMetaInfo *meta_info = NULL;

  if (g_once_init_enter ((GstMetaInfo **) & meta_info)) {
    const GstMetaInfo *mi = gst_meta_register (GST_VIDEO_UDU_META_API_TYPE,
        "GstVideoUDUMeta",
        sizeof (GstVideoUDUMeta),
        gst_video_user_data_unregistered_meta_init,
        gst_video_user_data_unregistered_meta_free,
        gst_video_user_data_unregistered_meta_transform);
    g_once_init_leave ((GstMetaInfo **) & meta_info, (GstMetaInfo *) mi);
  }
  return meta_info;
}

GstVideoUDUMeta *
gst_buffer_add_video_user_data_unregistered_meta (GstBuffer * buffer,
    const guint8 * data, gsize size)
{
  GstVideoUDUMeta *meta;

  g_return_val_if_fail (GST_IS_BUFFER (buffer), NULL);
  g_return_val_if_fail (data != NULL, NULL);
  g_return_val_if_fail (size > 0, NULL);

  meta = (GstVideoUDUMeta *) gst_buffer_add_meta (buffer,
      GST_VIDEO_UDU_META_INFO, NULL);
  g_return_val_if_fail (meta != NULL, NULL);

  meta->data = g_memdup (data, size);
  meta->size = size;

  return meta;
}
