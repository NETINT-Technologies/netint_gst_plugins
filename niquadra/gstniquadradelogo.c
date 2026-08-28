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
 *  \file   GstNiQuadraDelogo.c
 *
 *  \brief  Implement of NetInt Quadra hardware delogo filter.
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

GST_DEBUG_CATEGORY_STATIC (gst_niquadradelogo_debug);
#define GST_CAT_DEFAULT gst_niquadradelogo_debug

enum
{
  PROP_0,
  GST_NIQUADRA_DEC_PROP_TIMEOUT,
  PROP_W,
  PROP_H,
  PROP_X,
  PROP_Y,
  PROP_LAST
};

typedef struct _GstNiQuadraDelogo
{
  GstElement element;

  GstPad *sinkpad, *srcpad;

  GstVideoInfo info;
  gint in_width, in_height;
  GstVideoFormat format;

  ni_session_context_t api_ctx;
  ni_session_data_io_t api_dst_frame;

  int initialized;
  guint keep_alive_timeout;

  gint x;                       // x offset of the delogo area with respect to the input area
  gint y;                       // y offset of the delogo area with respect to the input area
  gint w;                       // width of the delogo area
  gint h;                       // height of the delogo area
  guint extra_frames;
  gint downstream_card;
} GstNiQuadraDelogo;

typedef struct _GstNiQuadraDelogoClass
{
  GstElementClass parent_class;
} GstNiQuadraDelogoClass;

#define GST_TYPE_NIQUADRADELOGO \
  (gst_niquadradelogo_get_type())
#define GST_NIQUADRADELOGO(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRADELOGO,GstNiQuadraDelogo))
#define GST_NIQUADRADELOGO_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRADELOGO,GstNiQuadraDelogo))
#define GST_IS_NIQUADRADELOGO(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRADELOGO))
#define GST_IS_NIQUADRADELOGO_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRADELOGO))

static gboolean niquadradelogo_element_init (GstPlugin * plugin);

GType gst_niquadradelogoniquadra_get_type (void);

G_DEFINE_TYPE (GstNiQuadraDelogo, gst_niquadradelogo, GST_TYPE_ELEMENT);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadradelogo,
    niquadradelogo_element_init);

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

static GstFlowReturn gst_niquadra_delogo_chain (GstPad * pad,
    GstObject * parent, GstBuffer * inbuf);

static gboolean
gst_niquadra_delogo_sink_setcaps (GstPad * pad, GstObject * parent,
    GstCaps * caps)
{
  GstNiQuadraDelogo *filter = GST_NIQUADRADELOGO (parent);
  GstStructure *structure = gst_caps_get_structure (caps, 0);
  GstCaps *src_caps;
  gboolean ret, gotit = FALSE;
  GstVideoInfo *info = &filter->info;
  GstQuery *query;
  guint i;
  GType gtype;
  const GstStructure *params = NULL;
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
        if (params && (strcmp (gst_structure_get_name (params),
                    NI_PREALLOCATE_STRUCTURE_NAME) == 0)) {

          gotit = gst_structure_get_uint (params, NI_VIDEO_META_BUFCNT,
              &filter->extra_frames);
          if (gotit == FALSE)
            GST_ERROR_OBJECT (filter, "Did not find buffercnt\n");

          gotit = gst_structure_get_int (params, NI_VIDEO_META_CARDNO,
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
  info->width = filter->in_width;
  info->height = filter->in_height;
  src_caps = gst_video_info_to_caps (info);

  filter->w = NI_ALIGN (filter->w, 2);
  filter->h = NI_ALIGN (filter->h, 2);
  filter->x = NI_ALIGN (filter->x, 2);
  filter->y = NI_ALIGN (filter->y, 2);

  if (filter->x < 0 || filter->y < 0 ||
      filter->x >= filter->in_width || filter->h >= filter->in_height) {
    return FALSE;
  }
  if (filter->w <= 0 || filter->h <= 0 ||
      filter->w > filter->in_width || filter->h > filter->in_height) {
    return FALSE;
  }

  gst_caps_set_features_simple (src_caps,
      gst_caps_features_from_string (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY));

  ret = gst_pad_set_caps (filter->srcpad, src_caps);
  gst_caps_unref (src_caps);

  return ret;
}

static gboolean
gst_niquadra_delogo_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstNiQuadraDelogo *filter = GST_NIQUADRADELOGO (parent);
  gboolean ret = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:{
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret = gst_niquadra_delogo_sink_setcaps (pad, parent, caps);
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
gst_niquadradelogo_init (GstNiQuadraDelogo * filter)
{
  filter->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_event_function (filter->sinkpad, gst_niquadra_delogo_sink_event);
  gst_pad_set_chain_function (filter->sinkpad, gst_niquadra_delogo_chain);
  gst_element_add_pad (GST_ELEMENT (filter), filter->sinkpad);

  filter->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  gst_element_add_pad (GST_ELEMENT (filter), filter->srcpad);

  filter->downstream_card = -1;
  filter->extra_frames = 0;
}

static void
gst_niquadradelogo_dispose (GObject * obj)
{
  GstNiQuadraDelogo *filter = GST_NIQUADRADELOGO (obj);

  if (filter->api_dst_frame.data.frame.p_buffer) {
    ni_frame_buffer_free (&filter->api_dst_frame.data.frame);
  }
  if (filter->api_ctx.session_id != NI_INVALID_SESSION_ID) {
    GST_DEBUG_OBJECT (filter, "libxcoder delogo free context\n");
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

  G_OBJECT_CLASS (gst_niquadradelogo_parent_class)->dispose (obj);
}

static GstFlowReturn
gst_niquadra_delogo_chain (GstPad * pad, GstObject * parent, GstBuffer * inbuf)
{
  GstNiQuadraDelogo *filter = GST_NIQUADRADELOGO (parent);
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
    filter->api_ctx.scaler_operation = NI_SCALER_OPCODE_DELOGO;
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
      NI_ALIGN (filter->in_width, 2),
      NI_ALIGN (filter->in_height, 2),
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

  if ((unsigned) filter->x + (unsigned) filter->w > filter->in_width) {
    filter->x = filter->in_width - filter->w;
    filter->x = NI_ALIGN (filter->x, 2);
  }
  if ((unsigned) filter->y + (unsigned) filter->h > filter->in_height) {
    filter->y = filter->in_height - filter->h;
    filter->y = NI_ALIGN (filter->y, 2);
  }

  retval = ni_frame_buffer_alloc_hwenc (&p_session_data->data.frame,
      filter->in_width, filter->in_height, 0);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter,
        "ni delogo filter frame buffer alloc hwenc failed with %d\n", retval);
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  niPixFmt = convertGstVideoFormatToNIPix (filter->format);
  gc620_pixfmt = convertNIPixToGC620Format (niPixFmt);

  /*
   * Allocate device input frame. This call won't actually allocate a frame,
   * but sends the incoming hardware frame index to the scaler manager
   */
  /* *INDENT-OFF* */
  retval = ni_device_alloc_frame (&filter->api_ctx,
      NI_ALIGN (filter->in_width, 2),
      NI_ALIGN (filter->in_height, 2),
      gc620_pixfmt,
      0,                        // input frame
      filter->w,                // src rectangle width
      filter->h,                // src rectangle height
      filter->x,                // src rectangle x
      filter->y,                // src rectangle y
      in_surface->ui32nodeAddress,
      in_surface->ui16FrameIdx, NI_DEVICE_TYPE_SCALER);
  /* *INDENT-ON* */

  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter,
        "ni delogo filter device alloc input frame failed with %d\n", retval);
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  /* Allocate device destination frame This will acquire a frame from the pool */
  /* *INDENT-OFF* */
  retval = ni_device_alloc_frame (&filter->api_ctx,
      NI_ALIGN (filter->in_width, 2),
      NI_ALIGN (filter->in_height, 2),
      gc620_pixfmt,
      NI_SCALER_FLAG_IO,
      0,
      0,
      0,
      0,
      0,
      -1,
      NI_DEVICE_TYPE_SCALER);
  /* *INDENT-ON* */

  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter,
        "ni delogo filter device alloc output frame failed with %d\n", retval);
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
  out_surface->ui16width = filter->in_width;
  out_surface->ui16height = filter->in_height;

  ni_set_bit_depth_and_encoding_type (&out_surface->bit_depth,
      &out_surface->encoding_type, niPixFmt);

  outbuf = gst_buffer_new ();

  alloc = gst_allocator_find (GST_NIQUADRA_MEMORY_TYPE_NAME);
  out_mem = gst_niquadra_allocator_alloc (alloc, &filter->api_ctx, out_surface,
      filter->api_ctx.hw_id, &filter->info);
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
gst_niquadradelogo_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiQuadraDelogo *self;

  g_return_if_fail (GST_IS_NIQUADRADELOGO (object));
  self = GST_NIQUADRADELOGO (object);

  switch (prop_id) {
    case PROP_W:
      self->w = g_value_get_int (value);
      break;
    case PROP_H:
      self->h = g_value_get_int (value);
      break;
    case PROP_X:
      self->x = g_value_get_int (value);
      break;
    case PROP_Y:
      self->y = g_value_get_int (value);
      break;
    case GST_NIQUADRA_DEC_PROP_TIMEOUT:
      self->keep_alive_timeout = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadradelogo_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiQuadraDelogo *self;

  g_return_if_fail (GST_IS_NIQUADRADELOGO (object));
  self = GST_NIQUADRADELOGO (object);

  switch (prop_id) {
    case PROP_W:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->w);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_H:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->h);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_X:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->x);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_Y:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->y);
      GST_OBJECT_UNLOCK (self);
      break;
    case GST_NIQUADRA_DEC_PROP_TIMEOUT:
      GST_OBJECT_LOCK (self);
      g_value_set_uint (value, self->keep_alive_timeout);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadradelogo_class_init (GstNiQuadraDelogoClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gobject_class->set_property = gst_niquadradelogo_set_property;
  gobject_class->get_property = gst_niquadradelogo_get_property;

  g_object_class_install_property (gobject_class, PROP_H,
      g_param_spec_int ("h", "H",
          "set the height delogo area expression", 0, G_MAXINT,
          0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_W,
      g_param_spec_int ("w", "W",
          "set the width delogo area expression", 0, G_MAXINT,
          0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_X,
      g_param_spec_int ("x", "X",
          "set the x delogo area expression", 0, G_MAXINT,
          0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_Y,
      g_param_spec_int ("y", "Y",
          "set the y delogo area expression", 0, G_MAXINT,
          0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, GST_NIQUADRA_DEC_PROP_TIMEOUT,
      g_param_spec_uint ("keep-alive-timeout", "TIMEOUT",
          "Specify a custom session keep alive timeout in seconds. Defult is 3.",
          NI_MIN_KEEP_ALIVE_TIMEOUT, NI_MAX_KEEP_ALIVE_TIMEOUT,
          NI_DEFAULT_KEEP_ALIVE_TIMEOUT,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (element_class, &src_factory);
  gst_element_class_add_static_pad_template (element_class, &sink_factory);

  gst_element_class_set_static_metadata (element_class,
      "NETINT Quadra DELOGO filter", "Filter/Effect/Video/NIDelogo",
      "Delogo Netint Quadra", "Minglong Zhang<Minglong.Zhang@netint.cn>");

  gobject_class->dispose = gst_niquadradelogo_dispose;
}

static gboolean
niquadradelogo_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadradelogo_debug, "niquadradelogo",
      0, "niquadradelogo");

  return gst_element_register (plugin, "niquadradelogo", GST_RANK_NONE,
      GST_TYPE_NIQUADRADELOGO);
}
