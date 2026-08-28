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
 *  \file   gstniquadrahwdownload.c
 *
 *  \brief  Implement of NetInt Quadra hardware download filter.
 ******************************************************************************/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/video.h>
#include "niquadra.h"
#include "ni_device_api.h"
#include "gstniquadramemory.h"
#include "gstniquadrautils.h"

GST_DEBUG_CATEGORY_STATIC (gst_niquadrahwdownload_debug);
#define GST_CAT_DEFAULT gst_niquadrahwdownload_debug

typedef struct _GstNiQuadraHWDownload
{
  GstElement element;

  GstPad *sinkpad, *srcpad;

  gint width, height;
  gboolean pass_mode;

  GstVideoInfo info;

  int last_width, last_height;
} GstNiQuadraHWDownload;

typedef struct _GstNiQuadraHWDownloadClass
{
  GstElementClass parent_class;
} GstNiQuadraHWDownloadClass;

#define GST_TYPE_NIQUADRAHWDOWNLOAD \
  (gst_niquadrahwdownload_get_type())
#define GST_NIQUADRAHWDOWNLOAD(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRAHWDOWNLOAD,GstNiQuadraHWDownload))
#define GST_NIQUADRAHWDOWNLOAD_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRAHWDOWNLOAD,GstNiQuadraHWDownload))
#define GST_IS_NIQUADRAHWDOWNLOAD(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRAHWDOWNLOAD))
#define GST_IS_NIQUADRAHWDOWNLOAD_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRAHWDOWNLOAD))

static gboolean niquadrahwdownload_element_init (GstPlugin * plugin);

GType gst_niquadrahwdownloadniquadra_get_type (void);

G_DEFINE_TYPE (GstNiQuadraHWDownload, gst_niquadrahwdownload, GST_TYPE_ELEMENT);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadrahwdownload,
    niquadrahwdownload_element_init);

#define SUPPORTED_FORMATS \
    "{ I420, YUY2, UYVY, NV12, ARGB, RGBA, ABGR, BGRA, I420_10LE, P010_10LE, " \
    "NV16, BGRx, NV12_10LE32 }"

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY, SUPPORTED_FORMATS))
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (SUPPORTED_FORMATS))
    );

static GstFlowReturn gst_niquadra_hw_download_chain (GstPad * pad,
    GstObject * parent, GstBuffer * inbuf);

static gboolean
gst_niquadra_hw_download_sink_setcaps (GstPad * pad, GstObject * parent,
    GstCaps * caps)
{
  GstNiQuadraHWDownload *filter = GST_NIQUADRAHWDOWNLOAD (parent);
  GstStructure *structure = NULL;
  GstCapsFeatures *features = NULL;
  GstCaps *src_caps = NULL;
  gboolean ret;

  structure = gst_caps_get_structure (caps, 0);
  if (!gst_structure_get_int (structure, "width", &filter->width))
    return FALSE;
  if (!gst_structure_get_int (structure, "height", &filter->height))
    return FALSE;

  if (!gst_video_info_from_caps (&filter->info, caps))
    return FALSE;

  src_caps = gst_caps_copy (caps);
  features = gst_caps_get_features (src_caps, 0);
  if (gst_caps_features_contains (features,
          GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY)) {
    gst_caps_features_remove (features,
        GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY);
    filter->pass_mode = FALSE;
  } else {
    filter->pass_mode = TRUE;
  }

  ret = gst_pad_set_caps (filter->srcpad, src_caps);
  gst_caps_unref (src_caps);

  return ret;
}

static gboolean
gst_niquadra_hw_download_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstNiQuadraHWDownload *filter = GST_NIQUADRAHWDOWNLOAD (parent);
  gboolean ret = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret = gst_niquadra_hw_download_sink_setcaps (pad, parent, caps);
      gst_event_unref (event);
      break;
    }
    default:
    {
      ret = gst_pad_push_event (filter->srcpad, event);
      break;
    }
  }

  return ret;
}

static void
gst_niquadrahwdownload_init (GstNiQuadraHWDownload * filter)
{
  filter->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_event_function (filter->sinkpad,
      gst_niquadra_hw_download_sink_event);
  gst_pad_set_chain_function (filter->sinkpad, gst_niquadra_hw_download_chain);
  gst_element_add_pad (GST_ELEMENT (filter), filter->sinkpad);

  filter->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  gst_element_add_pad (GST_ELEMENT (filter), filter->srcpad);

  gst_video_info_init (&filter->info);
  filter->last_width = -1;
  filter->last_height = -1;
  filter->pass_mode = FALSE;
}

static void
gst_niquadrahwdownload_dispose (GObject * obj)
{
  G_OBJECT_CLASS (gst_niquadrahwdownload_parent_class)->dispose (obj);
}

static GstFlowReturn
gst_niquadra_hw_download_chain (GstPad * pad, GstObject * parent,
    GstBuffer * inbuf)
{
  GstNiQuadraHWDownload *filter = GST_NIQUADRAHWDOWNLOAD (parent);
  GstVideoFrame dframe;
  GstMemory *in_mem = NULL;
  GstBuffer *outbuf = NULL;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  ni_session_data_io_t output;
  ni_session_context_t *p_session = NULL;
  niFrameSurface1_t *surface = NULL;
  ni_pix_fmt_t pix_fmt;
  int retval = 0;

  // If input frame is hwframe, don't need to upload it.
  if (filter->pass_mode) {
    flow_ret = gst_pad_push (filter->srcpad, inbuf);
    return flow_ret;
  }

  memset (&dframe, 0, sizeof (GstVideoFrame));
  memset (&output, 0, sizeof (ni_session_data_io_t));

  in_mem = gst_buffer_peek_memory (inbuf, 0);
  if (!gst_is_niquadra_memory (in_mem)) {
    GST_ERROR_OBJECT (filter,
        "Impossible to convert between the formats supported by the filter");
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  p_session = gst_session_from_ni_hw_memory (in_mem);
  surface = gst_surface_from_ni_hw_memory (in_mem);
  if (!p_session || !surface) {
    GST_ERROR_OBJECT (filter, "Failed to get Quadra hardware frame context");
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  outbuf = gst_buffer_new_and_alloc (filter->info.size);
  if (outbuf == NULL) {
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }
  gst_buffer_memset (outbuf, 0, 0, filter->info.size);

  if (!gst_video_frame_map (&dframe, &filter->info, outbuf, GST_MAP_WRITE)) {
    gst_buffer_unref (outbuf);
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  pix_fmt = convertGstVideoFormatToNIPix (dframe.info.finfo->format);
  retval = ni_frame_buffer_alloc_dl (&output.data.frame, filter->width,
      filter->height, pix_fmt);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "Failed to allocate download frame %d", retval);
    gst_video_frame_unmap (&dframe);
    gst_buffer_unref (outbuf);
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  p_session->is_auto_dl = false;
  retval = ni_device_session_hwdl (p_session, &output, surface);
  if (retval <= 0) {
    GST_ERROR_OBJECT (filter, "Failed to retrieve hardware frame");
    ni_frame_buffer_free (&output.data.frame);
    gst_video_frame_unmap (&dframe);
    gst_buffer_unref (outbuf);
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  if (copy_ni_to_gst_frame (&output.data.frame, &dframe, pix_fmt) !=
      NI_PLUGIN_OK) {
    GST_ERROR_OBJECT (filter, "Failed to copy downloaded frame");
    ni_frame_buffer_free (&output.data.frame);
    gst_video_frame_unmap (&dframe);
    gst_buffer_unref (outbuf);
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }
  ni_frame_buffer_free (&output.data.frame);

  gst_video_frame_unmap (&dframe);

  gst_buffer_copy_into (outbuf, inbuf,
      GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_META, 0, -1);
  gst_buffer_unref (inbuf);
  flow_ret = gst_pad_push (filter->srcpad, outbuf);

  return flow_ret;
}

static void
gst_niquadrahwdownload_class_init (GstNiQuadraHWDownloadClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_static_pad_template (element_class, &src_factory);
  gst_element_class_add_static_pad_template (element_class, &sink_factory);

  gst_element_class_set_static_metadata (element_class,
      "NETINT Quadra HWDOWNLOAD filter", "Filter/Effect/Video/HWDownload",
      "Download Netint Quadra HW AVFrame to SW",
      "Simon Shi<simon.shi@netint.cn>");

  gobject_class->dispose = gst_niquadrahwdownload_dispose;
}

static gboolean
niquadrahwdownload_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadrahwdownload_debug, "niquadrahwdownload",
      0, "niquadrahwdownload");

  return gst_element_register (plugin, "niquadrahwdownload", GST_RANK_NONE,
      GST_TYPE_NIQUADRAHWDOWNLOAD);
}
