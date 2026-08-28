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
 *  \file   GstNiQuadraFlip.c
 *
 *  \brief  Implement of NetInt Quadra hardware flip filter.
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

GST_DEBUG_CATEGORY_STATIC (gst_niquadraflip_debug);
#define GST_CAT_DEFAULT gst_niquadraflip_debug

enum
{
  PROP_0,
  GST_NIQUADRA_DEC_PROP_TIMEOUT,
  PROP_FLIP_TYPE,
  PROP_LAST
};

typedef struct _GstNiQuadraFlip
{
  GstElement element;

  GstPad *sinkpad, *srcpad;

  GstVideoInfo info;
  GstVideoInfo output_info;
  gint in_width, in_height;
  gint out_width, out_height;
  GstVideoFormat format;

  ni_session_context_t api_ctx;
  ni_session_data_io_t api_dst_frame;

  int initialized;
  guint keep_alive_timeout;     /* keep alive timeout setting */
  bool flip_type;
  guint extra_frames;
  gint downstream_card;
} GstNiQuadraFlip;

typedef struct _GstNiQuadraFlipClass
{
  GstElementClass parent_class;
} GstNiQuadraFlipClass;

#define GST_TYPE_NIQUADRAFLIP \
  (gst_niquadraflip_get_type())
#define GST_NIQUADRAFLIP(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRAFLIP,GstNiQuadraFlip))
#define GST_NIQUADRAFLIP_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRAFLIP,GstNiQuadraFlip))
#define GST_IS_NIQUADRAFLIP(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRAFLIP))
#define GST_IS_NIQUADRAFLIP_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRAFLIP))

static gboolean niquadraflip_element_init (GstPlugin * plugin);

GType gst_niquadraflipniquadra_get_type (void);

G_DEFINE_TYPE (GstNiQuadraFlip, gst_niquadraflip, GST_TYPE_ELEMENT);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadraflip, niquadraflip_element_init);

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
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY, SUPPORTED_FORMATS))
    );

static GstFlowReturn gst_niquadra_flip_chain (GstPad * pad,
    GstObject * parent, GstBuffer * inbuf);

static gboolean
gst_niquadra_flip_sink_setcaps (GstPad * pad, GstObject * parent,
    GstCaps * caps)
{
  GstNiQuadraFlip *filter = GST_NIQUADRAFLIP (parent);
  GstStructure *structure;
  GstCaps *src_caps;
  gboolean ret, gotit = FALSE;
  GstVideoInfo *info = &filter->info;
  GstQuery *query;
  guint i;
  GType gtype;
  const GstStructure *params = NULL;

  structure = gst_caps_get_structure (caps, 0);
  if (!gst_structure_get_int (structure, "width", &filter->in_width))
    return FALSE;
  if (!gst_structure_get_int (structure, "height", &filter->in_height))
    return FALSE;

  if (!gst_video_info_from_caps (info, caps))
    return FALSE;

  /* Query the downstream element for proposed allocation */
  query = gst_query_new_allocation (caps, TRUE);

  if (gst_pad_peer_query (filter->srcpad, query) == TRUE) {
    /* Search for allocation metadata */
    for (i = 0; i < gst_query_get_n_allocation_metas (query); i++) {
      gtype = gst_query_parse_nth_allocation_meta (query, i, &params);
      if (gtype == GST_VIDEO_META_API_TYPE) {
        if (params
            && (strcmp (gst_structure_get_name (params),
                    NI_PREALLOCATE_STRUCTURE_NAME) == 0)) {

          gotit =
              gst_structure_get_uint (params, NI_VIDEO_META_BUFCNT,
              &filter->extra_frames);
          if (gotit == FALSE)
            GST_ERROR_OBJECT (filter, "Did not find buffercnt\n");

          gotit =
              gst_structure_get_int (params, NI_VIDEO_META_CARDNO,
              &filter->downstream_card);
          if (gotit == FALSE)
            GST_ERROR_OBJECT (filter, "Did not find cardno\n");

          break;
        }
      }
    }
  }

  if (info->colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255) {
    GST_DEBUG_OBJECT (filter,
        "WARNING: Full color range input, limited color output\n");
  }
  info->colorimetry.range = GST_VIDEO_COLOR_RANGE_16_235;

  gst_query_unref (query);

  filter->format = info->finfo->format;
  filter->out_width = filter->in_width;
  filter->out_height = filter->in_height;
  info->width = filter->out_width = NI_ALIGN (filter->out_width, 2);
  info->height = filter->out_height = NI_ALIGN (filter->out_height, 2);

  src_caps = gst_video_info_to_caps (info);
  gst_caps_set_features_simple (src_caps,
      gst_caps_features_from_string (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY));

  if (!gst_video_info_from_caps (&filter->output_info, src_caps))
    return FALSE;

  ret = gst_pad_set_caps (filter->srcpad, src_caps);
  gst_caps_unref (src_caps);

  return ret;
}

static gboolean
gst_niquadra_flip_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstNiQuadraFlip *filter = GST_NIQUADRAFLIP (parent);
  gboolean ret = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:{
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret = gst_niquadra_flip_sink_setcaps (pad, parent, caps);
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
gst_niquadraflip_init (GstNiQuadraFlip * filter)
{
  filter->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_event_function (filter->sinkpad, gst_niquadra_flip_sink_event);
  gst_pad_set_chain_function (filter->sinkpad, gst_niquadra_flip_chain);
  gst_element_add_pad (GST_ELEMENT (filter), filter->sinkpad);

  filter->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  gst_element_add_pad (GST_ELEMENT (filter), filter->srcpad);

  filter->downstream_card = -1;
  filter->extra_frames = 0;
}

static void
gst_niquadraflip_dispose (GObject * obj)
{
  GstNiQuadraFlip *filter = GST_NIQUADRAFLIP (obj);
  if (filter->api_dst_frame.data.frame.p_buffer) {
    ni_frame_buffer_free (&filter->api_dst_frame.data.frame);
  }
  if (filter->api_ctx.session_id != NI_INVALID_SESSION_ID) {
    GST_DEBUG_OBJECT (filter, "libxcoder flip free context\n");
    ni_device_session_close (&filter->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
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

  G_OBJECT_CLASS (gst_niquadraflip_parent_class)->dispose (obj);
}

static GstFlowReturn
gst_niquadra_flip_chain (GstPad * pad, GstObject * parent, GstBuffer * inbuf)
{
  GstNiQuadraFlip *filter = GST_NIQUADRAFLIP (parent);
  GstAllocator *alloc = NULL;
  GstMemory *in_mem = NULL;
  GstMemory *out_mem = NULL;
  GstBuffer *outbuf = NULL;
  niFrameSurface1_t *in_surface = NULL;
  gint dev_idx = -1;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  int retval = 0;
  niFrameSurface1_t *out_surface = NULL;
  ni_session_data_io_t *p_session_data = &filter->api_dst_frame;
  int gc620_pixfmt;
  ni_pix_fmt_t niPixFmt;
  ni_frame_config_t input_frame_config = { 0 };

  memset (p_session_data, 0, sizeof (ni_session_data_io_t));

  in_mem = gst_buffer_peek_memory (inbuf, 0);
  dev_idx = gst_deviceid_from_ni_hw_memory (in_mem);
  in_surface = gst_surface_from_ni_hw_memory (in_mem);
  if (in_surface == NULL) {
    GST_ERROR_OBJECT (filter,
        "Impossible to convert between the formats supported by the filter\n");
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  if (!filter->initialized) {
    if (filter->format == GST_VIDEO_FORMAT_BGRP) {
      GST_ERROR_OBJECT (filter, "format BGRP not supported\n");
      gst_buffer_unref (inbuf);
      return GST_FLOW_ERROR;
    }

    ni_device_session_context_init (&filter->api_ctx);
    filter->api_ctx.session_id = NI_INVALID_SESSION_ID;
    filter->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
    filter->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;
    filter->api_ctx.hw_id = dev_idx;
    filter->api_ctx.device_type = NI_DEVICE_TYPE_SCALER;
    filter->api_ctx.scaler_operation = NI_SCALER_OPCODE_FLIP;
    filter->api_ctx.keep_alive_timeout = filter->keep_alive_timeout;
    filter->api_ctx.isP2P = 0;

    retval = ni_device_session_open (&filter->api_ctx, NI_DEVICE_TYPE_SCALER);
    if (retval < 0) {
      GST_ERROR_OBJECT (filter,
          "Can't open device session on card %d\n", filter->api_ctx.hw_id);
      gst_buffer_unref (inbuf);
      if (filter->api_dst_frame.data.frame.p_buffer) {
        ni_frame_buffer_free (&filter->api_dst_frame.data.frame);
      }
      ni_device_session_close (&filter->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
      ni_device_session_context_clear (&filter->api_ctx);
      return GST_FLOW_ERROR;
    } else {
      GST_DEBUG_OBJECT (filter,
          "XCoder %s.%d (inst: %d) opened successfully\n",
          filter->api_ctx.dev_xcoder_name, filter->api_ctx.hw_id,
          filter->api_ctx.session_id);
    }
    memset (&filter->api_dst_frame, 0, sizeof (ni_session_data_io_t));

    int options;
    options = NI_SCALER_FLAG_IO | NI_SCALER_FLAG_PC;
    if (filter->api_ctx.isP2P) {
      options |= NI_SCALER_FLAG_P2;
    }
    int pool_size = 4;
    if (filter->api_ctx.isP2P) {
      pool_size = 1;
    } else {
      // If the downstream element is on a different card,
      // Allocate extra frames suggested by the downstream element
      if (filter->api_ctx.hw_id != filter->downstream_card) {
        pool_size += filter->extra_frames;
        GST_INFO_OBJECT (filter,
            "Increase frame pool by %d\n", filter->extra_frames);
      }
    }

    niPixFmt = convertGstVideoFormatToNIPix (filter->format);
    gc620_pixfmt = convertNIPixToGC620Format (niPixFmt);

    /* Allocate a pool of frames by the scaler */
    /* *INDENT-OFF* */
    retval = ni_device_alloc_frame (&filter->api_ctx,
      NI_ALIGN (filter->out_width, 2),
      NI_ALIGN (filter->out_height, 2),
      gc620_pixfmt,
      options,
      0,                      // rec width
      0,                      // rec height
      0,                      // rec X pos
      0,                      // rec Y pos
      pool_size,              // rgba color/pool size
      0,                      // frame index
      NI_DEVICE_TYPE_SCALER);
    /* *INDENT-ON* */

    if (retval < 0) {
      GST_ERROR_OBJECT (filter, "Init frame pool error\n");
      ni_device_session_context_clear (&filter->api_ctx);
      gst_buffer_unref (inbuf);
      return GST_FLOW_ERROR;
    }

    filter->initialized = 1;
  }

  retval = ni_frame_buffer_alloc_hwenc (&p_session_data->data.frame,
      filter->out_width, filter->out_height, 0);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter,
        "ni flip filter frame buffer alloc hwenc failed with %d", retval);
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  niPixFmt = convertGstVideoFormatToNIPix (filter->format);
  gc620_pixfmt = convertNIPixToGC620Format (niPixFmt);

  input_frame_config.picture_format = gc620_pixfmt;
  input_frame_config.rgba_color = in_surface->ui32nodeAddress;
  input_frame_config.frame_index = in_surface->ui16FrameIdx;
  input_frame_config.rectangle_x = 0;
  input_frame_config.rectangle_y = 0;
  input_frame_config.rectangle_width = input_frame_config.picture_width =
      filter->in_width;
  input_frame_config.rectangle_height = input_frame_config.picture_height =
      filter->in_height;

  if (filter->flip_type == 0) { //hflip
    input_frame_config.orientation = 4;
  } else if (filter->flip_type == 1) {  //vflip
    input_frame_config.orientation = 5;
  }
  // use ni_device_config_frame() instead of ni_device_alloc_frame()
  // such that input_frame_config's orientation can be configured
  retval = ni_device_config_frame (&filter->api_ctx, &input_frame_config);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter,
        "ni flip filter device config input frame failed with %d", retval);
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  retval = ni_device_alloc_frame (&filter->api_ctx,
      filter->out_width,
      filter->out_height,
      gc620_pixfmt,
      NI_SCALER_FLAG_IO, 0, 0, 0, 0, 0, -1, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter,
        "ni flip filter device alloc output frame failed with %d", retval);
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  /* Set the new frame index */
  retval = ni_device_session_read_hwdesc (&filter->api_ctx, p_session_data,
      NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "Can't acquire output frame %d\n", retval);
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  out_surface = (niFrameSurface1_t *) p_session_data->data.frame.p_data[3];
  out_surface->dma_buf_fd = 0;
  out_surface->ui32nodeAddress = 0;
  out_surface->ui16width = filter->out_width;
  out_surface->ui16height = filter->out_height;

  ni_set_bit_depth_and_encoding_type (&out_surface->bit_depth,
      &out_surface->encoding_type, niPixFmt);

  outbuf = gst_buffer_new ();

  alloc = gst_allocator_find (GST_NIQUADRA_MEMORY_TYPE_NAME);
  out_mem = gst_niquadra_allocator_alloc (alloc, &filter->api_ctx, out_surface,
      filter->api_ctx.hw_id, &filter->output_info);
  gst_buffer_append_memory (outbuf, out_mem);
  gst_object_unref (alloc);

  ni_frame_buffer_free (&p_session_data->data.frame);

  outbuf = gst_buffer_make_writable (outbuf);
  gst_buffer_copy_into (outbuf, inbuf,
      GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_META, 0, -1);
  gst_buffer_unref (inbuf);
  flow_ret = gst_pad_push (filter->srcpad, outbuf);

  return flow_ret;
}

static void
gst_niquadraflip_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiQuadraFlip *self;

  g_return_if_fail (GST_IS_NIQUADRAFLIP (object));
  self = GST_NIQUADRAFLIP (object);

  switch (prop_id) {
    case GST_NIQUADRA_DEC_PROP_TIMEOUT:
      self->keep_alive_timeout = g_value_get_uint (value);
      break;
    case PROP_FLIP_TYPE:
      self->flip_type = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadraflip_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiQuadraFlip *self;

  g_return_if_fail (GST_IS_NIQUADRAFLIP (object));
  self = GST_NIQUADRAFLIP (object);

  switch (prop_id) {
    case GST_NIQUADRA_DEC_PROP_TIMEOUT:
      GST_OBJECT_LOCK (self);
      g_value_set_uint (value, self->keep_alive_timeout);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_FLIP_TYPE:
      GST_OBJECT_LOCK (self);
      g_value_set_boolean (value, self->flip_type);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadraflip_class_init (GstNiQuadraFlipClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gobject_class->set_property = gst_niquadraflip_set_property;
  gobject_class->get_property = gst_niquadraflip_get_property;

  g_object_class_install_property (gobject_class, GST_NIQUADRA_DEC_PROP_TIMEOUT,
      g_param_spec_uint ("keep-alive-timeout", "TIMEOUT",
          "Specify a custom session keep alive timeout in seconds. Defult is 3.",
          NI_MIN_KEEP_ALIVE_TIMEOUT, NI_MAX_KEEP_ALIVE_TIMEOUT,
          NI_DEFAULT_KEEP_ALIVE_TIMEOUT,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_FLIP_TYPE,
      g_param_spec_boolean ("flip-type", "Flip-Type",
          "choose horizontal (0) or vertical (1) flip", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_NAME | GST_PARAM_MUTABLE_PLAYING |
          GST_PARAM_CONTROLLABLE));

  gst_element_class_add_static_pad_template (element_class, &src_factory);
  gst_element_class_add_static_pad_template (element_class, &sink_factory);

  gst_element_class_set_static_metadata (element_class,
      "NETINT Quadra FLIP filter", "Filter/Effect/Video/NIFlip",
      "Flip Netint Quadra", "Minglong Zhang<Minglong.Zhang@netint.cn>");

  gobject_class->dispose = gst_niquadraflip_dispose;
}

static gboolean
niquadraflip_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadraflip_debug, "niquadraflip", 0,
      "niquadraflip");

  return gst_element_register (plugin, "niquadraflip", GST_RANK_NONE,
      GST_TYPE_NIQUADRAFLIP);
}
