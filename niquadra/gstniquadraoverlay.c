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
 *  \file   gstniquadraoverlay.c
 *
 *  \brief  Implement of NetInt Quadra hardware overlay filter.
 ******************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstniquadraoverlay.h"
#include "niquadra.h"
#include "gstniquadramemory.h"
#include "gstniquadrautils.h"

GST_DEBUG_CATEGORY_STATIC (gst_niquadraoverlay_debug);
#define GST_CAT_DEFAULT gst_niquadraoverlay_debug

#if ENABLE_NI_GST_FRAMEWORK_PATCH
#define NI_QUADRA_TILED_FORMATS ", NI_QUAD_8_4L4"
#else
#define NI_QUADRA_TILED_FORMATS ""
#endif

#define SUPPORTED_FORMATS \
    "{ I420, YUY2, UYVY, NV12, ARGB, RGBA, ABGR, BGRA, I420_10LE, P010_10LE, " \
    "NV16, BGRx, NV12_10LE32" NI_QUADRA_TILED_FORMATS " }"

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink_%u",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY, SUPPORTED_FORMATS))
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY, SUPPORTED_FORMATS))
    );

static void gst_niquadraoverlay_child_proxy_init (gpointer g_iface,
    gpointer iface_data);

G_DEFINE_TYPE (GstNiQuadraOverlayPad, gst_niquadraoverlay_pad,
    GST_TYPE_VIDEO_AGGREGATOR_CONVERT_PAD);

static void
gst_niquadraoverlay_pad_class_init (GstNiQuadraOverlayPadClass * klass)
{
  GstVideoAggregatorPadClass *vaggpad_class =
      GST_VIDEO_AGGREGATOR_PAD_CLASS (klass);
  vaggpad_class->prepare_frame = NULL;
  vaggpad_class->clean_frame = NULL;
}

static void
gst_niquadraoverlay_pad_init (GstNiQuadraOverlayPad * pad)
{
}

enum
{
  PROP_0,
  PROP_X,
  PROP_Y,
  PROP_EOF_ACTION,
  PROP_SHORTEST,
  PROP_REPEATLAST,
  PROP_ALPHA,
  PROP_INPLACE_OVERLAY,
  PROP_IS_P2P,
  PROP_KEEP_ALIVE_TIMEOUT,
  PROP_LAST
};

static const GstVideoFormat alpha_pix_fmts[] = {
  GST_VIDEO_FORMAT_ARGB, GST_VIDEO_FORMAT_ABGR, GST_VIDEO_FORMAT_RGBA,
  GST_VIDEO_FORMAT_BGRA, GST_VIDEO_FORMAT_UNKNOWN
};

static gboolean
gst_niquadraoverlay_sinkpads_interlace_mode (GstVideoAggregator * vagg,
    GstVideoAggregatorPad * skip_pad, GstVideoInterlaceMode * mode)
{
  GList *walk;

  GST_OBJECT_LOCK (vagg);
  for (walk = GST_ELEMENT (vagg)->sinkpads; walk; walk = g_list_next (walk)) {
    GstVideoAggregatorPad *vaggpad = walk->data;

    if (skip_pad && vaggpad == skip_pad)
      continue;
    if (vaggpad->info.finfo
        && GST_VIDEO_INFO_FORMAT (&vaggpad->info) != GST_VIDEO_FORMAT_UNKNOWN) {
      *mode = GST_VIDEO_INFO_INTERLACE_MODE (&vaggpad->info);
      GST_OBJECT_UNLOCK (vagg);
      return TRUE;
    }
  }
  GST_OBJECT_UNLOCK (vagg);
  return FALSE;
}


static gboolean
gst_fmt_is_in (GstVideoFormat fmt, const GstVideoFormat * fmts)
{
  GstVideoFormat p = fmts[0];
  int i = 0;

  for (; p != GST_VIDEO_FORMAT_UNKNOWN; i++) {
    if (fmt == p)
      return TRUE;

    p = fmts[i];
  }

  return FALSE;
}

static void
gst_niquadraoverlay_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiQuadraOverlay *self;

  g_return_if_fail (GST_IS_NIQUADRAOVERLAY (object));
  self = GST_NIQUADRAOVERLAY (object);

  switch (prop_id) {
    case PROP_X:
      g_value_set_int (value, self->x);
      break;
    case PROP_Y:
      g_value_set_int (value, self->y);
      break;
    case PROP_EOF_ACTION:
      g_value_take_string (value, g_strdup (self->eof_action_str));
      break;
    case PROP_SHORTEST:
      g_value_set_boolean (value, self->shortest);
      break;
    case PROP_REPEATLAST:
      g_value_set_boolean (value, self->repeat_last);
      break;
    case PROP_ALPHA:
      g_value_take_string (value, g_strdup (self->alpha));
      break;
    case PROP_INPLACE_OVERLAY:
      g_value_set_boolean (value, self->inplace);
      break;
    case PROP_IS_P2P:
      g_value_set_boolean (value, self->is_p2p);
      break;
    case PROP_KEEP_ALIVE_TIMEOUT:
      g_value_set_uint (value, self->keep_alive_timeout);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadraoverlay_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiQuadraOverlay *self;

  g_return_if_fail (GST_IS_NIQUADRAOVERLAY (object));
  self = GST_NIQUADRAOVERLAY (object);

  switch (prop_id) {
    case PROP_X:
      self->x = g_value_get_int (value);
      break;
    case PROP_Y:
      self->y = g_value_get_int (value);
      break;
    case PROP_EOF_ACTION:
      g_free (self->eof_action_str);
      self->eof_action_str = g_value_dup_string (value);
      break;
    case PROP_SHORTEST:
      self->shortest = g_value_get_boolean (value);
      break;
    case PROP_REPEATLAST:
      self->repeat_last = g_value_get_boolean (value);
      break;
    case PROP_ALPHA:
      g_free (self->alpha);
      self->alpha = g_value_dup_string (value);
      break;
    case PROP_INPLACE_OVERLAY:
      self->inplace = g_value_get_boolean (value);
      break;
    case PROP_IS_P2P:
      self->is_p2p = g_value_get_boolean (value);
      break;
    case PROP_KEEP_ALIVE_TIMEOUT:
      self->keep_alive_timeout = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static gboolean niquadraoverlay_element_init (GstPlugin * plugin);

#define gst_niquadraoverlay_parent_class parent_class

G_DEFINE_TYPE_WITH_CODE (GstNiQuadraOverlay, gst_niquadraoverlay,
    GST_TYPE_VIDEO_AGGREGATOR, G_IMPLEMENT_INTERFACE (GST_TYPE_CHILD_PROXY,
        gst_niquadraoverlay_child_proxy_init));

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadraoverlay,
    niquadraoverlay_element_init);

static GstCaps *
gst_niquadraoverlay_fixate_caps (GstAggregator * agg, GstCaps * caps)
{
  GstVideoAggregator *vagg = GST_VIDEO_AGGREGATOR (agg);
  GstNiQuadraOverlay *self = GST_NIQUADRAOVERLAY (vagg);
  GList *l;
  gint width, height;
  gint main_fps_n = 1, main_fps_d = 1;
  gint overlay_fps_n = 1, overlay_fps_d = 1;
  GstVideoFormat pixfmt;
  GstStructure *s;
  GstCaps *ret = NULL;
  gboolean has_interlace_mode;
  int i = 0;

  self->alpha_format = 0;
  if (!strcmp (self->alpha, "straight")) {
    self->alpha_format = 0;
  } else if (!strcmp (self->alpha, "premultiplied")) {
    self->alpha_format = 1;
  }

  self->eof_action = EOF_ACTION_REPEAT;
  if (!strcmp (self->eof_action_str, "repeat")) {
    self->eof_action = EOF_ACTION_REPEAT;
  } else if (!strcmp (self->eof_action_str, "endall")) {
    self->eof_action = EOF_ACTION_ENDALL;
  } else if (!strcmp (self->eof_action_str, "pass")) {
    self->eof_action = EOF_ACTION_PASS;
  }

  if (!self->repeat_last || (self->eof_action == EOF_ACTION_PASS)) {
    self->repeat_last = FALSE;
    self->eof_action = EOF_ACTION_PASS;
  }

  if (self->shortest || (self->eof_action == EOF_ACTION_ENDALL)) {
    self->shortest = TRUE;
    self->eof_action = EOF_ACTION_ENDALL;
  }

  GST_OBJECT_LOCK (vagg);

  for (l = GST_ELEMENT (vagg)->sinkpads; l; l = l->next) {
    GstVideoAggregatorPad *vaggpad = l->data;

    width = GST_VIDEO_INFO_WIDTH (&vaggpad->info);
    height = GST_VIDEO_INFO_HEIGHT (&vaggpad->info);
    pixfmt = GST_VIDEO_INFO_FORMAT (&vaggpad->info);

    if (i == 0) {
      main_fps_n = GST_VIDEO_INFO_FPS_N (&vaggpad->info);
      main_fps_d = GST_VIDEO_INFO_FPS_D (&vaggpad->info);

      self->out_width = self->main_width = width;
      self->out_height = self->main_height = height;
      self->out_pixfmt = self->main_pixfmt = pixfmt;
      self->main_has_alpha = gst_fmt_is_in (pixfmt, alpha_pix_fmts);
      self->main_colorimetry = vaggpad->info.colorimetry;
      self->main_colorimetry.range = GST_VIDEO_COLOR_RANGE_16_235;
    } else if (i == 1) {
      overlay_fps_n = GST_VIDEO_INFO_FPS_N (&vaggpad->info);
      overlay_fps_d = GST_VIDEO_INFO_FPS_D (&vaggpad->info);

      self->overlay_width = width;
      self->overlay_height = height;
      self->overlay_pixfmt = pixfmt;
      self->overlay_has_alpha = gst_fmt_is_in (pixfmt, alpha_pix_fmts);
      self->overlay_colorimetry = vaggpad->info.colorimetry;
    }

    i++;
  }

  GST_OBJECT_UNLOCK (vagg);

  ret = gst_caps_make_writable (caps);
  s = gst_caps_get_structure (ret, 0);

  has_interlace_mode = gst_niquadraoverlay_sinkpads_interlace_mode (vagg, NULL,
      &self->interlace_mode);
  if (has_interlace_mode)
    gst_structure_set (s, "interlace-mode", G_TYPE_STRING,
        gst_video_interlace_mode_to_string (self->interlace_mode), NULL);
  gst_structure_set (s, "width", G_TYPE_INT, self->out_width, NULL);
  gst_structure_set (s, "height", G_TYPE_INT, self->out_height, NULL);
  gst_caps_set_features_simple (ret,
      gst_caps_features_from_string (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY));

  GValue target = { 0 };
  g_value_init (&target, GST_TYPE_FRACTION);
  if (main_fps_n == 1 && main_fps_d == 1) {
    gst_value_set_fraction (&target, overlay_fps_n, overlay_fps_d);
  } else {
    gst_value_set_fraction (&target, main_fps_n, main_fps_d);
  }
  gst_structure_set_value (s, "framerate", &target);


  //HW does not support NV12 Compress + RGB -> NV12 Compress
#if ENABLE_NI_GST_FRAMEWORK_PATCH
/* *INDENT-OFF* */
  if ((self->main_pixfmt == GST_VIDEO_FORMAT_NI_QUAD_8_4L4
      || self->main_pixfmt == GST_VIDEO_FORMAT_NI_QUAD_10_4L4)
      && (self->overlay_pixfmt >= GST_VIDEO_FORMAT_RGBA
      && self->overlay_pixfmt <= GST_VIDEO_FORMAT_ABGR)) {
    self->out_pixfmt = GST_VIDEO_FORMAT_NV12;
    GST_WARNING_OBJECT (self, "Overlay output is changed to nv12\n");
  }
/* *INDENT-ON* */
#endif

  gchar *color_name = gst_video_colorimetry_to_string (&self->main_colorimetry);
  if (color_name != NULL)
    gst_structure_set (s, "colorimetry", G_TYPE_STRING, color_name, NULL);
  g_free (color_name);


  gst_structure_set (s, "format", G_TYPE_STRING,
      gst_video_format_to_string (self->out_pixfmt), NULL);

  ret = gst_caps_fixate (ret);
  return ret;
}

static gboolean
gst_niquadraoverlay_negotiated_caps (GstAggregator * agg, GstCaps * caps)
{
  GST_DEBUG_OBJECT (agg, "Negotiated caps %" GST_PTR_FORMAT, caps);
  return GST_AGGREGATOR_CLASS (parent_class)->negotiated_src_caps (agg, caps);
}

static GstFlowReturn
gst_niquadraoverlay_create_output_buffer (GstVideoAggregator * vagg,
    GstBuffer ** outbuf)
{
  GstNiQuadraOverlay *self = GST_NIQUADRAOVERLAY (vagg);
  GstVideoAggregatorPad *pad;
  GstBuffer *main_frame, *overlay_frame;

  GST_OBJECT_LOCK (vagg);

  main_frame = overlay_frame = NULL;

  pad = g_list_nth (GST_ELEMENT (vagg)->sinkpads, 0)->data;
  main_frame = gst_video_aggregator_pad_get_current_buffer (pad);

  pad = g_list_nth (GST_ELEMENT (vagg)->sinkpads, 1)->data;
  overlay_frame = gst_video_aggregator_pad_get_current_buffer (pad);

  if (self->shortest && (!main_frame || !overlay_frame)) {
    GST_OBJECT_UNLOCK (vagg);
    return GST_FLOW_EOS;
  }

  if ((self->eof_action == EOF_ACTION_PASS) && !main_frame) {
    GST_OBJECT_UNLOCK (vagg);
    return GST_FLOW_EOS;
  }

  if ((self->eof_action == EOF_ACTION_PASS) && !overlay_frame) {
    self->pass_main_frame = TRUE;
  }

  if (self->inplace || self->pass_main_frame) {
    if (main_frame) {
      *outbuf = gst_buffer_ref (main_frame);
    } else {
      *outbuf = gst_buffer_ref (self->last_main_buff);
    }

    GST_OBJECT_UNLOCK (vagg);
    return GST_FLOW_OK;
  }

  GST_OBJECT_UNLOCK (vagg);

  *outbuf = gst_buffer_new ();
  return GST_FLOW_OK;
}

static GstFlowReturn
process_frame (GstVideoAggregator * vagg, GstBuffer * outbuf)
{
  GstNiQuadraOverlay *self = GST_NIQUADRAOVERLAY (vagg);
  GstVideoAggregatorPad *pad;
  int flags;
  ni_pix_fmt_t main_ni_pixfmt, overlay_ni_pixfmt;
  int main_gc620_pixfmt, overlay_gc620_pixfmt;
  GstAllocator *alloc = NULL;
  GstMemory *out_mem = NULL;
  gint main_dev_idx, overlay_dev_idx;
  GstBuffer *main_frame, *overlay_frame;
  GstMemory *main_mem, *overlay_mem;
  niFrameSurface1_t *main_surface, *overlay_surface, *out_surface;
  ni_session_data_io_t *p_session_data;
  int retval = 0;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  uint16_t options = 0;

  GST_OBJECT_LOCK (vagg);

  main_frame = overlay_frame = NULL;

  pad = g_list_nth (GST_ELEMENT (vagg)->sinkpads, 0)->data;
  main_frame = gst_video_aggregator_pad_get_current_buffer (pad);

  pad = g_list_nth (GST_ELEMENT (vagg)->sinkpads, 1)->data;
  overlay_frame = gst_video_aggregator_pad_get_current_buffer (pad);

  if (self->eof_action == EOF_ACTION_REPEAT) {
    if (main_frame) {
      if (self->last_main_buff) {
        gst_buffer_unref (self->last_main_buff);
      }
      self->last_main_buff = gst_buffer_ref (main_frame);
    } else {
      main_frame = self->last_main_buff;
    }

    if (overlay_frame) {
      if (self->last_overlay_buff) {
        gst_buffer_unref (self->last_overlay_buff);
      }
      self->last_overlay_buff = gst_buffer_ref (overlay_frame);
    } else {
      overlay_frame = self->last_overlay_buff;
    }
  }

  main_mem = gst_buffer_peek_memory (main_frame, 0);
  main_dev_idx = gst_deviceid_from_ni_hw_memory (main_mem);
  main_surface = gst_surface_from_ni_hw_memory (main_mem);
  if (main_surface == NULL) {
    GST_ERROR_OBJECT (self,
        "Impossible to convert between the formats supported by the filter");
    flow_ret = GST_FLOW_ERROR;
    if (main_frame) {
      gst_buffer_unref (main_frame);
    }
    goto EXIT;
  }

  overlay_mem = gst_buffer_peek_memory (overlay_frame, 0);
  overlay_dev_idx = gst_deviceid_from_ni_hw_memory (overlay_mem);
  overlay_surface = gst_surface_from_ni_hw_memory (overlay_mem);
  if (overlay_surface == NULL) {
    GST_ERROR_OBJECT (self,
        "Impossible to convert between the formats supported by the filter");
    flow_ret = GST_FLOW_ERROR;
    if (overlay_frame) {
      gst_buffer_unref (overlay_frame);
    }
    goto EXIT;
  }

  if (main_dev_idx != overlay_dev_idx) {
    flow_ret = GST_FLOW_ERROR;
    goto EXIT;
  }

  main_ni_pixfmt = convertGstVideoFormatToNIPix (self->main_pixfmt);
  main_gc620_pixfmt = convertNIPixToGC620Format (main_ni_pixfmt);

  overlay_ni_pixfmt = convertGstVideoFormatToNIPix (self->overlay_pixfmt);
  overlay_gc620_pixfmt = convertNIPixToGC620Format (overlay_ni_pixfmt);

  if (!self->initialized) {
    self->api_ctx = calloc (1, sizeof (*self->api_ctx));
    if (!self->api_ctx) {
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }

    retval = ni_device_session_context_init (self->api_ctx);
    if (retval < 0) {
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }

    self->api_ctx->hw_id = main_dev_idx;
    self->api_ctx->device_type = NI_DEVICE_TYPE_SCALER;
    self->api_ctx->scaler_operation = NI_SCALER_OPCODE_OVERLAY;
    self->api_ctx->keep_alive_timeout = self->keep_alive_timeout;
    self->api_ctx->isP2P = self->is_p2p;

    retval = ni_device_session_open (self->api_ctx, NI_DEVICE_TYPE_SCALER);
    if (retval < 0) {
      flow_ret = GST_FLOW_ERROR;
      ni_device_session_close (self->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
      ni_device_session_context_clear (self->api_ctx);
      goto EXIT;
    }

    memset (&self->api_dst_frame, 0, sizeof (ni_session_data_io_t));
    memset (&self->crop_api_dst_frame, 0, sizeof (ni_session_data_io_t));

    int pool_size = DEFAULT_NI_FILTER_POOL_SIZE;
    if (self->api_ctx->isP2P) {
      pool_size = 1;
    } else {
      // If the downstream element is on a different card,
      // Allocate extra frames suggested by the downstream element
      if (self->api_ctx->hw_id != self->downstream_card) {
        pool_size += self->extra_frames;
        GST_INFO_OBJECT (self, "Increase frame pool by %d", self->extra_frames);
      }
    }
    options = 0;
    options = NI_SCALER_FLAG_IO | NI_SCALER_FLAG_PC;
    if (self->is_p2p) {
      options |= NI_SCALER_FLAG_P2;
    }

    /* Allocate a pool of frames by the scaler */
    /* *INDENT-OFF* */
    retval = ni_device_alloc_frame (self->api_ctx,
      NI_ALIGN (self->out_width, 2),
      NI_ALIGN (self->out_height, 2),
      main_gc620_pixfmt,
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
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }

    self->initialized = TRUE;
  }

  /* Allocate a ni_frame for the overlay output */
  retval = ni_frame_buffer_alloc_hwenc (&self->api_dst_frame.data.frame,
      self->out_width, self->out_height, 0);
  if (retval != NI_RETCODE_SUCCESS) {
    flow_ret = GST_FLOW_ERROR;
    goto EXIT;
  }


  /*
   * Assign an input frame for overlay picture. Send the
   * incoming hardware frame index to the scaler manager.
   */
  options = 0;
  options |= (overlay_surface->encoding_type == 2) ? NI_SCALER_FLAG_CMP : 0;
  retval = ni_device_alloc_frame (self->api_ctx,
      NI_ALIGN (self->overlay_width, 2),
      NI_ALIGN (self->overlay_height, 2),
      overlay_gc620_pixfmt,
      options,
      NI_ALIGN (self->overlay_width, 2),
      NI_ALIGN (self->overlay_height, 2), self->x, self->y, 0,
      overlay_surface->ui16FrameIdx, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    flow_ret = GST_FLOW_ERROR;
    goto EXIT;
  }

  /*
   * Allocate device output frame from the pool. We also send down the frame index
   * of the background frame to the scaler manager.
   */
  flags = (self->alpha_format ? NI_SCALER_FLAG_PA : 0) | NI_SCALER_FLAG_IO;
  flags |= (main_surface->encoding_type == 2) ? NI_SCALER_FLAG_CMP : 0;
  retval =
      ni_device_alloc_frame (self->api_ctx, NI_ALIGN (self->out_width, 2),
      NI_ALIGN (self->out_height, 2), main_gc620_pixfmt, flags,
      NI_ALIGN (self->out_width, 2), NI_ALIGN (self->out_height, 2), 0, 0, 0,
      main_surface->ui16FrameIdx, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    flow_ret = GST_FLOW_ERROR;
    goto EXIT;
  }

  /* Set the new frame index */
  retval = ni_device_session_read_hwdesc (self->api_ctx, &self->api_dst_frame,
      NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    flow_ret = GST_FLOW_ERROR;
    goto EXIT;
  }

  p_session_data = &self->api_dst_frame;
  out_surface = (niFrameSurface1_t *) p_session_data->data.frame.p_data[3];
  out_surface->dma_buf_fd = 0;
  out_surface->ui32nodeAddress = 0;
  out_surface->ui16width = self->out_width;
  out_surface->ui16height = self->out_height;

  ni_set_bit_depth_and_encoding_type (&out_surface->bit_depth,
      &out_surface->encoding_type, main_ni_pixfmt);

  alloc = gst_allocator_find (GST_NIQUADRA_MEMORY_TYPE_NAME);
  out_mem = gst_niquadra_allocator_alloc (alloc, self->api_ctx, out_surface,
      self->api_ctx->hw_id, &vagg->info);
  gst_buffer_append_memory (outbuf, out_mem);
  gst_object_unref (alloc);

EXIT:
  GST_OBJECT_UNLOCK (vagg);
  return flow_ret;
}

static gboolean
overlay_intersects_background (GstNiQuadraOverlay * self)
{
  if (self->x >= self->main_width)
    return FALSE;

  if (self->y >= self->main_height)
    return FALSE;

  if (self->x + self->overlay_width <= 0)
    return FALSE;

  if (self->y + self->overlay_height <= 0)
    return FALSE;

  return TRUE;
}

static void
calculate_src_rectangle (int *px,
    int *py,
    int *pw,
    int *ph,
    int bgnd_x,
    int bgnd_y,
    int bgnd_w, int bgnd_h, int ovly_x, int ovly_y, int ovly_w, int ovly_h)
{
  *px = (ovly_x > 0) ? 0 : -ovly_x;
  *py = (ovly_y > 0) ? 0 : -ovly_y;

  if (ovly_x > 0) {
    *pw = ni_min (bgnd_w - ovly_x, ovly_w);
  } else {
    *pw = ni_min (ovly_w + ovly_x, bgnd_w);
  }

  if (ovly_y > 0) {
    *ph = ni_min (bgnd_h - ovly_y, ovly_h);
  } else {
    *ph = ni_min (ovly_h + ovly_y, bgnd_h);
  }
}

static void
calculate_dst_rectangle (int *px,
    int *py,
    int *pw,
    int *ph,
    int bgnd_x,
    int bgnd_y,
    int bgnd_w, int bgnd_h, int ovly_x, int ovly_y, int ovly_w, int ovly_h)
{
  *px = ni_max (0, ovly_x);
  *py = ni_max (0, ovly_y);

  if (ovly_x > 0) {
    *pw = ni_min (bgnd_w - ovly_x, ovly_w);
  } else {
    *pw = ni_min (ovly_w + ovly_x, bgnd_w);
  }

  if (ovly_y > 0) {
    *ph = ni_min (bgnd_h - ovly_y, ovly_h);
  } else {
    *ph = ni_min (ovly_h + ovly_y, bgnd_h);
  }
}

static gboolean
do_intermediate_crop_and_overlay (GstVideoAggregator * vagg,
    GstBuffer * main_frame, GstBuffer * overlay_frame)
{
  GstNiQuadraOverlay *self = GST_NIQUADRAOVERLAY (vagg);

  GstMemory *main_mem, *overlay_mem;
  niFrameSurface1_t *main_surface, *overlay_surface, *frame_surface;
  ni_pix_fmt_t main_ni_pixfmt, overlay_ni_pixfmt, ni_pixfmt;
  int main_gc620_pixfmt, overlay_gc620_pixfmt, gc620_pixfmt;
  int crop_x, crop_y, crop_w, crop_h;
  int src_x, src_y, src_w, src_h;
  int flags;
  int retval = 0;

  main_ni_pixfmt = convertGstVideoFormatToNIPix (self->main_pixfmt);
  main_gc620_pixfmt = convertNIPixToGC620Format (main_ni_pixfmt);

  overlay_ni_pixfmt = convertGstVideoFormatToNIPix (self->overlay_pixfmt);
  overlay_gc620_pixfmt = convertNIPixToGC620Format (overlay_ni_pixfmt);

  /* Allocate a ni_frame_t for the intermediate crop operation */
  retval = ni_frame_buffer_alloc_hwenc (&self->crop_api_dst_frame.data.frame,
      self->overlay_width, self->overlay_height, 0);
  if (retval != NI_RETCODE_SUCCESS) {
    return FALSE;
  }

  calculate_dst_rectangle (&crop_x, &crop_y, &crop_w, &crop_h,
      0, 0, self->main_width, self->main_height,
      NI_ALIGN (self->x, 2), NI_ALIGN (self->y, 2),
      self->overlay_width, self->overlay_height);

  main_mem = gst_buffer_peek_memory (main_frame, 0);
  main_surface = gst_surface_from_ni_hw_memory (main_mem);

  /* Assign a device input frame. Send incoming frame index to crop session */
  retval = ni_device_alloc_frame (self->crop_api_ctx,
      NI_ALIGN (self->main_width, 2),
      NI_ALIGN (self->main_height, 2),
      main_gc620_pixfmt,
      0,
      crop_w,
      crop_h,
      crop_x, crop_y, 0, main_surface->ui16FrameIdx, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    return FALSE;
  }

  ni_pixfmt = convertGstVideoFormatToNIPix (GST_VIDEO_FORMAT_RGBA);
  gc620_pixfmt = convertNIPixToGC620Format (ni_pixfmt);

  /* Allocate destination frame. This acquires a frame from the pool */
  retval = ni_device_alloc_frame (self->crop_api_ctx,
      NI_ALIGN (self->overlay_width, 2),
      NI_ALIGN (self->overlay_height, 2),
      gc620_pixfmt,
      NI_SCALER_FLAG_IO, 0, 0, 0, 0, 0, -1, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    return FALSE;
  }

  retval = ni_device_session_read_hwdesc (self->crop_api_ctx,
      &self->crop_api_dst_frame, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    return FALSE;
  }

  /* Get the acquired frame */
  frame_surface = (niFrameSurface1_t *)
      self->crop_api_dst_frame.data.frame.p_data[3];
  self->ui16CropFrameIdx = frame_surface->ui16FrameIdx;

  /* Overlay the icon over the intermediate cropped frame */

  /* Allocate a ni_frame_t for the intermediate overlay */
  retval = ni_frame_buffer_alloc_hwenc (&self->api_dst_frame.data.frame,
      self->overlay_width, self->overlay_height, 0);
  if (retval < 0) {
    return FALSE;
  }

  calculate_src_rectangle (&src_x, &src_y, &src_w, &src_h,
      0, 0, self->main_width, self->main_width,
      NI_ALIGN (self->x, 2), NI_ALIGN (self->y, 2),
      self->overlay_width, self->overlay_height);

  overlay_mem = gst_buffer_peek_memory (overlay_frame, 0);
  overlay_surface = gst_surface_from_ni_hw_memory (overlay_mem);

  /* Assign input frame to intermediate overlay session */
  retval = ni_device_alloc_frame (self->api_ctx,
      NI_ALIGN (self->overlay_width, 2),
      NI_ALIGN (self->overlay_height, 2),
      overlay_gc620_pixfmt,
      0,
      src_w,
      src_h,
      src_x, src_y, 0, overlay_surface->ui16FrameIdx, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    return FALSE;
  }

  /* In-place overlay frame. Send down frame index of background frame */
  flags = NI_SCALER_FLAG_IO;    /* Configure output */
  flags |= self->alpha_format ? NI_SCALER_FLAG_PA : 0;  /* Premultiply/straight */

  retval = ni_device_alloc_frame (self->api_ctx,
      NI_ALIGN (self->overlay_width, 2),
      NI_ALIGN (self->overlay_height, 2),
      gc620_pixfmt,
      flags,
      crop_w, crop_h, 0, 0, 0, self->ui16CropFrameIdx, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    return FALSE;
  }

  retval = ni_device_session_read_hwdesc (self->api_ctx,
      &self->api_dst_frame, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    return FALSE;
  }

  return TRUE;
}

static GstFlowReturn
process_frame_inplace (GstVideoAggregator * vagg, GstBuffer * outbuf)
{
  GstNiQuadraOverlay *self = GST_NIQUADRAOVERLAY (vagg);
  GstVideoAggregatorPad *pad;
  int flags;
  ni_pix_fmt_t main_ni_pixfmt, overlay_ni_pixfmt, ni_pixfmt;
  int main_gc620_pixfmt, overlay_gc620_pixfmt, gc620_pixfmt;
  gint main_dev_idx, overlay_dev_idx;
  GstBuffer *main_frame, *overlay_frame;
  GstMemory *main_mem, *overlay_mem;
  niFrameSurface1_t *main_surface, *overlay_surface;
  int src_x, src_y, src_w, src_h;
  int dst_x, dst_y, dst_w, dst_h;
  int retval = 0;
  GstFlowReturn flow_ret = GST_FLOW_OK;

  GST_OBJECT_LOCK (vagg);

  main_frame = overlay_frame = NULL;

  pad = g_list_nth (GST_ELEMENT (vagg)->sinkpads, 0)->data;
  main_frame = gst_video_aggregator_pad_get_current_buffer (pad);

  pad = g_list_nth (GST_ELEMENT (vagg)->sinkpads, 1)->data;
  overlay_frame = gst_video_aggregator_pad_get_current_buffer (pad);

  if (self->eof_action == EOF_ACTION_REPEAT) {
    if (main_frame) {
      if (self->last_main_buff) {
        gst_buffer_unref (self->last_main_buff);
      }
      self->last_main_buff = gst_buffer_ref (main_frame);
    } else {
      main_frame = self->last_main_buff;
    }

    if (overlay_frame) {
      if (self->last_overlay_buff) {
        gst_buffer_unref (self->last_overlay_buff);
      }
      self->last_overlay_buff = gst_buffer_ref (overlay_frame);
    } else {
      overlay_frame = self->last_overlay_buff;
    }
  }

  main_mem = gst_buffer_peek_memory (main_frame, 0);
  main_dev_idx = gst_deviceid_from_ni_hw_memory (main_mem);
  main_surface = gst_surface_from_ni_hw_memory (main_mem);
  if (main_surface == NULL) {
    flow_ret = GST_FLOW_ERROR;
    goto EXIT;
  }

  overlay_mem = gst_buffer_peek_memory (overlay_frame, 0);
  overlay_dev_idx = gst_deviceid_from_ni_hw_memory (overlay_mem);
  overlay_surface = gst_surface_from_ni_hw_memory (overlay_mem);
  if (overlay_surface == NULL) {
    flow_ret = GST_FLOW_ERROR;
    goto EXIT;
  }

  if (main_dev_idx != overlay_dev_idx) {
    flow_ret = GST_FLOW_ERROR;
    goto EXIT;
  }

  main_ni_pixfmt = convertGstVideoFormatToNIPix (self->main_pixfmt);
  main_gc620_pixfmt = convertNIPixToGC620Format (main_ni_pixfmt);

  overlay_ni_pixfmt = convertGstVideoFormatToNIPix (self->overlay_pixfmt);
  overlay_gc620_pixfmt = convertNIPixToGC620Format (overlay_ni_pixfmt);

  // If overlay does not intersect the background, pass
  // the frame through the overlay filter.
  if (!overlay_intersects_background (self)) {
    flow_ret = GST_FLOW_OK;
    goto EXIT;
  }

  if (!self->initialized) {
    self->api_ctx = calloc (1, sizeof (*self->api_ctx));
    if (!self->api_ctx) {
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }

    if (self->overlay_has_alpha && !self->main_has_alpha) {
      self->crop_api_ctx = calloc (1, sizeof (*self->api_ctx));
      if (!self->crop_api_ctx) {
        flow_ret = GST_FLOW_ERROR;
        goto EXIT;
      }
    }

    /* Set up a scaler session for the in-place overlay */
    retval = ni_device_session_context_init (self->api_ctx);
    if (retval < 0) {
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }

    self->api_ctx->hw_id = main_dev_idx;
    self->api_ctx->device_type = NI_DEVICE_TYPE_SCALER;
    self->api_ctx->scaler_operation = NI_SCALER_OPCODE_IPOVLY;
    self->api_ctx->keep_alive_timeout = self->keep_alive_timeout;

    retval = ni_device_session_open (self->api_ctx, NI_DEVICE_TYPE_SCALER);
    if (retval < 0) {
      flow_ret = GST_FLOW_ERROR;
      ni_device_session_close (self->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
      ni_device_session_context_clear (self->api_ctx);
      goto EXIT;
    }
    // If the in-place overlay is rgba over yuv, we need to set up
    // an extra intermediate crop session.
    if (self->overlay_has_alpha && !self->main_has_alpha) {
      /* Set up a scaler session for the crop operation */
      retval = ni_device_session_context_init (self->crop_api_ctx);
      if (retval < 0) {
        flow_ret = GST_FLOW_ERROR;
        goto EXIT;
      }

      self->crop_api_ctx->hw_id = main_dev_idx;
      self->crop_api_ctx->device_type = NI_DEVICE_TYPE_SCALER;
      self->crop_api_ctx->scaler_operation = NI_SCALER_OPCODE_CROP;
      self->crop_api_ctx->keep_alive_timeout = self->keep_alive_timeout;

      retval = ni_device_session_open (self->crop_api_ctx,
          NI_DEVICE_TYPE_SCALER);
      if (retval < 0) {
        flow_ret = GST_FLOW_ERROR;
        ni_device_session_close (self->crop_api_ctx, 1, NI_DEVICE_TYPE_SCALER);
        ni_device_session_context_clear (self->crop_api_ctx);
        goto EXIT;
      }

      int pool_size = DEFAULT_NI_FILTER_POOL_SIZE;
      if (self->is_p2p) {
        pool_size = 1;
      }

      int options = NI_SCALER_FLAG_IO | NI_SCALER_FLAG_PC;
      if (self->is_p2p) {
        options |= NI_SCALER_FLAG_P2;
      }

      ni_pixfmt = convertGstVideoFormatToNIPix (GST_VIDEO_FORMAT_RGBA);
      gc620_pixfmt = convertNIPixToGC620Format (ni_pixfmt);

      /* Allocate a pool of frames by the scaler */
      /* *INDENT-OFF* */
      retval = ni_device_alloc_frame (self->api_ctx,
          NI_ALIGN (self->overlay_width, 2),
          NI_ALIGN (self->overlay_height, 2),
          gc620_pixfmt,
          options,
          0,  // rec width
          0,                    // rec height
          0,                    // rec X pos
          0,                    // rec Y pos
          pool_size,            // rgba color/pool size
          0,                    // frame index
          NI_DEVICE_TYPE_SCALER);
      /* *INDENT-ON* */
      if (retval < 0) {
        flow_ret = GST_FLOW_ERROR;
        goto EXIT;
      }
    }

    self->initialized = TRUE;
  }

  /* For rgba over yuv, we do an intermediate crop and overlay */
  if (self->overlay_has_alpha && !self->main_has_alpha) {
    if (!do_intermediate_crop_and_overlay (vagg, main_frame, overlay_frame)) {
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }

    /* Allocate a ni_frame for the overlay output */
    retval = ni_frame_buffer_alloc_hwenc (&self->api_dst_frame.data.frame,
        self->out_width, self->out_height, 0);

    if (retval != NI_RETCODE_SUCCESS) {
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }

    ni_pixfmt = convertGstVideoFormatToNIPix (GST_VIDEO_FORMAT_RGBA);
    gc620_pixfmt = convertNIPixToGC620Format (ni_pixfmt);

    calculate_src_rectangle (&src_x, &src_y, &src_w, &src_h,
        0, 0, self->main_width, self->main_height,
        NI_ALIGN (self->x, 2), NI_ALIGN (self->y, 2),
        self->overlay_width, self->overlay_height);
    /*
     * Assign an input frame for overlay picture. Send the
     * incoming hardware frame index to the scaler manager.
     */
    /* *INDENT-OFF* */
    retval = ni_device_alloc_frame (self->api_ctx,
        NI_ALIGN (self->overlay_width, 2),  // ovly width
        NI_ALIGN (self->overlay_height, 2),     // ovly height
        gc620_pixfmt,           // ovly pix fmt
        0,                      // flags
        src_w,                  // src rect width
        src_h,                  // src rect height
        0,                      // src rect x
        0,                      // src rect y
        0,                      // n/a
        self->ui16CropFrameIdx, // ovly frame idx
        NI_DEVICE_TYPE_SCALER);
    /* *INDENT-ON* */

    if (retval != NI_RETCODE_SUCCESS) {
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }

    calculate_dst_rectangle (&dst_x, &dst_y, &dst_w, &dst_h,
        0, 0, self->main_width, self->main_height,
        NI_ALIGN (self->x, 2), NI_ALIGN (self->y, 2),
        self->overlay_width, self->overlay_height);

    /*
     * Allocate device output frame from the pool. We also send down the
     * frame index of the background frame to the scaler manager.
     */
    /* configure the output */
    flags = NI_SCALER_FLAG_IO;
    /* premultiply vs straight alpha */
    flags |= (self->alpha_format) ? NI_SCALER_FLAG_PA : 0;

    /* *INDENT-OFF* */
    retval = ni_device_alloc_frame (self->api_ctx,
        NI_ALIGN (self->main_width, 2),     // main width
        NI_ALIGN (self->main_height, 2),    // main height
        main_gc620_pixfmt,                  // main pix fmt
        flags,                              // flags
        dst_w,                              // dst rect width
        dst_h,                              // dst rect height
        dst_x,                              // dst rect x
        dst_y,                              // dst rect y
        0,                                  // n/a
        main_surface->ui16FrameIdx,         // main frame idx
        NI_DEVICE_TYPE_SCALER);
    /* *INDENT-ON* */
    if (retval != NI_RETCODE_SUCCESS) {
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }

    /* Set the new frame index */
    retval = ni_device_session_read_hwdesc (self->api_ctx,
        &self->api_dst_frame, NI_DEVICE_TYPE_SCALER);

    if (retval != NI_RETCODE_SUCCESS) {
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }
  } else {

    /* Not rgba over yuv. For yuv over yuv, yuv over rgba, */
    /* rgba over rgba, we can perform an in-place overlay immediately. */

    /* Allocate ni_frame for the overlay output */
    retval = ni_frame_buffer_alloc_hwenc (&self->api_dst_frame.data.frame,
        self->out_width, self->out_height, 0);

    if (retval != NI_RETCODE_SUCCESS) {
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }

    calculate_src_rectangle (&src_x, &src_y, &src_w, &src_h,
        0, 0, self->main_width, self->main_height,
        NI_ALIGN (self->x, 2), NI_ALIGN (self->y, 2),
        self->overlay_width, self->overlay_height);

    /*
     * Assign input frame for overlay picture. Sends the
     * incoming hardware frame index to the scaler manager.
     */
    /* *INDENT-OFF* */
    retval = ni_device_alloc_frame (self->api_ctx,
        NI_ALIGN (self->overlay_width, 2),  // overlay width
        NI_ALIGN (self->overlay_height, 2),     // overlay height
        overlay_gc620_pixfmt,   // overlay pix fmt
        0,                      // flags
        src_w,                  // src rect width
        src_h,                  // src rect height
        src_x,                  // src rect x
        src_y,                  // src rect y
        0,                      // n/a
        overlay_surface->ui16FrameIdx,    // overlay frame idx
        NI_DEVICE_TYPE_SCALER);
    /* *INDENT-ON* */

    if (retval != NI_RETCODE_SUCCESS) {
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }

    /* In-place overlay frame. Send down frame index of background frame */

    /* Configure the output */
    flags = NI_SCALER_FLAG_IO;
    /* Premultiply vs straight alpha */
    flags |= self->alpha_format ? NI_SCALER_FLAG_PA : 0;

    calculate_dst_rectangle (&dst_x, &dst_y, &dst_w, &dst_h,
        0, 0, self->main_width, self->main_height,
        NI_ALIGN (self->x, 2), NI_ALIGN (self->y, 2),
        self->overlay_width, self->overlay_height);

    /* *INDENT-OFF* */
    retval = ni_device_alloc_frame (self->api_ctx,
        NI_ALIGN (self->main_width, 2),     // main width
        NI_ALIGN (self->main_height, 2),    // main height
        main_gc620_pixfmt,                  // main pix fmt
        flags,                              // flags
        dst_w,                              // dst rect width
        dst_h,                              // dst rect height
        dst_x,                              // dst rect x
        dst_y,                              // dst rect y
        0,                                  // n/a
        main_surface->ui16FrameIdx,         // main frame idx
        NI_DEVICE_TYPE_SCALER);
    /* *INDENT-ON* */
    if (retval != NI_RETCODE_SUCCESS) {
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }

    retval =
        ni_device_session_read_hwdesc (self->api_ctx, &self->api_dst_frame,
        NI_DEVICE_TYPE_SCALER);

    if (retval != NI_RETCODE_SUCCESS) {
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }
  }

EXIT:
  GST_OBJECT_UNLOCK (vagg);
  return flow_ret;
}

static GstFlowReturn
gst_niquadraoverlay_aggregate_frames (GstVideoAggregator * vagg,
    GstBuffer * outbuf)
{
  GstNiQuadraOverlay *self = GST_NIQUADRAOVERLAY (vagg);

  if (self->pass_main_frame) {
    return GST_FLOW_OK;
  }

  if (self->inplace) {
    return process_frame_inplace (vagg, outbuf);
  } else {
    return process_frame (vagg, outbuf);
  }
}

static void
gst_niquadraoverlay_dispose (GObject * obj)
{
  GstNiQuadraOverlay *self = GST_NIQUADRAOVERLAY (obj);

  if (self->api_dst_frame.data.frame.p_buffer) {
    ni_frame_buffer_free (&self->api_dst_frame.data.frame);
  }

  if (self->api_ctx) {
    if (self->api_ctx->session_id != NI_INVALID_SESSION_ID) {
      ni_device_session_close (self->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
    }
    ni_session_context_t *p_ctx = self->api_ctx;
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

    ni_device_session_context_clear (self->api_ctx);
    free (self->api_ctx);
  }

  if (self->crop_api_dst_frame.data.frame.p_buffer) {
    ni_frame_buffer_free (&self->crop_api_dst_frame.data.frame);
  }

  if (self->crop_api_ctx) {
    if (self->crop_api_ctx->session_id != NI_INVALID_SESSION_ID) {
      ni_device_session_close (self->crop_api_ctx, 1, NI_DEVICE_TYPE_SCALER);
    }

    ni_device_session_context_clear (self->crop_api_ctx);
    free (self->crop_api_ctx);
  }

  if (self->eof_action_str) {
    g_free (self->eof_action_str);
  }
  if (self->alpha) {
    g_free (self->alpha);
  }

  G_OBJECT_CLASS (parent_class)->dispose (obj);
}

static gboolean
gst_niquadraoverlay_stop (GstAggregator * agg)
{
  GstNiQuadraOverlay *self = GST_NIQUADRAOVERLAY (agg);

  if (self->last_main_buff) {
    gst_buffer_unref (self->last_main_buff);
  }
  if (self->last_overlay_buff) {
    gst_buffer_unref (self->last_overlay_buff);
  }

  return GST_AGGREGATOR_CLASS (parent_class)->stop (agg);
}

static gboolean
gst_niquadraoverlay_decide_allocation (GstAggregator * agg, GstQuery * query)
{
  gboolean gotit = FALSE;
  guint i;
  GType gtype;
  const GstStructure *params = NULL;
  GstNiQuadraOverlay *self = GST_NIQUADRAOVERLAY (agg);

  /* Search for allocation metadata */
  for (i = 0; i < gst_query_get_n_allocation_metas (query); i++) {
    gtype = gst_query_parse_nth_allocation_meta (query, i, &params);
    if (gtype == GST_VIDEO_META_API_TYPE) {
      if (params && (strcmp (gst_structure_get_name (params),
                  NI_PREALLOCATE_STRUCTURE_NAME) == 0)) {

        gotit = gst_structure_get_uint (params, NI_VIDEO_META_BUFCNT,
            &self->extra_frames);
        if (gotit == FALSE)
          GST_ERROR_OBJECT (self, "Did not find buffercnt\n");

        gotit = gst_structure_get_int (params, NI_VIDEO_META_CARDNO,
            &self->downstream_card);
        if (gotit == FALSE)
          GST_ERROR_OBJECT (self, "Did not find cardno\n");

        break;
      }
    }
  }

  return TRUE;
}

static void
gst_niquadraoverlay_class_init (GstNiQuadraOverlayClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;
  GstVideoAggregatorClass *videoaggregator_class =
      (GstVideoAggregatorClass *) klass;
  GstAggregatorClass *agg_class = (GstAggregatorClass *) klass;

  gobject_class->get_property = gst_niquadraoverlay_get_property;
  gobject_class->set_property = gst_niquadraoverlay_set_property;

  agg_class->fixate_src_caps = gst_niquadraoverlay_fixate_caps;
  agg_class->negotiated_src_caps = gst_niquadraoverlay_negotiated_caps;
  agg_class->stop = gst_niquadraoverlay_stop;
  agg_class->decide_allocation = gst_niquadraoverlay_decide_allocation;

  videoaggregator_class->aggregate_frames =
      gst_niquadraoverlay_aggregate_frames;
  videoaggregator_class->create_output_buffer =
      gst_niquadraoverlay_create_output_buffer;

  g_object_class_install_property (gobject_class, PROP_X,
      g_param_spec_int ("x", "X",
          "set the x expression", 0, G_MAXINT,
          0, G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_Y,
      g_param_spec_int ("y", "Y",
          "set the y expression", 0, G_MAXINT,
          0, G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_EOF_ACTION,
      g_param_spec_string ("eof-action", "Eof-action",
          "Action to take when encountering EOF from secondary input", "repeat",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT));

  g_object_class_install_property (gobject_class, PROP_SHORTEST,
      g_param_spec_boolean ("shortest", "Shortest",
          "force termination when the shortest input terminates",
          FALSE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

  g_object_class_install_property (gobject_class, PROP_REPEATLAST,
      g_param_spec_boolean ("repeatlast", "Repeatlast",
          "repeat overlay of the last overlay frame",
          TRUE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

  g_object_class_install_property (gobject_class, PROP_ALPHA,
      g_param_spec_string ("alpha", "Alpha",
          "alpha format", "straight",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT));

  g_object_class_install_property (gobject_class, PROP_INPLACE_OVERLAY,
      g_param_spec_boolean ("inplace", "In-place",
          "Perform an in-place overlay",
          FALSE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

  g_object_class_install_property (gobject_class, PROP_IS_P2P,
      g_param_spec_boolean ("is-p2p", "Is-p2p",
          "enable p2p transfer", FALSE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

  g_object_class_install_property (gobject_class, PROP_KEEP_ALIVE_TIMEOUT,
      g_param_spec_uint ("keep-alive-timeout", "Keep-alive-timeout",
          "Specify a custom session keep alive timeout in seconds",
          NI_MIN_KEEP_ALIVE_TIMEOUT, NI_MAX_KEEP_ALIVE_TIMEOUT,
          NI_DEFAULT_KEEP_ALIVE_TIMEOUT,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template_with_gtype (gstelement_class,
      &src_factory, GST_TYPE_AGGREGATOR_PAD);
  gst_element_class_add_static_pad_template_with_gtype (gstelement_class,
      &sink_factory, GST_TYPE_NIQUADRAOVERLAY_PAD);

  gst_element_class_set_static_metadata (gstelement_class,
      "NETINT Quadra OVERLAY filter", "Filter/Effect/Video/OverlayNIQuadra",
      "Hardware Netint Overlay", "Leo Liu<leo.liu@netint.cn>");

  gobject_class->dispose = gst_niquadraoverlay_dispose;
}

static void
gst_niquadraoverlay_init (GstNiQuadraOverlay * self)
{
  self->initialized = FALSE;
  self->api_ctx = NULL;
  self->crop_api_ctx = NULL;
  self->main_has_alpha = FALSE;
  self->overlay_has_alpha = FALSE;
  self->last_main_buff = NULL;
  self->last_overlay_buff = NULL;
  self->pass_main_frame = FALSE;
  self->downstream_card = -1;
  self->extra_frames = 0;
}

/* GstChildProxy implementation */
static GObject *
gst_niquadraoverlay_child_proxy_get_child_by_index (GstChildProxy * child_proxy,
    guint index)
{
  GstNiQuadraOverlay *overlay = GST_NIQUADRAOVERLAY (child_proxy);
  GObject *obj = NULL;

  GST_OBJECT_LOCK (overlay);
  obj = g_list_nth_data (GST_ELEMENT_CAST (overlay)->sinkpads, index);
  if (obj)
    gst_object_ref (obj);
  GST_OBJECT_UNLOCK (overlay);

  return obj;
}

static guint
gst_niquadraoverlay_child_proxy_get_children_count (GstChildProxy * child_proxy)
{
  guint count = 0;
  GstNiQuadraOverlay *overlay = GST_NIQUADRAOVERLAY (child_proxy);

  GST_OBJECT_LOCK (overlay);
  count = GST_ELEMENT_CAST (overlay)->numsinkpads;
  GST_OBJECT_UNLOCK (overlay);

  return count;
}

static void
gst_niquadraoverlay_child_proxy_init (gpointer g_iface, gpointer iface_data)
{
  GstChildProxyInterface *iface = g_iface;

  iface->get_child_by_index =
      gst_niquadraoverlay_child_proxy_get_child_by_index;
  iface->get_children_count =
      gst_niquadraoverlay_child_proxy_get_children_count;
}

static gboolean
niquadraoverlay_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadraoverlay_debug, "niquadraoverlay", 0,
      "niquadraoverlay");

  return gst_element_register (plugin, "niquadraoverlay", GST_RANK_PRIMARY + 1,
      GST_TYPE_NIQUADRAOVERLAY);
}
