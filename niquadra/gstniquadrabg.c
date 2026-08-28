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
 *  \file   gstniquadrabg.c
 *
 *  \brief  Implement of NetInt Quadra hardware background filter.
 ******************************************************************************/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/video.h>

#include <gst/app/app.h>

#include "niquadra.h"
#include "ni_device_api.h"
#include "gstniquadramemory.h"
#include "gstniquadrautils.h"

GST_DEBUG_CATEGORY_STATIC (gst_niquadrabg_debug);
#define GST_CAT_DEFAULT gst_niquadrabg_debug

enum
{
  PROP_0,
  PROP_NB,
  PROP_BG_IMG,
  PROP_USE_DEFAULT_BG,
  PROP_SKIP,
  PROP_IS_P2P,
  PROP_KEEP_ALIVE_TIMEOUT,
  PROP_LAST
};

typedef struct _ni_roi_network_layer
{
  int32_t width;
  int32_t height;
  int32_t channel;
  int32_t classes;
  int32_t component;
  int32_t output_number;
  float *output;
} ni_roi_network_layer_t;

typedef struct _ni_roi_network
{
  int32_t netw;
  int32_t neth;
  ni_network_data_t raw;
  ni_roi_network_layer_t *layers;
} ni_roi_network_t;

typedef struct HwScaleContext
{
  ni_session_context_t api_ctx;
  ni_session_data_io_t api_dst_frame;
} HwScaleContext;

typedef struct AiContext
{
  ni_session_context_t api_ctx;
  ni_session_data_io_t api_dst_pkt;
} AiContext;

typedef struct HwUploadContext
{
  ni_session_context_t api_ctx;
  ni_session_data_io_t api_dst_frame;
} HwUploadContext;

typedef struct OverlayContext
{
  ni_session_context_t api_ctx;
  ni_session_data_io_t api_dst_frame;
} OverlayContext;

typedef struct SwScaleContext
{
  GstElement *pipeline;
  GstElement *appsrc;
  GstElement *appsink;
} SwScaleContext;

typedef struct BgFrame
{
  GstVideoFrame vframe;
  GstBuffer *buffer;
} BgFrame;

typedef struct _GstNIQuadraBg
{
  GstElement element;

  GstPad *sinkpad, *srcpad;

  gchar *nb_file;
  gchar *bg_img;
  guint use_default_bg;
  guint skip;
  gboolean is_p2p;
  guint keep_alive_timeout;

  GstVideoInfo info;
  gint in_width, in_height;
  GstVideoFormat in_pixfmt;
  gint out_width, out_height;
  GstVideoFormat out_pixfmt;

  gboolean initialized;

  //hw scale
  HwScaleContext *hws_ctx;

  //ai
  AiContext *ai_ctx;
  ni_roi_network_t network;

  //sw scale
  SwScaleContext sw_scale_ctx;

  //upload
  HwUploadContext *hwupload_ctx;

  //overlay
  OverlayContext *overlay_ctx;

  BgFrame mask_frame;
  BgFrame alpha_mask_frame;
  BgFrame alpha_enlarge_frame;

  int framecount;

  guint extra_frames;
  gint downstream_card;
} GstNiQuadraBg;

typedef struct _GstNIQuadraBgClass
{
  GstElementClass parent_class;
} GstNiQuadraBgClass;

#define GST_TYPE_NIQUADRABG \
  (gst_niquadrabg_get_type())
#define GST_NIQUADRABG(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRABG,GstNiQuadraBg))
#define GST_NIQUADRABG_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRABG,GstNiQuadraBg))
#define GST_IS_NIQUADRABG(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRABG))
#define GST_IS_NIQUADRABG_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRABG))

static gboolean niquadrabg_element_init (GstPlugin * plugin);

GType gst_niquadrabg_get_type (void);

G_DEFINE_TYPE (GstNiQuadraBg, gst_niquadrabg, GST_TYPE_ELEMENT);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadrabg, niquadrabg_element_init);

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

static gboolean
create_sw_scale_pipeline (GstNiQuadraBg * filter,
    GstVideoFormat in_fmt, GstVideoFormat out_fmt)
{
  GstElement *pipeline = NULL;

  GstElement *appsrc = NULL;
  GstElement *convert = NULL;
  GstElement *scale = NULL;
  GstElement *capsfilter = NULL;
  GstElement *appsink = NULL;

  GstCaps *in_caps = NULL;
  GstCaps *out_caps = NULL;

  pipeline = gst_pipeline_new ("sw-scale-pipeline");
  filter->sw_scale_ctx.pipeline = pipeline;

  appsrc = gst_element_factory_make ("appsrc", NULL);
  filter->sw_scale_ctx.appsrc = appsrc;

  in_caps = gst_caps_new_simple ("video/x-raw",
      "width", G_TYPE_INT, filter->network.netw,
      "height", G_TYPE_INT, filter->network.neth,
      "format", G_TYPE_STRING, gst_video_format_to_string (in_fmt), NULL);
  g_object_set (appsrc, "caps", in_caps, NULL);

  convert = gst_element_factory_make ("videoconvert", NULL);
  scale = gst_element_factory_make ("videoscale", NULL);

  capsfilter = gst_element_factory_make ("capsfilter", NULL);

  out_caps = gst_caps_new_simple ("video/x-raw",
      "width", G_TYPE_INT, filter->in_width,
      "height", G_TYPE_INT, filter->in_height,
      "format", G_TYPE_STRING, gst_video_format_to_string (out_fmt), NULL);
  g_object_set (capsfilter, "caps", out_caps, NULL);

  appsink = gst_element_factory_make ("appsink", NULL);
  filter->sw_scale_ctx.appsink = appsink;

  gst_bin_add_many (GST_BIN (pipeline), appsrc, convert,
      scale, capsfilter, appsink, NULL);

  if (!gst_element_link_many (appsrc, convert, scale, capsfilter, appsink,
          NULL)) {
    GST_ERROR_OBJECT (filter,
        "Elements could not be linked when create sw scale pipeline.\n");
    goto FAIL_EXIT;
  }

  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  gst_caps_unref (in_caps);
  gst_caps_unref (out_caps);

  return TRUE;

FAIL_EXIT:
  if (appsrc) {
    gst_object_unref (appsrc);
  }
  if (convert) {
    gst_object_unref (convert);
  }
  if (scale) {
    gst_object_unref (scale);
  }
  if (capsfilter) {
    gst_object_unref (capsfilter);
  }
  if (appsink) {
    gst_object_unref (appsink);
  }
  if (pipeline) {
    gst_object_unref (pipeline);
  }
  if (in_caps) {
    gst_caps_unref (in_caps);
  }
  if (out_caps) {
    gst_caps_unref (out_caps);
  }
  return FALSE;
}

static void
stop_sw_scale_pipeline (GstNiQuadraBg * filter)
{
  GstElement *pipeline = filter->sw_scale_ctx.pipeline;
  gst_element_set_state (pipeline, GST_STATE_NULL);
}

static void
cleanup_sw_scale_pipeline (GstNiQuadraBg * filter)
{
  if (filter->sw_scale_ctx.pipeline) {
    gst_object_unref (filter->sw_scale_ctx.pipeline);
    filter->sw_scale_ctx.pipeline = NULL;
  }
}

static void
cleanup_ai_context (GstNiQuadraBg * filter)
{
  AiContext *ai_ctx = filter->ai_ctx;

  if (ai_ctx) {
    ni_session_context_t *p_ctx = &ai_ctx->api_ctx;
    ni_packet_buffer_free (&ai_ctx->api_dst_pkt.data.packet);

    ni_device_session_close (&ai_ctx->api_ctx, 1, NI_DEVICE_TYPE_AI);
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
    ni_device_session_context_clear (&ai_ctx->api_ctx);

    g_free (ai_ctx);
    filter->ai_ctx = NULL;
  }
}

static gboolean
init_ai_context (GstNiQuadraBg * filter, gint dev_idx)
{
  ni_roi_network_t *network = &filter->network;
  AiContext *ai_ctx = NULL;
  ni_session_context_t *ctx = NULL;
  int retval = 0;

  if ((!filter->nb_file) ||
      (!g_file_test (filter->nb_file, G_FILE_TEST_EXISTS))) {
    GST_ERROR_OBJECT (filter, "nb_file is invalid or not exsit.\n");
    return FALSE;
  }

  ai_ctx = g_malloc0 (sizeof (AiContext));
  if (!ai_ctx) {
    GST_ERROR_OBJECT (filter, "failed to allocate ai context\n");
    return FALSE;
  }
  filter->ai_ctx = ai_ctx;

  ctx = &ai_ctx->api_ctx;

  retval = ni_device_session_context_init (ctx);
  if (retval != NI_RETCODE_SUCCESS) {
    return FALSE;
  }

  ctx->hw_id = dev_idx;
  ctx->hw_action = NI_CODEC_HW_ENABLE;
  ctx->device_type = NI_DEVICE_TYPE_AI;
  ctx->keep_alive_timeout = filter->keep_alive_timeout;

  retval = ni_device_session_open (ctx, NI_DEVICE_TYPE_AI);
  if (retval != NI_RETCODE_SUCCESS) {
    cleanup_ai_context (filter);
    return FALSE;
  }

  retval = ni_ai_config_network_binary (ctx, &network->raw, filter->nb_file);
  if (retval != NI_RETCODE_SUCCESS) {
    return FALSE;
  }

  retval = ni_ai_packet_buffer_alloc (&ai_ctx->api_dst_pkt.data.packet,
      &network->raw);
  if (retval != NI_RETCODE_SUCCESS) {
    return FALSE;
  }

  return TRUE;
}

static void
ni_destroy_network (GstNiQuadraBg * filter, ni_roi_network_t * network)
{
  if (network && network->layers) {
    int i;

    for (i = 0; i < network->raw.output_num; i++) {
      if (network->layers[i].output) {
        free (network->layers[i].output);
        network->layers[i].output = NULL;
      }
    }

    free (network->layers);
    network->layers = NULL;
  }
}

static gboolean
ni_create_network (GstNiQuadraBg * filter, ni_roi_network_t * network)
{
  ni_network_data_t *ni_network = &network->raw;
  int i;

  if (ni_network->input_num == 0 || ni_network->output_num == 0) {
    return FALSE;
  }

  /* only support one input for now */
  if (ni_network->input_num != 1) {
    return FALSE;
  }

  network->layers =
      g_malloc0 (sizeof (ni_roi_network_layer_t) * ni_network->output_num);
  if (!network->layers) {
    return FALSE;
  }

  for (i = 0; i < ni_network->output_num; i++) {
    network->layers[i].width = ni_network->linfo.out_param[i].sizes[0];
    network->layers[i].height = ni_network->linfo.out_param[i].sizes[1];
    network->layers[i].channel = ni_network->linfo.out_param[i].sizes[2];
    network->layers[i].component = 3;

    network->layers[i].classes =
        (network->layers[i].channel / network->layers[i].component) - (4 + 1);

    network->layers[i].output_number =
        ni_ai_network_layer_dims (&ni_network->linfo.out_param[i]);

    network->layers[i].output =
        g_malloc0 (network->layers[i].output_number * sizeof (float));
    if (!network->layers[i].output) {
      return FALSE;
    }
  }

  network->netw = ni_network->linfo.in_param[0].sizes[0];
  network->neth = ni_network->linfo.in_param[0].sizes[1];

  return TRUE;
}

static gboolean
init_hwframe_scale (GstNiQuadraBg * filter, GstVideoFormat format, gint dev_idx)
{
  HwScaleContext *hws_ctx = NULL;
  ni_session_context_t *ctx = NULL;
  int pool_size = DEFAULT_NI_FILTER_POOL_SIZE;
  int retval = 0;

  hws_ctx = g_malloc0 (sizeof (HwScaleContext));
  if (!hws_ctx) {
    GST_ERROR_OBJECT (filter, "could not allocate hwframe ctx\n");
    return FALSE;
  }
  filter->hws_ctx = hws_ctx;

  ctx = &hws_ctx->api_ctx;
  retval = ni_device_session_context_init (ctx);
  if (retval < 0) {
    return FALSE;
  }

  ctx->hw_id = dev_idx;
  ctx->device_type = NI_DEVICE_TYPE_SCALER;
  ctx->scaler_operation = NI_SCALER_OPCODE_SCALE;
  ctx->keep_alive_timeout = filter->keep_alive_timeout;
  ctx->isP2P = filter->is_p2p;

  retval = ni_device_session_open (ctx, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "could not open scaler session\n");
    ni_device_session_close (ctx, 1, NI_DEVICE_TYPE_SCALER);
    ni_device_session_context_clear (ctx);
    return FALSE;
  }

  /* Create scale frame pool on device */
  retval = ni_build_frame_pool (ctx, filter->network.netw,
      filter->network.neth, format, pool_size);
  if (retval < 0) {
    GST_ERROR_OBJECT (filter, "could not build frame pool, ret=%d\n", retval);
    return FALSE;
  }

  return TRUE;
}

static void
cleanup_hwframe_scale (GstNiQuadraBg * filter)
{
  HwScaleContext *hws_ctx = filter->hws_ctx;

  if (hws_ctx) {
    ni_session_context_t *p_ctx = &hws_ctx->api_ctx;

    ni_frame_buffer_free (&hws_ctx->api_dst_frame.data.frame);
    ni_device_session_close (&hws_ctx->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
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
    ni_device_session_context_clear (&hws_ctx->api_ctx);
    g_free (hws_ctx);
    filter->hws_ctx = NULL;
  }
}

static gboolean
init_hwframe_uploader (GstNiQuadraBg * filter,
    GstVideoFormat format, gint dev_idx)
{
  HwUploadContext *hwupload_ctx = NULL;
  ni_session_context_t *ctx = NULL;
  ni_pix_fmt_t ni_pixfmt;
  int pool_size, retval = 0;

  hwupload_ctx = g_malloc0 (sizeof (HwUploadContext));
  if (!hwupload_ctx) {
    return FALSE;
  }
  filter->hwupload_ctx = hwupload_ctx;

  ctx = &hwupload_ctx->api_ctx;
  retval = ni_device_session_context_init (ctx);
  if (retval < 0) {
    return FALSE;
  }

  ctx->hw_id = dev_idx;
  ctx->keep_alive_timeout = filter->keep_alive_timeout;

  ni_pixfmt = convertGstVideoFormatToNIPix (format);
  ni_uploader_set_frame_format (ctx, filter->in_width,
      filter->in_height, ni_pixfmt, 0);

  retval = ni_device_session_open (ctx, NI_DEVICE_TYPE_UPLOAD);
  if (retval != NI_RETCODE_SUCCESS) {
    ni_device_session_close (ctx, 1, NI_DEVICE_TYPE_UPLOAD);
    ni_device_session_context_clear (ctx);
    return FALSE;
  }

  pool_size = 3;
  retval = ni_device_session_init_framepool (ctx, pool_size, 0);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "Init frame pool error\n");
    return FALSE;
  }

  return TRUE;
}

static void
cleanup_hwframe_uploader (GstNiQuadraBg * filter)
{
  HwUploadContext *hwupload_ctx = filter->hwupload_ctx;

  if (hwupload_ctx) {
    ni_session_context_t *p_ctx = &hwupload_ctx->api_ctx;
    ni_frame_buffer_free (&hwupload_ctx->api_dst_frame.data.frame);
    ni_device_session_close (&hwupload_ctx->api_ctx, 1, NI_DEVICE_TYPE_UPLOAD);
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
    ni_device_session_context_clear (&hwupload_ctx->api_ctx);

    g_free (hwupload_ctx);
    filter->hwupload_ctx = NULL;
  }
}

static gboolean
init_hwframe_overlay (GstNiQuadraBg * filter,
    GstVideoFormat format, gint dev_idx)
{
  OverlayContext *overlay_ctx = NULL;
  ni_session_context_t *ctx = NULL;
  int pool_size = DEFAULT_NI_FILTER_POOL_SIZE;
  int retval = 0;

  overlay_ctx = g_malloc0 (sizeof (OverlayContext));
  if (!overlay_ctx) {
    return FALSE;
  }
  filter->overlay_ctx = overlay_ctx;

  ctx = &overlay_ctx->api_ctx;
  retval = ni_device_session_context_init (ctx);
  if (retval < 0) {
    return FALSE;
  }

  ctx->hw_id = dev_idx;
  ctx->device_type = NI_DEVICE_TYPE_SCALER;
  ctx->scaler_operation = NI_SCALER_OPCODE_OVERLAY;
  ctx->keep_alive_timeout = filter->keep_alive_timeout;
  ctx->isP2P = filter->is_p2p;

  retval = ni_device_session_open (ctx, NI_DEVICE_TYPE_SCALER);
  if (retval < 0) {
    ni_device_session_close (ctx, 1, NI_DEVICE_TYPE_SCALER);
    ni_device_session_context_clear (ctx);
    return FALSE;
  }

  if (ctx->isP2P) {
    pool_size = 1;
  } else {
    // If the downstream element is on a different card,
    // Allocate extra frames suggested by the downstream element
    if (ctx->hw_id != filter->downstream_card) {
      pool_size += filter->extra_frames;
      GST_DEBUG_OBJECT (filter,
          "Increase frame pool by %d\n", filter->extra_frames);
    }
  }

  /* Create scale frame pool on device */
  retval = ni_build_frame_pool (ctx, filter->out_width,
      filter->out_height, format, pool_size);
  if (retval < 0) {
    GST_ERROR_OBJECT (filter,
        "could not build frame pool, retval=%d\n", retval);
    return FALSE;
  }

  return TRUE;
}

static void
cleanup_hwframe_overlay (GstNiQuadraBg * filter)
{
  OverlayContext *overlay_ctx = filter->overlay_ctx;

  if (overlay_ctx) {
    ni_session_context_t *p_ctx = &overlay_ctx->api_ctx;
    ni_frame_buffer_free (&overlay_ctx->api_dst_frame.data.frame);
    ni_device_session_close (&overlay_ctx->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
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
    ni_device_session_context_clear (&overlay_ctx->api_ctx);
    g_free (overlay_ctx);
    filter->overlay_ctx = NULL;
  }
}

static gboolean
ni_hwframe_scale (GstNiQuadraBg * filter, niFrameSurface1_t * src_surface,
    int w, int h, niFrameSurface1_t * dst_surface)
{
  HwScaleContext *scale_ctx = filter->hws_ctx;

  ni_session_context_t *ctx = NULL;
  niFrameSurface1_t *new_frame_surface = NULL;
  int in_format, out_format;
  int retval = 0;

  ctx = &scale_ctx->api_ctx;

  in_format = convertGstVideoFormatToGC620Format (filter->in_pixfmt);

  retval = ni_frame_buffer_alloc_hwenc (&scale_ctx->api_dst_frame.data.frame,
      w, h, 0);
  if (retval != NI_RETCODE_SUCCESS) {
    return FALSE;
  }

  /*
   * Allocate device input frame. This call won't actually allocate a frame,
   * but sends the incoming hardware frame index to the scaler manager
   */
  retval = ni_device_alloc_frame (ctx, NI_ALIGN (filter->in_width, 2),
      NI_ALIGN (filter->in_height, 2), in_format, 0, 0, 0, 0, 0,
      src_surface->ui32nodeAddress, src_surface->ui16FrameIdx,
      NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    return FALSE;
  }

  out_format = convertGstVideoFormatToGC620Format (GST_VIDEO_FORMAT_BGRP);

  /* Allocate hardware device destination frame. This acquires a frame from
   * the pool */
  retval = ni_device_alloc_frame (ctx, NI_ALIGN (w, 2), NI_ALIGN (h, 2),
      out_format, NI_SCALER_FLAG_IO, 0, 0, 0, 0, 0, -1, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    return FALSE;
  }

  /* Set the new frame index */
  ni_device_session_read_hwdesc (ctx, &scale_ctx->api_dst_frame,
      NI_DEVICE_TYPE_SCALER);

  new_frame_surface =
      (niFrameSurface1_t *) scale_ctx->api_dst_frame.data.frame.p_data[3];

  memcpy (dst_surface, new_frame_surface, sizeof (niFrameSurface1_t));

  return TRUE;
}

static gboolean
ni_hwframe_upload (GstNiQuadraBg * filter, BgFrame * sw_frame,
    niFrameSurface1_t * upload_surface)
{
  HwUploadContext *hwupload_ctx = filter->hwupload_ctx;

  ni_session_context_t *ctx = NULL;
  ni_session_data_io_t *p_src_session_data = NULL;
  ni_frame_t *dst_frame = NULL;
  uint8_t *src_data[NI_MAX_NUM_DATA_POINTERS] = { NULL };
  int src_stride[NI_MAX_NUM_DATA_POINTERS] = { 0 };
  int src_height[NI_MAX_NUM_DATA_POINTERS] = { 0 };
  int dst_stride[NI_MAX_NUM_DATA_POINTERS] = { 0 };
  int dst_height[NI_MAX_NUM_DATA_POINTERS] = { 0 };
  GstVideoInfo info;
  GstCaps *caps = NULL;
  bool is_semiplanar = false;
  int pixel_format = NI_PIX_FMT_RGBA;
  int retval = 0;

  ctx = &hwupload_ctx->api_ctx;
  p_src_session_data = &hwupload_ctx->api_dst_frame;

  dst_frame = &p_src_session_data->data.frame;
  dst_frame->extra_data_len = NI_APP_ENC_FRAME_META_DATA_SIZE;

  dst_stride[0] = NI_ALIGN (filter->in_width, 16) * 4;
  dst_height[0] = filter->in_height;

  retval = ni_frame_buffer_alloc_pixfmt (dst_frame, pixel_format,
      filter->in_width, filter->in_height, dst_stride, 1,
      (int) dst_frame->extra_data_len);
  if (retval != NI_RETCODE_SUCCESS) {
    return FALSE;
  }

  caps = gst_caps_new_simple ("video/x-raw",
      "width", G_TYPE_INT, filter->in_width,
      "height", G_TYPE_INT, filter->in_height,
      "format", G_TYPE_STRING, "RGBA", NULL);

  gst_video_info_from_caps (&info, caps);
  gst_video_frame_map (&sw_frame->vframe, &info, sw_frame->buffer,
      GST_MAP_READ);

  src_data[0] = GST_VIDEO_FRAME_PLANE_DATA (&sw_frame->vframe, 0);

  src_stride[0] = GST_VIDEO_FRAME_PLANE_STRIDE (&sw_frame->vframe, 0);
  src_height[0] = filter->in_height;

  ni_copy_hw_yuv420p ((uint8_t **) dst_frame->p_data, src_data,
      filter->in_width, filter->in_height,
      ctx->bit_depth_factor, is_semiplanar, 0,
      dst_stride, dst_height, src_stride, src_height);

  retval = ni_device_session_hwup (ctx, p_src_session_data, upload_surface);
  if (retval < 0) {
    gst_video_frame_unmap (&sw_frame->vframe);
    gst_caps_unref (caps);
    return FALSE;
  }

  upload_surface->ui16width = filter->in_width;
  upload_surface->ui16height = filter->in_height;
  upload_surface->ui32nodeAddress = 0;  // always 0 offset for upload
  upload_surface->encoding_type =
      is_semiplanar ? NI_PIXEL_PLANAR_FORMAT_SEMIPLANAR :
      NI_PIXEL_PLANAR_FORMAT_PLANAR;

  gst_video_frame_unmap (&sw_frame->vframe);
  gst_caps_unref (caps);

  return TRUE;
}

static gboolean
ni_hwframe_overlay (GstNiQuadraBg * filter, niFrameSurface1_t * main,
    niFrameSurface1_t * overlay, niFrameSurface1_t * output)
{
  OverlayContext *overlay_ctx = filter->overlay_ctx;

  ni_session_context_t *ctx = NULL;
  niFrameSurface1_t *new_frame_surface = NULL;
  GstVideoFormat ovly_format = GST_VIDEO_FORMAT_RGBA;
  int main_scaler_format, ovly_scaler_format;
  ni_pix_fmt_t niPixFmt;
  int retval = 0;
  int flags;

  ctx = &overlay_ctx->api_ctx;

  main_scaler_format = convertGstVideoFormatToGC620Format (filter->in_pixfmt);
  ovly_scaler_format = convertGstVideoFormatToGC620Format (ovly_format);

  /* Allocate a ni_frame for the overlay output */
  retval = ni_frame_buffer_alloc_hwenc (&overlay_ctx->api_dst_frame.data.frame,
      filter->out_width, filter->out_height, 0);
  if (retval != NI_RETCODE_SUCCESS) {
    return FALSE;
  }

  /*
   * Allocate device input frame for overlay picture. This call won't actually
   * allocate a frame, but sends the incoming hardware frame index to the
   * scaler manager.
   */
  retval = ni_device_alloc_frame (ctx,
      NI_ALIGN (filter->in_width, 2),
      NI_ALIGN (filter->in_height, 2),
      ovly_scaler_format,
      0,
      NI_ALIGN (filter->in_width, 2),
      NI_ALIGN (filter->in_height, 2),
      0,
      0,
      overlay->ui32nodeAddress, overlay->ui16FrameIdx, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    return FALSE;
  }

  /*
   * Allocate device output frame from the pool. We also send down the frame
   * index of the background frame to the scaler manager.
   */
  flags = NI_SCALER_FLAG_IO;
  retval = ni_device_alloc_frame (ctx,
      NI_ALIGN (filter->out_width, 2),
      NI_ALIGN (filter->out_height, 2),
      main_scaler_format,
      flags,
      NI_ALIGN (filter->out_width, 2),
      NI_ALIGN (filter->out_height, 2),
      0, 0, main->ui32nodeAddress, main->ui16FrameIdx, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    return FALSE;
  }

  /* Set the new frame index */
  ni_device_session_read_hwdesc (ctx, &overlay_ctx->api_dst_frame,
      NI_DEVICE_TYPE_SCALER);

  new_frame_surface =
      (niFrameSurface1_t *) overlay_ctx->api_dst_frame.data.frame.p_data[3];
  output->ui16FrameIdx = new_frame_surface->ui16FrameIdx;
  output->ui16session_ID = new_frame_surface->ui16session_ID;
  output->device_handle = new_frame_surface->device_handle;
  output->output_idx = new_frame_surface->output_idx;
  output->src_cpu = new_frame_surface->src_cpu;
  output->dma_buf_fd = 0;

  niPixFmt = convertGstVideoFormatToNIPix (filter->out_pixfmt);
  ni_set_bit_depth_and_encoding_type (&output->bit_depth,
      &output->encoding_type, niPixFmt);

  output->ui32nodeAddress = 0;
  output->ui16width = filter->in_width;
  output->ui16height = filter->in_height;

  return TRUE;
}

static void
pad_added_handler (GstElement * src, GstPad * new_pad, GstElement * convert)
{
  GstPad *sink_pad;
  GstPadLinkReturn ret;

  sink_pad = gst_element_get_static_pad (convert, "sink");
  ret = gst_pad_link (new_pad, sink_pad);
  if (ret != GST_PAD_LINK_OK) {
    GST_ERROR ("Could not link convert with uridecodebin, ret:%d\n", ret);
  } else {
    GST_DEBUG ("linked scale with uridecodebin\n");
  }

  gst_object_unref (sink_pad);
}

static gboolean
import_bg_frame (GstNiQuadraBg * filter, int src_w, int src_h,
    GstVideoFormat format, const char *img_file_name)
{
  GstElement *pipeline = NULL;

  GstElement *uridecodebin = NULL;
  GstElement *convert = NULL;
  GstElement *scale = NULL;
  GstElement *capsfilter = NULL;
  GstElement *appsink = NULL;

  GstVideoInfo info;
  GstCaps *caps = NULL;
  GstSample *sample = NULL;
  GstBuffer *buffer = NULL;
  gchar *bg_file_uri = NULL;
  GError *error = NULL;

  if ((!img_file_name) || (!g_file_test (img_file_name, G_FILE_TEST_EXISTS))) {
    GST_ERROR_OBJECT (filter, "bg image is invalid or not exsit.\n");
    return FALSE;
  }

  pipeline = gst_pipeline_new ("import-bg-pipeline");

  uridecodebin = gst_element_factory_make ("uridecodebin3", NULL);

  bg_file_uri = g_filename_to_uri (img_file_name, NULL, &error);
  if (!bg_file_uri) {
    GST_ERROR_OBJECT (filter, "Failed to convert filename to URI: %s\n",
        error ? error->message : "Unknown error");
    if (error) {
      g_error_free (error);
    }
    goto FAIL_EXIT;
  }

  g_object_set (uridecodebin, "uri", bg_file_uri, NULL);
  g_free (bg_file_uri);

  convert = gst_element_factory_make ("videoconvert", NULL);
  scale = gst_element_factory_make ("videoscale", NULL);

  capsfilter = gst_element_factory_make ("capsfilter", NULL);
  caps = gst_caps_new_simple ("video/x-raw",
      "width", G_TYPE_INT, src_w,
      "height", G_TYPE_INT, src_h,
      "format", G_TYPE_STRING, gst_video_format_to_string (format), NULL);
  if (caps) {
    g_object_set (capsfilter, "caps", caps, NULL);
  } else {
    GST_ERROR_OBJECT (filter, "gst_caps_new_simple failed\n");
    goto FAIL_EXIT;
  }

  appsink = gst_element_factory_make ("appsink", NULL);

  gst_bin_add_many (GST_BIN (pipeline), uridecodebin, convert, scale,
      capsfilter, appsink, NULL);

  if (!gst_element_link_many (convert, scale, capsfilter, appsink, NULL)) {
    GST_ERROR_OBJECT (filter, "Elements could not be linked when import bg\n");
    goto FAIL_EXIT;
  }

  g_signal_connect (uridecodebin, "pad-added", (GCallback) pad_added_handler,
      convert);

  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  /* get GstBuffer from appsink */
  sample = gst_app_sink_pull_sample (GST_APP_SINK (appsink));
  buffer = gst_sample_get_buffer (sample);
  filter->alpha_mask_frame.buffer = gst_buffer_copy (buffer);
  filter->alpha_mask_frame.buffer =
      gst_buffer_make_writable (filter->alpha_mask_frame.buffer);

  gst_video_info_from_caps (&info, caps);
  gst_video_frame_map (&filter->alpha_mask_frame.vframe, &info,
      filter->alpha_mask_frame.buffer, GST_MAP_READ | GST_MAP_WRITE);

  gst_sample_unref (sample);
  gst_caps_unref (caps);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  return TRUE;

FAIL_EXIT:
  if (uridecodebin) {
    gst_object_unref (uridecodebin);
  }
  if (bg_file_uri) {
    g_free (bg_file_uri);
  }
  if (convert) {
    gst_object_unref (convert);
  }
  if (scale) {
    gst_object_unref (scale);
  }
  if (capsfilter) {
    gst_object_unref (capsfilter);
  }
  if (appsink) {
    gst_object_unref (appsink);
  }
  if (pipeline) {
    gst_object_unref (pipeline);
  }
  if (caps) {
    gst_caps_unref (caps);
  }
  return FALSE;
}

static gboolean
create_bg_frame (GstNiQuadraBg * filter, int src_w, int src_h,
    GstVideoFormat format)
{
  GstVideoInfo info;
  GstCaps *caps = NULL;
  gint bg_frame_size;
  int size_Y, size_UV, size_A;
  uint8_t *Y_value, *U_value, *V_value, *A_value;

  //calculate frame size
  //todo: use accurate algorithm to calculate frame size
  bg_frame_size = filter->network.netw * filter->network.neth * 8;

  //malloc buffer in bg_frame_size bytes
  filter->alpha_mask_frame.buffer = gst_buffer_new_and_alloc (bg_frame_size);
  if (!filter->alpha_mask_frame.buffer) {
    return FALSE;
  }

  filter->alpha_mask_frame.buffer =
      gst_buffer_make_writable (filter->alpha_mask_frame.buffer);

  caps = gst_caps_new_simple ("video/x-raw",
      "width", G_TYPE_INT, src_w,
      "height", G_TYPE_INT, src_h,
      "format", G_TYPE_STRING, gst_video_format_to_string (format), NULL);
  if (!caps) {
    GST_ERROR_OBJECT (filter, "gst_caps_new_simple failed\n");
    goto FAIL_EXIT;
  }

  gst_video_info_from_caps (&info, caps);
  gst_video_frame_map (&filter->alpha_mask_frame.vframe, &info,
      filter->alpha_mask_frame.buffer, GST_MAP_READ | GST_MAP_WRITE);

  /*copy value to dst(bg_frame) frame */
  size_Y = size_A = src_w * src_h;
  size_UV = src_w * src_h / 4;

  Y_value = GST_VIDEO_FRAME_PLANE_DATA (&filter->alpha_mask_frame.vframe, 0);
  U_value = GST_VIDEO_FRAME_PLANE_DATA (&filter->alpha_mask_frame.vframe, 1);
  V_value = GST_VIDEO_FRAME_PLANE_DATA (&filter->alpha_mask_frame.vframe, 2);
  A_value = GST_VIDEO_FRAME_PLANE_DATA (&filter->alpha_mask_frame.vframe, 3);

  for (int i = 0; i < size_Y; i++) {
    Y_value[i] = 149;
  }

  for (int i = 0; i < size_UV; i++) {
    U_value[i] = 43;
  }

  for (int i = 0; i < size_UV; i++) {
    V_value[i] = 21;
  }

  for (int i = 0; i < size_A; i++) {
    A_value[i] = 21;
  }

  gst_caps_unref (caps);

  return TRUE;

FAIL_EXIT:
  if (caps) {
    gst_caps_unref (caps);
  }

  return FALSE;
}

static gboolean
init_mask_data (GstNiQuadraBg * filter, GstVideoFormat format)
{
  GstVideoInfo info;
  gint bg_frame_size;
  GstCaps *caps = NULL;

  //calculate frame size
  //todo: use accurate algorithm to calculate frame size
  bg_frame_size = filter->network.netw * filter->network.neth * 8;

  //malloc buffer in bg_frame_size bytes
  filter->mask_frame.buffer = gst_buffer_new_and_alloc (bg_frame_size);
  if (!filter->mask_frame.buffer) {
    return FALSE;
  }

  filter->mask_frame.buffer =
      gst_buffer_make_writable (filter->mask_frame.buffer);

  caps = gst_caps_new_simple ("video/x-raw",
      "width", G_TYPE_INT, filter->network.netw,
      "height", G_TYPE_INT, filter->network.neth,
      "format", G_TYPE_STRING, gst_video_format_to_string (format), NULL);

  gst_video_info_from_caps (&info, caps);
  gst_video_frame_map (&filter->mask_frame.vframe, &info,
      filter->mask_frame.buffer, GST_MAP_READ | GST_MAP_WRITE);

  gst_caps_unref (caps);
  return TRUE;
}

static gboolean
ni_get_mask (GstNiQuadraBg * filter, uint8_t * mask_data,
    ni_roi_network_t * network)
{
  ni_roi_network_layer_t *l = &network->layers[0];
  int mask_size = network->netw * network->neth;
  uint8_t Y_MIN = 0;
  uint8_t Y_MAX = 255;
  int i;

  if (!mask_data) {
    return FALSE;
  }
  // nhwc proprocessing
  for (i = 0; i < mask_size; i++) {
    if (l->output[2 * i] > l->output[2 * i + 1]) {
      mask_data[i] = Y_MAX;
    } else {
      mask_data[i] = Y_MIN;
    }
  }

  return TRUE;
}

static gboolean
get_alpha_mask_frame (GstNiQuadraBg * filter)
{
  GstElement *appsrc = NULL;
  GstElement *appsink = NULL;
  GstSample *sample = NULL;

  GstBuffer *inbuf = NULL;
  GstBuffer *outbuf = NULL;
  GstVideoFrame *src = NULL;
  GstVideoFrame *dst = NULL;

  //replace bg's alpha channel with mask data
  src = &filter->mask_frame.vframe;
  dst = &filter->alpha_mask_frame.vframe;
  gst_video_frame_copy_plane (dst, src, 3);

  //send bg data to sw scale pipeline
  appsrc = filter->sw_scale_ctx.appsrc;
  inbuf = gst_buffer_ref (filter->alpha_mask_frame.buffer);
  gst_app_src_push_buffer (GST_APP_SRC (appsrc), inbuf);

  //get scaled bg data from sw scale pipeline
  appsink = filter->sw_scale_ctx.appsink;
  sample = gst_app_sink_pull_sample (GST_APP_SINK (appsink));
  outbuf = gst_sample_get_buffer (sample);

  if (outbuf) {
    if (filter->alpha_enlarge_frame.buffer) {
      gst_buffer_unref (filter->alpha_enlarge_frame.buffer);
    }

    filter->alpha_enlarge_frame.buffer = gst_buffer_ref (outbuf);
  }

  if (sample) {
    gst_sample_unref (sample);
  }

  return TRUE;
}

static gboolean
ni_bg_process (GstNiQuadraBg * filter, ni_session_data_io_t * p_dst_pkt)
{
  ni_roi_network_t *network = &filter->network;
  uint8_t *mask_data = NULL;
  int i, retval = 0;

  for (i = 0; i < network->raw.output_num; i++) {
    retval = ni_network_layer_convert_output (network->layers[i].output,
        network->layers[i].output_number * sizeof (float),
        &p_dst_pkt->data.packet, &network->raw, i);
    if (retval != NI_RETCODE_SUCCESS) {
      return FALSE;
    }
  }

  mask_data = GST_VIDEO_FRAME_PLANE_DATA (&filter->mask_frame.vframe, 3);

  if (!ni_get_mask (filter, mask_data, network)) {
    return FALSE;
  }

  if (!get_alpha_mask_frame (filter)) {
    return FALSE;
  }

  return TRUE;
}

static gboolean
ni_bg_config_input (GstNiQuadraBg * filter, gint dev_idx)
{
  GstVideoFormat hw_scale_fmt = GST_VIDEO_FORMAT_BGRP;
  GstVideoFormat bg_in_fmt = GST_VIDEO_FORMAT_A420;
  GstVideoFormat bg_scale_fmt = GST_VIDEO_FORMAT_RGBA;

  if (filter->initialized)
    return TRUE;

  if (!init_hwframe_uploader (filter, bg_scale_fmt, dev_idx)) {
    return FALSE;
  }

  if (!init_ai_context (filter, dev_idx)) {
    return FALSE;
  }

  if (!ni_create_network (filter, &filter->network)) {
    return FALSE;
  }

  if (!init_hwframe_scale (filter, hw_scale_fmt, dev_idx)) {
    return FALSE;
  }

  if (!init_hwframe_overlay (filter, filter->in_pixfmt, dev_idx)) {
    return FALSE;
  }

  if (!init_mask_data (filter, bg_in_fmt)) {
    return FALSE;
  }

  if (filter->use_default_bg == 0) {
    if (!import_bg_frame (filter, filter->network.netw,
            filter->network.neth, bg_in_fmt, filter->bg_img)) {
      return FALSE;
    }
  } else {
    if (!create_bg_frame (filter, filter->network.netw,
            filter->network.neth, bg_in_fmt)) {
      return FALSE;
    }
  }

  if (!create_sw_scale_pipeline (filter, bg_in_fmt, bg_scale_fmt)) {
    return FALSE;
  }

  return TRUE;
}

static GstFlowReturn
gst_niquadrabg_chain (GstPad * pad, GstObject * parent, GstBuffer * inbuf)
{
  GstNiQuadraBg *filter = GST_NIQUADRABG (parent);

  AiContext *ai_ctx = NULL;
  ni_roi_network_t *network = NULL;
  niFrameSurface1_t *in_surface = NULL;
  niFrameSurface1_t *p_scale_surface = NULL;
  niFrameSurface1_t *p_ovly_surface = NULL;
  niFrameSurface1_t scale_surface;
  niFrameSurface1_t ovly_surface;
  niFrameSurface1_t realout;
  GstMemory *in_mem = NULL;
  GstMemory *out_mem = NULL;
  GstAllocator *alloc = NULL;
  GstBuffer *outbuf = NULL;
  gint dev_idx = -1;
  int retval = 0;

  in_mem = gst_buffer_peek_memory (inbuf, 0);
  if (!in_mem) {
    GST_ERROR_OBJECT (filter,
        "Impossible to convert between the formats supported by the filter\n");
    goto fail_out;
  }
  dev_idx = gst_deviceid_from_ni_hw_memory (in_mem);

  filter->framecount++;

  if (!filter->initialized) {
    if (!ni_bg_config_input (filter, dev_idx)) {
      goto fail_out;
    }

    filter->initialized = TRUE;
  }

  ai_ctx = filter->ai_ctx;
  network = &filter->network;

  retval = ni_ai_packet_buffer_alloc (&ai_ctx->api_dst_pkt.data.packet,
      &network->raw);
  if (retval != NI_RETCODE_SUCCESS) {
    goto fail_out;
  }

  in_surface = gst_surface_from_ni_hw_memory (in_mem);
  p_scale_surface = &scale_surface;

  if ((filter->skip == 0) ||
      ((filter->framecount - 1) % (filter->skip + 1) == 0)) {
    if (!ni_hwframe_scale (filter, in_surface, network->netw, network->neth,
            p_scale_surface)) {
      p_scale_surface = NULL;
      goto fail_out;
    }

    /* allocate output buffer */
    retval = ni_device_alloc_frame (&ai_ctx->api_ctx, 0, 0, 0, 0, 0, 0, 0, 0,
        p_scale_surface->ui32nodeAddress,
        p_scale_surface->ui16FrameIdx, NI_DEVICE_TYPE_AI);
    if (retval != NI_RETCODE_SUCCESS) {
      goto fail_out;
    }

    do {
      retval = ni_device_session_read (&ai_ctx->api_ctx, &ai_ctx->api_dst_pkt,
          NI_DEVICE_TYPE_AI);
      if (retval < 0) {
        GST_ERROR_OBJECT (filter, "read hwdesc retval %d\n", retval);
        goto fail_out;
      } else if (retval > 0) {
        if (!ni_bg_process (filter, &ai_ctx->api_dst_pkt)) {
          GST_ERROR_OBJECT (filter, "failed to read roi from packet\n");
          goto fail_out;
        }
      }

      ni_usleep (100);          //prevent spamming HW

    } while (retval == 0);

    ni_hwframe_buffer_recycle (p_scale_surface, p_scale_surface->device_handle);
    p_scale_surface = NULL;
  }

  p_ovly_surface = &ovly_surface;
  if (!ni_hwframe_upload (filter, &filter->alpha_enlarge_frame, p_ovly_surface)) {
    p_ovly_surface = NULL;
    goto fail_out;
  }

  if (!ni_hwframe_overlay (filter, in_surface, p_ovly_surface, &realout)) {
    goto fail_out;
  }

  ni_hwframe_buffer_recycle (p_ovly_surface, p_ovly_surface->device_handle);
  p_ovly_surface = NULL;

  outbuf = gst_buffer_new ();
  alloc = gst_allocator_find (GST_NIQUADRA_MEMORY_TYPE_NAME);
  out_mem = gst_niquadra_allocator_alloc (alloc, &filter->overlay_ctx->api_ctx,
      &realout, filter->overlay_ctx->api_ctx.hw_id, &filter->info);
  gst_buffer_append_memory (outbuf, out_mem);
  gst_object_unref (alloc);

  outbuf = gst_buffer_make_writable (outbuf);
  gst_buffer_copy_into (outbuf, inbuf,
      GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_META, 0, -1);

  gst_buffer_unref (inbuf);

  return gst_pad_push (filter->srcpad, outbuf);

fail_out:
  GST_ERROR_OBJECT (filter, "failed to do bg process\n");

  if (p_scale_surface) {
    ni_hwframe_buffer_recycle (p_scale_surface, p_scale_surface->device_handle);
  }

  if (p_ovly_surface) {
    ni_hwframe_buffer_recycle (p_ovly_surface, p_ovly_surface->device_handle);
  }

  if (inbuf) {
    gst_buffer_unref (inbuf);
  }

  return GST_FLOW_ERROR;
}

//bg filter only replace background, nothing else to do.
static gboolean
gst_niquadrabg_sink_setcaps (GstPad * pad, GstObject * parent, GstCaps * caps)
{
  GstNiQuadraBg *filter = GST_NIQUADRABG (parent);

  GstCaps *src_caps = NULL;
  GstVideoInfo *info = &filter->info;
  gboolean ret, gotit = FALSE;
  GstQuery *query = NULL;
  guint i;
  GType gtype;
  const GstStructure *params = NULL;

  if (!gst_video_info_from_caps (info, caps)) {
    return FALSE;
  }

  if (info->finfo->format == GST_VIDEO_FORMAT_BGRP) {
    GST_DEBUG_OBJECT (filter, "WARNING: bgrp not supported\n");
    return FALSE;
  }

  if (info->colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255) {
    GST_DEBUG_OBJECT (filter,
        "WARNING: Full color range input, limited color output\n");
  }
  info->colorimetry.range = GST_VIDEO_COLOR_RANGE_16_235;

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

  gst_query_unref (query);

  filter->out_width = filter->in_width = info->width;
  filter->out_height = filter->in_height = info->height;
  filter->out_pixfmt = filter->in_pixfmt = info->finfo->format;

  src_caps = gst_video_info_to_caps (info);
  gst_caps_set_features_simple (src_caps,
      gst_caps_features_from_string (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY));

  ret = gst_pad_set_caps (filter->srcpad, src_caps);
  gst_caps_unref (src_caps);

  return ret;
}

static gboolean
gst_niquadrabg_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstNiQuadraBg *filter = GST_NIQUADRABG (parent);
  gboolean ret = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;
      gst_event_parse_caps (event, &caps);

      ret = gst_niquadrabg_sink_setcaps (pad, parent, caps);

      gst_event_unref (event);
      break;
    }
    case GST_EVENT_EOS:
    {
      stop_sw_scale_pipeline (filter);
      ret = gst_pad_push_event (filter->srcpad, event);
      break;
    }
    default:
      ret = gst_pad_push_event (filter->srcpad, event);
      break;
  }

  return ret;
}

static void
gst_niquadrabg_dispose (GObject * obj)
{
  GstNiQuadraBg *filter = GST_NIQUADRABG (obj);

  /* ai */
  cleanup_ai_context (filter);
  ni_destroy_network (filter, &filter->network);

  cleanup_hwframe_scale (filter);
  cleanup_hwframe_uploader (filter);
  cleanup_hwframe_overlay (filter);

  cleanup_sw_scale_pipeline (filter);

  gst_video_frame_unmap (&filter->mask_frame.vframe);
  gst_buffer_unref (filter->mask_frame.buffer);

  gst_video_frame_unmap (&filter->alpha_mask_frame.vframe);
  gst_buffer_unref (filter->alpha_mask_frame.buffer);

  if (filter->alpha_enlarge_frame.buffer) {
    gst_buffer_unref (filter->alpha_enlarge_frame.buffer);
    filter->alpha_enlarge_frame.buffer = NULL;
  }

  if (filter->nb_file) {
    free (filter->nb_file);
  }

  if (filter->bg_img) {
    free (filter->bg_img);
  }

  G_OBJECT_CLASS (gst_niquadrabg_parent_class)->dispose (obj);
}

static void
gst_niquadrabg_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiQuadraBg *filter;

  g_return_if_fail (GST_IS_NIQUADRABG (object));
  filter = GST_NIQUADRABG (object);

  switch (prop_id) {
    case PROP_NB:
      g_free (filter->nb_file);
      filter->nb_file = g_value_dup_string (value);
      break;
    case PROP_BG_IMG:
      g_free (filter->bg_img);
      filter->bg_img = g_value_dup_string (value);
      break;
    case PROP_USE_DEFAULT_BG:
      filter->use_default_bg = g_value_get_uint (value);
      break;
    case PROP_SKIP:
      filter->skip = g_value_get_uint (value);
      break;
    case PROP_IS_P2P:
      filter->is_p2p = g_value_get_boolean (value);
      break;
    case PROP_KEEP_ALIVE_TIMEOUT:
      filter->keep_alive_timeout = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
gst_niquadrabg_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiQuadraBg *filter;

  g_return_if_fail (GST_IS_NIQUADRABG (object));
  filter = GST_NIQUADRABG (object);

  switch (prop_id) {
    case PROP_NB:
      g_value_take_string (value, g_strdup (filter->nb_file));
      break;
    case PROP_BG_IMG:
      g_value_take_string (value, g_strdup (filter->bg_img));
      break;
    case PROP_USE_DEFAULT_BG:
      g_value_set_uint (value, filter->use_default_bg);
      break;
    case PROP_SKIP:
      g_value_set_uint (value, filter->skip);
      break;
    case PROP_IS_P2P:
      g_value_set_boolean (value, filter->is_p2p);
      break;
    case PROP_KEEP_ALIVE_TIMEOUT:
      g_value_set_uint (value, filter->keep_alive_timeout);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
gst_niquadrabg_class_init (GstNiQuadraBgClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gobject_class->set_property = gst_niquadrabg_set_property;
  gobject_class->get_property = gst_niquadrabg_get_property;

  g_object_class_install_property (gobject_class, PROP_NB,
      g_param_spec_string ("nb", "Nb",
          "path to network binary file", NULL,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_BG_IMG,
      g_param_spec_string ("bg-img", "Bg-img",
          "path to replacement background file", NULL,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_USE_DEFAULT_BG,
      g_param_spec_uint ("use-default-bg", "Use-default-bg",
          "define use_default_bg", 0, G_MAXUINT, 0,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SKIP,
      g_param_spec_uint ("skip", "Skip",
          "frames to skip between inference", 0, G_MAXUINT, 0,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_IS_P2P,
      g_param_spec_boolean ("is-p2p", "Is-p2p",
          "enable p2p transfer", FALSE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

  g_object_class_install_property (gobject_class, PROP_KEEP_ALIVE_TIMEOUT,
      g_param_spec_uint ("keep-alive-timeout", "Keep-alive-timeout",
          "Specify a custom session keep alive timeout in seconds",
          NI_MIN_KEEP_ALIVE_TIMEOUT, NI_MAX_KEEP_ALIVE_TIMEOUT,
          NI_DEFAULT_KEEP_ALIVE_TIMEOUT,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (element_class, &src_factory);
  gst_element_class_add_static_pad_template (element_class, &sink_factory);

  gst_element_class_set_static_metadata (element_class,
      "NETINT Quadra BG filter", "Filter/Effect/Video/NIBG",
      "BG Netint Quadra", "Leo Liu<leo.liu@netint.cn>");

  gobject_class->dispose = gst_niquadrabg_dispose;
}

static void
gst_niquadrabg_init (GstNiQuadraBg * filter)
{
  filter->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_event_function (filter->sinkpad, gst_niquadrabg_sink_event);
  gst_pad_set_chain_function (filter->sinkpad, gst_niquadrabg_chain);
  gst_element_add_pad (GST_ELEMENT (filter), filter->sinkpad);

  filter->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  gst_element_add_pad (GST_ELEMENT (filter), filter->srcpad);

  filter->initialized = FALSE;

  memset (&filter->sw_scale_ctx, 0, sizeof (SwScaleContext));

  filter->hws_ctx = NULL;
  filter->ai_ctx = NULL;
  filter->hwupload_ctx = NULL;
  filter->overlay_ctx = NULL;

  memset (&filter->mask_frame, 0, sizeof (BgFrame));
  memset (&filter->alpha_mask_frame, 0, sizeof (BgFrame));
  memset (&filter->alpha_enlarge_frame, 0, sizeof (BgFrame));

  filter->framecount = 0;
  filter->downstream_card = -1;
  filter->extra_frames = 0;
}

static gboolean
niquadrabg_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadrabg_debug, "niquadrabg", 0, "niquadrabg");

  return gst_element_register (plugin, "niquadrabg", GST_RANK_NONE,
      GST_TYPE_NIQUADRABG);
}
