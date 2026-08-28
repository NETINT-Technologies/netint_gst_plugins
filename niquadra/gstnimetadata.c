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

#include "gstnimetadata.h"

GType
gst_video_hdr_plus_meta_api_get_type (void)
{
  static GType type = 0;

  if (g_once_init_enter (&type)) {
    static const gchar *tags[] = { NULL };
    GType _type = gst_meta_api_type_register ("GstVideoHdrPlusMetaAPI", tags);
    GST_DEBUG ("registering");
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
  GstVideoHdrPlusMeta *emeta = NULL;

  if (!meta) {
    return FALSE;
  }

  emeta = (GstVideoHdrPlusMeta *) meta;
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

#if GLIB_CHECK_VERSION(2, 67, 4)
  meta->data = g_memdup2 (data, size);
#else
  meta->data = g_memdup (data, size);
#endif
  meta->size = size;

  return meta;
}

GType
gst_netint_private_meta_api_get_type (void)
{
  static GType type = 0;

  if (g_once_init_enter (&type)) {
    static const gchar *tags[] = { NULL };
    GType _type =
        gst_meta_api_type_register ("GstVideoNetintPrivateMetaAPI", tags);
    GST_DEBUG ("registering");
    g_once_init_leave (&type, _type);
  }
  return type;
}

static gboolean
gst_netint_private_meta_transform (GstBuffer * dest,
    GstMeta * meta, GstBuffer * buffer, GQuark type, gpointer data)
{
  GstNetintPrivateMeta *dmeta, *smeta;

  smeta = (GstNetintPrivateMeta *) meta;

  dmeta = gst_buffer_add_netint_private_meta (dest, smeta->data, smeta->size);
  if (!dmeta)
    return FALSE;

  return TRUE;
}

static gboolean
gst_netint_private_meta_init (GstMeta * meta, gpointer params,
    GstBuffer * buffer)
{
  GstNetintPrivateMeta *emeta = NULL;

  if (!meta) {
    return FALSE;
  }

  emeta = (GstNetintPrivateMeta *) meta;
  emeta->data = NULL;
  emeta->size = 0;

  return TRUE;
}

static void
gst_netint_private_meta_free (GstMeta * meta, GstBuffer * buffer)
{
  GstNetintPrivateMeta *emeta = (GstNetintPrivateMeta *) meta;

  g_free (emeta->data);
}

const GstMetaInfo *
gst_netint_private_meta_get_info (void)
{
  static const GstMetaInfo *meta_info = NULL;

  if (g_once_init_enter ((GstMetaInfo **) & meta_info)) {
    const GstMetaInfo *mi = gst_meta_register (GST_NETINT_PRIVATE_META_API_TYPE,
        "GstNetintPrivateMeta",
        sizeof (GstNetintPrivateMeta),
        gst_netint_private_meta_init,
        gst_netint_private_meta_free,
        gst_netint_private_meta_transform);
    g_once_init_leave ((GstMetaInfo **) & meta_info, (GstMetaInfo *) mi);
  }
  return meta_info;
}

GstNetintPrivateMeta *
gst_buffer_add_netint_private_meta (GstBuffer * buffer,
    const guint8 * data, gsize size)
{
  GstNetintPrivateMeta *meta;

  g_return_val_if_fail (GST_IS_BUFFER (buffer), NULL);
  g_return_val_if_fail (data != NULL, NULL);
  g_return_val_if_fail (size > 0, NULL);

  meta = (GstNetintPrivateMeta *) gst_buffer_add_meta (buffer,
      GST_NETINT_PRIVATE_META_INFO, NULL);
  g_return_val_if_fail (meta != NULL, NULL);

#if GLIB_CHECK_VERSION(2, 67, 4)
  meta->data = g_memdup2 (data, size);
#else
  meta->data = g_memdup (data, size);
#endif
  meta->size = size;

  return meta;
}
