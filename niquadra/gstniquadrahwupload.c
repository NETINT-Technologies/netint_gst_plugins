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
 *  \file   gstniquadrahwupload.c
 *
 *  \brief  Implement of NetInt Quadra hardware upload filter.
 ******************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/video.h>
#include "niquadra.h"
#include "ni_device_api.h"
#include "gstniquadramemory.h"
#include "gstniquadrautils.h"

GST_DEBUG_CATEGORY_STATIC (gst_niquadrahwupload_debug);
#define GST_CAT_DEFAULT gst_niquadrahwupload_debug

enum
{
  PROP_0,
  PROP_DEVICE,
  PROP_KEEP_ALIVE_TIMEOUT,
  PROP_LAST
};

typedef struct _GstNiQuadraHWUpload
{
  GstElement element;

  GstPad *sinkpad, *srcpad;

  gint width, height;
  GstVideoFormat format;
  gint device;
  gboolean pass_mode;

  guint keep_alive_timeout;

  GstVideoInfo info;

  ni_session_context_t api_ctx;
  ni_session_data_io_t src_session_io_data;

  int last_width, last_height;
} GstNiQuadraHWUpload;

typedef struct _GstNiQuadraHWUploadClass
{
  GstElementClass parent_class;
} GstNiQuadraHWUploadClass;

#define GST_TYPE_NIQUADRAHWUPLOAD \
  (gst_niquadrahwupload_get_type())
#define GST_NIQUADRAHWUPLOAD(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRAHWUPLOAD,GstNiQuadraHWUpload))
#define GST_NIQUADRAHWUPLOAD_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRAHWUPLOAD,GstNiQuadraHWUpload))
#define GST_IS_NIQUADRAHWUPLOAD(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRAHWUPLOAD))
#define GST_IS_NIQUADRAHWUPLOAD_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRAHWUPLOAD))

static gboolean niquadrahwupload_element_init (GstPlugin * plugin);

GType gst_niquadrahwuploadniquadra_get_type (void);

G_DEFINE_TYPE (GstNiQuadraHWUpload, gst_niquadrahwupload, GST_TYPE_ELEMENT);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadrahwupload,
    niquadrahwupload_element_init);

#define SUPPORTED_FORMATS \
    "{ I420, YUY2, UYVY, NV12, ARGB, RGBA, ABGR, BGRA, I420_10LE, P010_10LE, " \
    "NV16, BGRx, NV12_10LE32 }"

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (SUPPORTED_FORMATS) "; "
        GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY, SUPPORTED_FORMATS))
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY, SUPPORTED_FORMATS))
    );

static GstFlowReturn gst_niquadra_hw_upload_chain (GstPad * pad,
    GstObject * parent, GstBuffer * inbuf);

static gboolean
gst_niquadra_hw_upload_sink_setcaps (GstPad * pad, GstObject * parent,
    GstCaps * caps)
{
  GstNiQuadraHWUpload *filter = GST_NIQUADRAHWUPLOAD (parent);
  GstStructure *structure = NULL;
  GstCapsFeatures *features = NULL;
  GstCaps *src_caps = NULL;
  gboolean res = FALSE;
  int retval = 0;

  structure = gst_caps_get_structure (caps, 0);
  if (!gst_structure_get_int (structure, "width", &filter->width))
    return FALSE;
  if (!gst_structure_get_int (structure, "height", &filter->height))
    return FALSE;

  features = gst_caps_get_features (caps, 0);
  if (gst_caps_features_contains (features,
          GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY)) {
    filter->pass_mode = TRUE;
  } else {
    filter->pass_mode = FALSE;
  }

  if (!gst_video_info_from_caps (&filter->info, caps))
    return FALSE;

  src_caps = gst_caps_copy (caps);
  gst_caps_set_features_simple (src_caps,
      gst_caps_features_from_string (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY));

  ni_device_session_context_init (&filter->api_ctx);

  filter->api_ctx.session_id = NI_INVALID_SESSION_ID;
  filter->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
  filter->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;
  filter->api_ctx.hw_id = filter->device;
  filter->api_ctx.keep_alive_timeout = filter->keep_alive_timeout;
  ni_pix_fmt_t fmt = convertGstVideoFormatToNIPix (filter->info.finfo->format);

  filter->api_ctx.active_video_width = filter->width;
  filter->api_ctx.active_video_height = filter->height;

  if (fmt != NI_PIX_FMT_NONE) {
    filter->api_ctx.pixel_format = fmt;
    switch (fmt) {
      case NI_PIX_FMT_YUV420P:
        filter->api_ctx.bit_depth_factor = 1;
        filter->api_ctx.src_bit_depth = 8;
        break;
      case NI_PIX_FMT_YUV420P10LE:
        filter->api_ctx.bit_depth_factor = 2;
        filter->api_ctx.src_bit_depth = 10;
        filter->api_ctx.src_endian = NI_FRAME_LITTLE_ENDIAN;
        break;
      case NI_PIX_FMT_NV12:
        filter->api_ctx.bit_depth_factor = 1;
        filter->api_ctx.src_bit_depth = 8;
        break;
      case NI_PIX_FMT_P010LE:
        filter->api_ctx.bit_depth_factor = 2;
        filter->api_ctx.src_bit_depth = 10;
        filter->api_ctx.src_endian = NI_FRAME_LITTLE_ENDIAN;
        break;
      case NI_PIX_FMT_RGBA:
        filter->api_ctx.bit_depth_factor = 4;
        filter->api_ctx.src_bit_depth = 32;
        filter->api_ctx.src_endian = NI_FRAME_LITTLE_ENDIAN;
        break;
      case NI_PIX_FMT_BGRA:
      case NI_PIX_FMT_ARGB:
      case NI_PIX_FMT_ABGR:
      case NI_PIX_FMT_BGR0:
        filter->api_ctx.bit_depth_factor = 4;
        filter->api_ctx.src_bit_depth = 32;
        filter->api_ctx.src_endian = NI_FRAME_LITTLE_ENDIAN;
        break;
      case NI_PIX_FMT_BGRP:
        filter->api_ctx.bit_depth_factor = 1;
        filter->api_ctx.src_bit_depth = 24;
        filter->api_ctx.src_endian = NI_FRAME_LITTLE_ENDIAN;
        break;
      case NI_PIX_FMT_NV16:
        filter->api_ctx.bit_depth_factor = 1;
        filter->api_ctx.src_bit_depth = 8;
        filter->api_ctx.src_endian = NI_FRAME_LITTLE_ENDIAN;
        break;
      case NI_PIX_FMT_YUYV422:
        filter->api_ctx.bit_depth_factor = 1;
        filter->api_ctx.src_bit_depth = 8;
        filter->api_ctx.src_endian = NI_FRAME_LITTLE_ENDIAN;
        break;
      case NI_PIX_FMT_UYVY422:
        filter->api_ctx.bit_depth_factor = 1;
        filter->api_ctx.src_bit_depth = 8;
        filter->api_ctx.src_endian = NI_FRAME_LITTLE_ENDIAN;
        break;
      default:
        GST_ERROR_OBJECT (filter,
            "Pixel format %d not supported by device.\n", fmt);
        return FALSE;
    }
  } else {
    return FALSE;
  }

  retval = ni_device_session_open (&filter->api_ctx, NI_DEVICE_TYPE_UPLOAD);
  if (retval < 0) {
    GST_ERROR_OBJECT (filter, "Open uploader session error\n");
    ni_device_session_close (&filter->api_ctx, 1, NI_DEVICE_TYPE_UPLOAD);
    ni_device_session_context_clear (&filter->api_ctx);
    return FALSE;
  } else {
    GST_DEBUG_OBJECT (filter,
        "XCoder %s.%d (inst: %d) opened successfully\n",
        filter->api_ctx.dev_xcoder_name, filter->api_ctx.hw_id,
        filter->api_ctx.session_id);
  }

  memset (&filter->src_session_io_data, 0, sizeof (ni_session_data_io_t));
  retval = ni_device_session_init_framepool (&filter->api_ctx,
      DEFAULT_NI_FILTER_POOL_SIZE, 0);
  if (retval < 0) {
    GST_ERROR_OBJECT (filter, "Init frame pool error\n");
    ni_device_session_context_clear (&filter->api_ctx);
    return FALSE;
  }

  res = gst_pad_set_caps (filter->srcpad, src_caps);
  gst_caps_unref (src_caps);

  return res;
}

static gboolean
gst_niquadra_hw_upload_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstNiQuadraHWUpload *filter = GST_NIQUADRAHWUPLOAD (parent);
  gboolean ret = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret = gst_niquadra_hw_upload_sink_setcaps (pad, parent, caps);
      gst_event_unref (event);
      break;
    }
    default:
      ret = gst_pad_push_event (filter->srcpad, event);
      break;
  }

  return ret;
}

static void
gst_niquadrahwupload_init (GstNiQuadraHWUpload * filter)
{
  filter->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_event_function (filter->sinkpad,
      gst_niquadra_hw_upload_sink_event);
  gst_pad_set_chain_function (filter->sinkpad, gst_niquadra_hw_upload_chain);
  gst_element_add_pad (GST_ELEMENT (filter), filter->sinkpad);

  filter->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  gst_element_add_pad (GST_ELEMENT (filter), filter->srcpad);

  gst_video_info_init (&filter->info);
  filter->pass_mode = FALSE;
  filter->keep_alive_timeout = NI_DEFAULT_KEEP_ALIVE_TIMEOUT;
  filter->last_width = -1;
  filter->last_height = -1;
  filter->device = -1;
}

static void
gst_niquadrahwupload_dispose (GObject * obj)
{
  GstNiQuadraHWUpload *filter = GST_NIQUADRAHWUPLOAD (obj);
  if (filter->src_session_io_data.data.frame.p_buffer) {
    ni_frame_buffer_free (&filter->src_session_io_data.data.frame);
  }
  if (filter->api_ctx.session_id != NI_INVALID_SESSION_ID) {
    GST_DEBUG_OBJECT (filter, "libxcoder uploader free context");
    ni_device_session_close (&filter->api_ctx, 1, NI_DEVICE_TYPE_UPLOAD);
  }
  ni_session_context_t *p_ctx = &filter->api_ctx;
  if (p_ctx) {
    if (p_ctx->device_handle != NI_INVALID_DEVICE_HANDLE) {
      ni_device_close (p_ctx->device_handle);
      p_ctx->device_handle = NI_INVALID_DEVICE_HANDLE;
    }
    if (p_ctx->blk_io_handle != NI_INVALID_DEVICE_HANDLE) {
      ni_device_close (p_ctx->blk_io_handle);
      p_ctx->blk_io_handle = NI_INVALID_DEVICE_HANDLE;
    }
  }
  ni_device_session_context_clear (&filter->api_ctx);

  G_OBJECT_CLASS (gst_niquadrahwupload_parent_class)->dispose (obj);
}

static GstFlowReturn
gst_niquadra_hw_upload_chain (GstPad * pad, GstObject * parent,
    GstBuffer * inbuf)
{
  GstNiQuadraHWUpload *filter = GST_NIQUADRAHWUPLOAD (parent);
  GstAllocator *alloc = NULL;
  GstMemory *out_mem = NULL;
  GstBuffer *outbuf = NULL;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  int retval = 0;
  int align = 0;
  int dst_stride[4] = { 0 };
  bool isSemiPlanar = false;
  ni_session_data_io_t *p_src_session_data;
  ni_session_data_io_t dst_session_data;
  ni_session_data_io_t *p_dst_session_data = &dst_session_data;

  p_src_session_data = &filter->src_session_io_data;

  // If input frame is hwframe, don't need to upload it.
  if (filter->pass_mode) {
    flow_ret = gst_pad_push (filter->srcpad, inbuf);
    return flow_ret;
  }

  GstVideoFrame vframe;
  gst_video_frame_map (&vframe, &filter->info, inbuf, GST_MAP_READ);

  ni_pix_fmt_t niPixFmt =
      convertGstVideoFormatToNIPix (vframe.info.finfo->format);

  switch (niPixFmt) {
    case NI_PIX_FMT_YUV420P:
    case NI_PIX_FMT_YUV420P10LE:
      align = 128;
      isSemiPlanar = false;
      break;
    case NI_PIX_FMT_NV12:
    case NI_PIX_FMT_P010LE:
      align = 128;
      isSemiPlanar = true;
      break;
    case NI_PIX_FMT_RGBA:
    case NI_PIX_FMT_BGRA:
    case NI_PIX_FMT_ARGB:
    case NI_PIX_FMT_ABGR:
    case NI_PIX_FMT_BGR0:
      align = 16;
      isSemiPlanar = false;
      break;
    case NI_PIX_FMT_NV16:
      align = 64;
      isSemiPlanar = false;
      break;
    case NI_PIX_FMT_YUYV422:
    case NI_PIX_FMT_UYVY422:
      align = 16;
      isSemiPlanar = false;
      break;
    case NI_PIX_FMT_BGRP:
    case NI_PIX_FMT_8_TILED4X4:
    case NI_PIX_FMT_10_TILED4X4:
    case NI_PIX_FMT_NONE:
      gst_buffer_unref (inbuf);
      return GST_FLOW_ERROR;
  }

  if (!gst_image_fill_linesizes (dst_stride, niPixFmt, vframe.info.width,
          align)) {
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  if (!p_src_session_data->data.frame.p_buffer) {
    p_src_session_data->data.frame.extra_data_len =
        NI_APP_ENC_FRAME_META_DATA_SIZE;
    retval = ni_frame_buffer_alloc_pixfmt (&p_src_session_data->data.frame,
        niPixFmt, vframe.info.width, vframe.info.height, dst_stride, 1,
        (int) p_src_session_data->data.frame.extra_data_len);
    if (retval < 0) {
      GST_ERROR_OBJECT (filter, "Cannot allocate ni_frame %d", retval);
      gst_buffer_unref (inbuf);
      return GST_FLOW_ERROR;
    }
  }

  memset (p_dst_session_data, 0, sizeof (dst_session_data));

  retval = ni_frame_buffer_alloc (&p_dst_session_data->data.frame,
      filter->width, filter->height, 0, 1, filter->api_ctx.bit_depth_factor, 1,
      !isSemiPlanar);
  if (retval < 0) {
    GST_ERROR_OBJECT (filter, "Failed to ni_frame_buffer_alloc : %d", retval);
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  if (copy_gst_to_ni_frame (dst_stride, &p_src_session_data->data.frame,
          &vframe) < 0) {
    GST_ERROR_OBJECT (filter, "Cannot copy frame\n");
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  niFrameSurface1_t *dst_surf;
  ni_frame_t *xfme;
  xfme = &p_dst_session_data->data.frame;
  dst_surf = (niFrameSurface1_t *) (xfme->p_buffer + xfme->data_len[0] +
      xfme->data_len[1] + xfme->data_len[2]);

  if (ni_device_session_hwup (&filter->api_ctx, p_src_session_data,
          dst_surf) < 0) {
    GST_ERROR_OBJECT (filter, "Failed to upload frame\n");
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  dst_surf->ui16width = filter->width;
  dst_surf->ui16height = filter->height;
  dst_surf->ui32nodeAddress = 0;
  ni_set_bit_depth_and_encoding_type (&dst_surf->bit_depth,
      &dst_surf->encoding_type, niPixFmt);

  outbuf = gst_buffer_new ();
  alloc = gst_allocator_find (GST_NIQUADRA_MEMORY_TYPE_NAME);
  out_mem = gst_niquadra_allocator_alloc (alloc, &filter->api_ctx, dst_surf,
      filter->api_ctx.hw_id, &filter->info);
  gst_buffer_append_memory (outbuf, out_mem);
  gst_object_unref (alloc);

  gst_video_frame_unmap (&vframe);

  ni_frame_buffer_free (&p_dst_session_data->data.frame);

  outbuf = gst_buffer_make_writable (outbuf);
  gst_buffer_copy_into (outbuf, inbuf,
      GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_META, 0, -1);
  gst_buffer_unref (inbuf);
  flow_ret = gst_pad_push (filter->srcpad, outbuf);

  return flow_ret;
}

static void
gst_niquadrahwupload_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiQuadraHWUpload *self;

  g_return_if_fail (GST_IS_NIQUADRAHWUPLOAD (object));
  self = GST_NIQUADRAHWUPLOAD (object);

  switch (prop_id) {
    case PROP_DEVICE:
      self->device = g_value_get_int (value);
      break;
    case PROP_KEEP_ALIVE_TIMEOUT:
      self->keep_alive_timeout = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadrahwupload_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiQuadraHWUpload *self;

  g_return_if_fail (GST_IS_NIQUADRAHWUPLOAD (object));
  self = GST_NIQUADRAHWUPLOAD (object);

  switch (prop_id) {
    case PROP_DEVICE:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->device);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_KEEP_ALIVE_TIMEOUT:
      g_value_set_uint (value, self->keep_alive_timeout);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadrahwupload_class_init (GstNiQuadraHWUploadClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gobject_class->set_property = gst_niquadrahwupload_set_property;
  gobject_class->get_property = gst_niquadrahwupload_get_property;

  g_object_class_install_property (gobject_class, PROP_DEVICE,
      g_param_spec_int ("device", "Device",
          "Device ID of Quadra hardware to use", -1, 255,
          -1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));

  g_object_class_install_property (gobject_class, PROP_KEEP_ALIVE_TIMEOUT,
      g_param_spec_uint ("keep-alive-timeout", "Keep-alive-timeout",
          "Specify a custom session keep alive timeout in seconds",
          NI_MIN_KEEP_ALIVE_TIMEOUT, NI_MAX_KEEP_ALIVE_TIMEOUT,
          NI_DEFAULT_KEEP_ALIVE_TIMEOUT,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (element_class, &src_factory);
  gst_element_class_add_static_pad_template (element_class, &sink_factory);

  gst_element_class_set_static_metadata (element_class,
      "NETINT Quadra HWUPLOAD filter", "Filter/Effect/Video/HWUpload",
      "Upload Netint Quadra SW to HW", "Simon Shi<simon.shi@netint.cn>");

  gobject_class->dispose = gst_niquadrahwupload_dispose;
}

static gboolean
niquadrahwupload_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadrahwupload_debug, "niquadrahwupload", 0,
      "niquadrahwupload");

  return gst_element_register (plugin, "niquadrahwupload", GST_RANK_NONE,
      GST_TYPE_NIQUADRAHWUPLOAD);
}
