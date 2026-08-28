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
 *  \file   GstNiQuadraBgr.c
 *
 *  \brief  Implement of NetInt Quadra hardware background replace filter.
 ******************************************************************************/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/video.h>
#include <unistd.h>
#include <math.h>

#include "niquadra.h"
#include "ni_device_api.h"
#include "gstniquadramemory.h"
#include "gstniquadrautils.h"

GST_DEBUG_CATEGORY_STATIC (gst_niquadrabgr_debug);
#define GST_CAT_DEFAULT gst_niquadrabgr_debug

enum
{
  PROP_0,
  PROP_NB,
  PROP_SKIP,
  PROP_KEEP_ALIVE_TIMEOUT,
  PROP_LAST
};

typedef struct _ni_roi_network_layer
{
  guint32 width;
  guint32 height;
  guint32 channel;
  guint32 classes;
  guint32 component;
  guint32 output_number;
  float *output;
} ni_roi_network_layer_t;

typedef struct _ni_roi_network
{
  gint32 netw;
  gint32 neth;
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
  ni_session_data_io_t api_src_frame;
  ni_session_data_io_t api_dst_pkt;
} AiContext;

typedef struct HwFormatContext
{
  ni_session_context_t api_ctx;
  ni_session_data_io_t api_dst_frame;
} HwFormatContext;


typedef struct _GstNiQuadraBgr
{
  GstElement element;

  GstPad *sinkpad, *srcpad;

  gint in_width, in_height;
  GstVideoFormat outformat, informat;

  GstVideoInfo info;
  ni_session_context_t upload_ctx;
  ni_session_data_io_t src_session_io_data;
  /* Output sw frame */
  uint8_t *p_rgba_data;
  uint32_t rgba_frame_size;
  /* ai */
  gboolean initialized;
  gchar *nb_file;               /* path to network binary */

  AiContext *ai_ctx;
  ni_roi_network_t network;
  HwScaleContext *hws_ctx;

  /* format conversion using 2D */
  HwFormatContext *format_ctx;
  /* bg */
  uint8_t *mask_data;

  ni_session_data_io_t p_dl_frame;
  uint8_t *aui8SmallMask;
  gboolean skipInference;
  gint framecount;
  guint skip;
  gint skip_random_offset;

  gboolean reconfigure;

  guint keep_alive_timeout;     /* keep alive timeout setting */
  guint extra_frames;
  gint downstream_card;
} GstNiQuadraBgr;

typedef struct _GstNiQuadraBgrClass
{
  GstElementClass parent_class;
} GstNiQuadraBgrClass;

#define GST_TYPE_NIQUADRABGR \
  (gst_niquadrabgr_get_type())
#define GST_NIQUADRABGR(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRABGR,GstNiQuadraBgr))
#define GST_NIQUADRABGR_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRABGR,GstNiQuadraBgr))
#define GST_IS_NIQUADRABGR(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRABGR))
#define GST_IS_NIQUADRABGR_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRABGR))

static gboolean niquadrabgr_element_init (GstPlugin * plugin);

GType gst_niquadrabgr_get_type (void);

G_DEFINE_TYPE (GstNiQuadraBgr, gst_niquadrabgr, GST_TYPE_ELEMENT);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadrabgr, niquadrabgr_element_init);

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

static GstFlowReturn gst_niquadra_bgr_chain (GstPad * pad,
    GstObject * parent, GstBuffer * inbuf);

static void
gst_nifilter_caps_set_pixfmts (GstCaps * caps, const GstVideoFormat * fmt)
{
  GValue va = { 0, };
  GValue v = { 0, };
  GstVideoFormat format;

  g_value_init (&va, GST_TYPE_LIST);
  g_value_init (&v, G_TYPE_STRING);
  while (*fmt != -1) {
    format = *fmt;
    if (format != GST_VIDEO_FORMAT_UNKNOWN) {
      g_value_set_string (&v, gst_video_format_to_string (format));
      gst_value_list_append_value (&va, &v);
    }
    fmt++;
  }
  if (gst_value_list_get_size (&va) == 1) {
    /* The single value is still in v */
    gst_caps_set_value (caps, "format", &v);
  } else if (gst_value_list_get_size (&va) > 1) {
    gst_caps_set_value (caps, "format", &va);
  }
  g_value_unset (&v);
  g_value_unset (&va);
}

static gboolean
gst_niquadra_bgr_sink_setcaps (GstPad * pad, GstObject * parent, GstCaps * caps)
{
  GstNiQuadraBgr *filter = GST_NIQUADRABGR (parent);
  GstStructure *structure = gst_caps_get_structure (caps, 0);
  GstCaps *src_caps;
  GstVideoInfo output_info;
  gboolean ret, gotit = FALSE;
  GstQuery *query;
  guint i;
  GType gtype;
  const GstStructure *params = NULL;
  if (!gst_structure_get_int (structure, "width", &filter->in_width))
    return FALSE;
  if (!gst_structure_get_int (structure, "height", &filter->in_height))
    return FALSE;

  if (!gst_video_info_from_caps (&filter->info, caps))
    return FALSE;

  if (!gst_video_info_from_caps (&output_info, caps))
    return FALSE;

  if (filter->info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255) {
    GST_DEBUG_OBJECT (filter,
        "WARNING: Full color range input, limited color output\n");
  }
  filter->info.colorimetry.range = GST_VIDEO_COLOR_RANGE_16_235;

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

  filter->outformat = GST_VIDEO_FORMAT_RGBA;
  filter->informat = filter->info.finfo->format;

  output_info.colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_RGB;
  src_caps = gst_video_info_to_caps (&output_info);

  GstVideoFormat fmts[] = { filter->outformat, -1 };
  gst_nifilter_caps_set_pixfmts (src_caps, fmts);

  gst_caps_set_features_simple (src_caps,
      gst_caps_features_from_string (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY));

  ret = gst_pad_set_caps (filter->srcpad, src_caps);
  gst_caps_unref (src_caps);

  return ret;
}

static gboolean
gst_niquadra_bgr_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstNiQuadraBgr *filter = GST_NIQUADRABGR (parent);
  gboolean ret = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:{
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret = gst_niquadra_bgr_sink_setcaps (pad, parent, caps);
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
gst_niquadrabgr_init (GstNiQuadraBgr * filter)
{
  filter->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_event_function (filter->sinkpad, gst_niquadra_bgr_sink_event);
  gst_pad_set_chain_function (filter->sinkpad, gst_niquadra_bgr_chain);
  gst_element_add_pad (GST_ELEMENT (filter), filter->sinkpad);

  filter->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  gst_element_add_pad (GST_ELEMENT (filter), filter->srcpad);

  filter->reconfigure = FALSE;
  filter->keep_alive_timeout = NI_DEFAULT_KEEP_ALIVE_TIMEOUT;
  filter->aui8SmallMask = g_malloc0 (256 * 144);
  filter->downstream_card = -1;
  filter->extra_frames = 0;

  // download the rgba data from Input YUV.
  memset (&filter->p_dl_frame, 0, sizeof (ni_session_data_io_t));
  // upload the processed rgba data to down pad.
  memset (&filter->src_session_io_data, 0, sizeof (ni_session_data_io_t));
}

static void
cleanup_ai_context (GstNiQuadraBgr * filter)
{
  int retval = 0;
  AiContext *ai_ctx = filter->ai_ctx;

  if (ai_ctx) {
    ni_session_context_t *p_ctx = &ai_ctx->api_ctx;
    ni_frame_buffer_free (&ai_ctx->api_src_frame.data.frame);
    ni_packet_buffer_free (&ai_ctx->api_dst_pkt.data.packet);
    retval = ni_device_session_close (&ai_ctx->api_ctx, 1, NI_DEVICE_TYPE_AI);
    if (retval != NI_RETCODE_SUCCESS) {
      GST_ERROR_OBJECT (filter,
          "%s: failed to close ai session. retval %d\n", __func__, retval);
    }
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

static void
ni_destroy_network (GstNiQuadraBgr * filter, ni_roi_network_t * network)
{
  if (network) {
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

static void
cleanup_hwframe_scale (GstNiQuadraBgr * filter)
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

static void
cleanup_hwframe_format (GstNiQuadraBgr * filter)
{
  HwFormatContext *fmt_ctx = filter->format_ctx;

  if (fmt_ctx) {
    ni_frame_buffer_free (&fmt_ctx->api_dst_frame.data.frame);
    ni_device_session_close (&fmt_ctx->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
    ni_device_session_context_clear (&fmt_ctx->api_ctx);
    g_free (fmt_ctx);
    filter->format_ctx = NULL;
  }
}

static void
gst_niquadrabgr_dispose (GObject * obj)
{
  GstNiQuadraBgr *filter = GST_NIQUADRABGR (obj);

  if (filter->src_session_io_data.data.frame.p_buffer) {
    ni_frame_buffer_free (&filter->src_session_io_data.data.frame);
  }
  if (filter->upload_ctx.session_id != NI_INVALID_SESSION_ID) {
    GST_DEBUG_OBJECT (filter, "libxcoder uploader free context\n");
    ni_device_session_close (&filter->upload_ctx, 1, NI_DEVICE_TYPE_UPLOAD);
  }
  ni_device_session_context_clear (&filter->upload_ctx);

  /* ai */
  cleanup_ai_context (filter);
  ni_destroy_network (filter, &filter->network);

  /* bg */
  ni_frame_buffer_free (&filter->p_dl_frame.data.frame);

  cleanup_hwframe_scale (filter);
  cleanup_hwframe_format (filter);

  if (filter->nb_file) {
    g_free (filter->nb_file);
    filter->nb_file = NULL;
  }

  g_free (filter->aui8SmallMask);
  free (filter->mask_data);

  G_OBJECT_CLASS (gst_niquadrabgr_parent_class)->dispose (obj);
}

static gboolean
init_ai_context (GstNiQuadraBgr * filter, gint dev_idx)
{
  int retval = 0;
  AiContext *ai_ctx;
  ni_roi_network_t *network = &filter->network;
  int hwframe = 1;

  if ((filter->nb_file == NULL) || (access (filter->nb_file, R_OK) != 0)) {
    GST_ERROR_OBJECT (filter, "invalid network binary path\n");
    return FALSE;
  }

  ai_ctx = g_malloc0 (sizeof (AiContext));
  if (!ai_ctx) {
    GST_ERROR_OBJECT (filter, "failed to allocate ai context\n");
    return FALSE;
  }
  filter->ai_ctx = ai_ctx;

  memset (ai_ctx, 0, sizeof (AiContext));

  ni_device_session_context_init (&ai_ctx->api_ctx);
  if (hwframe) {
    ai_ctx->api_ctx.session_id = NI_INVALID_SESSION_ID;
    ai_ctx->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
    ai_ctx->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;
    ai_ctx->api_ctx.hw_action = NI_CODEC_HW_ENABLE;
    ai_ctx->api_ctx.hw_id = dev_idx;
  }

  ai_ctx->api_ctx.device_type = NI_DEVICE_TYPE_AI;
  ai_ctx->api_ctx.keep_alive_timeout = filter->keep_alive_timeout;

  retval = ni_device_session_open (&ai_ctx->api_ctx, NI_DEVICE_TYPE_AI);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "failed to open ai session. retval %d\n", retval);
    goto failed_out;
  }

  retval = ni_ai_config_network_binary (&ai_ctx->api_ctx,
      &network->raw, filter->nb_file);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter,
        "failed to configure ai session. retval %d\n", retval);
    goto failed_out;
  }

  if (!hwframe) {
    retval = ni_ai_frame_buffer_alloc (&ai_ctx->api_src_frame.data.frame,
        &network->raw);
    if (retval != NI_RETCODE_SUCCESS) {
      GST_ERROR_OBJECT (filter, "failed to allocate ni frame\n");
      goto failed_out;
    }
  }

  retval = ni_ai_packet_buffer_alloc (&ai_ctx->api_dst_pkt.data.packet,
      &network->raw);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "failed to allocate ni packet\n");
    goto failed_out;
  }

  return TRUE;

failed_out:
  cleanup_ai_context (filter);
  return FALSE;
}

static gboolean
ni_create_network (GstNiQuadraBgr * filter, ni_roi_network_t * network)
{
  int i;
  ni_network_data_t *ni_network = &network->raw;

  GST_DEBUG_OBJECT (filter, "network input number %d, output number %d\n",
      ni_network->input_num, ni_network->output_num);

  if (ni_network->input_num == 0 || ni_network->output_num == 0) {
    GST_ERROR_OBJECT (filter, "invalid network layer\n");
    return FALSE;
  }

  /* only support one input for now */
  if (ni_network->input_num != 1) {
    GST_ERROR_OBJECT (filter, "network input layer number %d not supported\n",
        ni_network->input_num);
    return FALSE;
  }
  // create network and its layers.
  network->layers = g_new0 (ni_roi_network_layer_t, ni_network->output_num);
  if (!network->layers) {
    GST_ERROR_OBJECT (filter, "cannot allocate network layer memory\n");
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
        g_new0 (float, network->layers[i].output_number);
    if (!network->layers[i].output) {
      GST_ERROR_OBJECT (filter,
          "failed to allocate network layer %d output buffer\n", i);
      goto failed_out;
    }

    GST_DEBUG_OBJECT (filter,
        "network layer %d: w %d, h %d, ch %d, co %d, cl %d\n", i,
        network->layers[i].width, network->layers[i].height,
        network->layers[i].channel, network->layers[i].component,
        network->layers[i].classes);
  }

  network->netw = ni_network->linfo.in_param[0].sizes[0];
  network->neth = ni_network->linfo.in_param[0].sizes[1];

  return TRUE;

failed_out:
  ni_destroy_network (filter, network);
  return FALSE;
}

static gboolean
init_hwframe_scale (GstNiQuadraBgr * filter,
    GstVideoFormat format, gint dev_idx)
{
  int retval = 0;
  HwScaleContext *hws_ctx;
  int pool_size = DEFAULT_NI_FILTER_POOL_SIZE;

  hws_ctx = g_malloc0 (sizeof (HwScaleContext));
  if (!hws_ctx) {
    GST_ERROR_OBJECT (filter, "could not allocate hwframe ctx\n");
    return FALSE;
  }
  filter->hws_ctx = hws_ctx;

  memset (&hws_ctx->api_dst_frame, 0, sizeof (ni_session_data_io_t));

  ni_device_session_context_init (&hws_ctx->api_ctx);

  hws_ctx->api_ctx.session_id = NI_INVALID_SESSION_ID;
  hws_ctx->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
  hws_ctx->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;
  hws_ctx->api_ctx.device_type = NI_DEVICE_TYPE_SCALER;
  hws_ctx->api_ctx.scaler_operation = NI_SCALER_OPCODE_SCALE;
  hws_ctx->api_ctx.hw_id = dev_idx;
  hws_ctx->api_ctx.keep_alive_timeout = filter->keep_alive_timeout;

  retval = ni_device_session_open (&hws_ctx->api_ctx, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "could not open scaler session\n");
    ni_device_session_close (&hws_ctx->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
    ni_device_session_context_clear (&hws_ctx->api_ctx);
    goto failed_out;
  }

  if (hws_ctx->api_ctx.hw_id != filter->downstream_card) {
    pool_size += filter->extra_frames;
    GST_INFO_OBJECT (filter,
        "Increase frame pool by %d\n", filter->extra_frames);
  }

  /* Create scale frame pool on device */
  retval = ni_build_frame_pool (&hws_ctx->api_ctx, filter->network.netw,
      filter->network.neth, format, pool_size);
  if (retval < 0) {
    GST_ERROR_OBJECT (filter, "could not build frame pool, ret=%d\n", retval);
    ni_device_session_close (&hws_ctx->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
    ni_device_session_context_clear (&hws_ctx->api_ctx);
    goto failed_out;
  }

  return TRUE;

failed_out:
  g_free (hws_ctx);
  return FALSE;
}

static gboolean
init_hwframe_format (GstNiQuadraBgr * filter, gint dev_idx)
{
  int retval = 0;
  HwFormatContext *format_ctx;

  format_ctx = g_malloc0 (sizeof (HwScaleContext));
  if (!format_ctx) {
    GST_ERROR_OBJECT (filter,
        "Could not allocate hwframe ctx for format conversion\n");
    return FALSE;
  }
  filter->format_ctx = format_ctx;
  memset (&format_ctx->api_dst_frame, 0, sizeof (ni_session_data_io_t));

  ni_device_session_context_init (&format_ctx->api_ctx);

  format_ctx->api_ctx.session_id = NI_INVALID_SESSION_ID;
  format_ctx->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
  format_ctx->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;
  format_ctx->api_ctx.device_type = NI_DEVICE_TYPE_SCALER;
  format_ctx->api_ctx.scaler_operation = NI_SCALER_OPCODE_SCALE;
  format_ctx->api_ctx.hw_id = dev_idx;
  format_ctx->api_ctx.keep_alive_timeout = filter->keep_alive_timeout;

  retval = ni_device_session_open (&format_ctx->api_ctx, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter,
        "Could not open scaler session for format conversion\n");
    ni_device_session_close (&format_ctx->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
    ni_device_session_context_clear (&format_ctx->api_ctx);
    goto failed_out;
  }

  /* Create frame pool on device for format conversion through 2D engine.
   * It must be in P2P so that DSP can have access to it. */
  retval = ni_build_frame_pool (&format_ctx->api_ctx, filter->info.width,
      filter->info.height, GST_VIDEO_FORMAT_RGBA, DEFAULT_NI_FILTER_POOL_SIZE);
  if (retval < 0) {
    GST_ERROR_OBJECT (filter, "could not build frame pool\n");
    ni_device_session_close (&format_ctx->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
    ni_device_session_context_clear (&format_ctx->api_ctx);
    goto failed_out;
  }

  return TRUE;

failed_out:
  g_free (format_ctx);
  return FALSE;
}

static gboolean
init_hwframe_uploader (GstNiQuadraBgr * filter, gint dev_idx)
{
  int retval = 0;

  ni_device_session_context_init (&filter->upload_ctx);

  filter->upload_ctx.session_id = NI_INVALID_SESSION_ID;
  filter->upload_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
  filter->upload_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;
  filter->upload_ctx.hw_id = dev_idx;

  filter->upload_ctx.active_video_width = filter->info.width;
  filter->upload_ctx.active_video_height = filter->info.height;
  filter->upload_ctx.bit_depth_factor = 4;
  filter->upload_ctx.src_bit_depth = 32;
  filter->upload_ctx.src_endian = NI_FRAME_LITTLE_ENDIAN;

  ni_uploader_set_frame_format (&filter->upload_ctx, filter->info.width,
      filter->info.height, NI_PIX_FMT_RGBA, 0);
  retval = ni_device_session_open (&filter->upload_ctx, NI_DEVICE_TYPE_UPLOAD);
  if (retval < 0) {
    GST_ERROR_OBJECT (filter, "Open uploader session error\n");
    if (filter->upload_ctx.session_id != NI_INVALID_SESSION_ID) {
      ni_device_session_close (&filter->upload_ctx, 1, NI_DEVICE_TYPE_UPLOAD);
    }
    ni_device_session_context_clear (&filter->upload_ctx);
    return FALSE;
  } else {
    GST_DEBUG_OBJECT (filter, "XCoder %s.%d (inst: %d) opened successfully\n",
        filter->upload_ctx.dev_xcoder_name, filter->upload_ctx.hw_id,
        filter->upload_ctx.session_id);
  }

  retval = ni_device_session_init_framepool (&filter->upload_ctx, 3, 0);
  if (retval < 0) {
    GST_ERROR_OBJECT (filter, "Init frame pool error\n");
    if (filter->upload_ctx.session_id != NI_INVALID_SESSION_ID) {
      ni_device_session_close (&filter->upload_ctx, 1, NI_DEVICE_TYPE_UPLOAD);
    }
    ni_device_session_context_clear (&filter->upload_ctx);
    return FALSE;
  }

  return TRUE;
}

static gboolean
ni_bgr_config_output (GstNiQuadraBgr * filter)
{
  int retval = 0;
  int linesize[4];
  linesize[0] = NI_ALIGN (filter->info.width, 16) * 4;
  linesize[1] = linesize[2] = linesize[3] = 0;

  retval = ni_frame_buffer_alloc_pixfmt (&filter->p_dl_frame.data.frame,
      NI_PIX_FMT_RGBA, filter->info.width, filter->info.height, linesize, 2, 0);
  if (retval != NI_RETCODE_SUCCESS) {
    return FALSE;
  }

  filter->rgba_frame_size = filter->p_dl_frame.data.frame.data_len[0] +
      filter->p_dl_frame.data.frame.data_len[1] +
      filter->p_dl_frame.data.frame.data_len[2] +
      filter->p_dl_frame.data.frame.data_len[3];
  filter->p_rgba_data = filter->p_dl_frame.data.frame.p_data[0];

  return TRUE;
}

static gboolean
ni_bgr_config_input (GstNiQuadraBgr * filter, gint dev_idx)
{
  if (filter->initialized)
    return TRUE;

  if (filter->info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255) {
    GST_ERROR_OBJECT (filter,
        "WARNING: Full color range input, limited color output\n");
  }

  if (!init_hwframe_uploader (filter, dev_idx)) {
    GST_ERROR_OBJECT (filter, "failed to initialize uploader session\n");
    return FALSE;
  }

  if (!init_ai_context (filter, dev_idx)) {
    GST_ERROR_OBJECT (filter, "failed to initialize ai context\n");
    return FALSE;
  }

  if (!ni_create_network (filter, &filter->network)) {
    GST_ERROR_OBJECT (filter, "failed to ni_create_network\n");
    goto fail_out;
  }

  if (!init_hwframe_scale (filter, GST_VIDEO_FORMAT_BGRP, dev_idx)) {
    GST_ERROR_OBJECT (filter, "could not initialized hwframe scale context\n");
    goto fail_out;
  }

  /* Allocate a frame pool of type AV_PIX_FMT_RGBA for converting
   * the input frame to RGBA using 2D engine */
  if (!init_hwframe_format (filter, dev_idx)) {
    GST_ERROR_OBJECT (filter,
        "Could not initialize hwframe context for format conversion\n");
    goto fail_out;
  }

  filter->mask_data =
      g_new (uint8_t, filter->network.netw * filter->network.neth);
  if (!filter->mask_data) {
    GST_ERROR_OBJECT (filter, "cannot allocate sctx->mask_datamemory\n");
    goto fail_out;
  }

  filter->initialized = 1;
  return TRUE;

fail_out:
  cleanup_ai_context (filter);
  ni_destroy_network (filter, &filter->network);
  return FALSE;
}

static gboolean
ni_hwframe_scale (GstNiQuadraBgr * filter, niFrameSurface1_t * frame_surface,
    int w, int h, niFrameSurface1_t ** downscale_bgr_surface)
{
  HwScaleContext *scale_ctx = filter->hws_ctx;
  int scaler_format;
  int retval = 0;
  niFrameSurface1_t *new_frame_surface = NULL;

  GST_DEBUG_OBJECT (filter, "in frame surface frameIdx %d,w=%d,h=%d\n",
      frame_surface->ui16FrameIdx, w, h);

  scaler_format =
      convertGstVideoFormatToGC620Format (filter->info.finfo->format);

  retval = ni_frame_buffer_alloc_hwenc (&scale_ctx->api_dst_frame.data.frame,
      w, h, 0);
  if (retval != NI_RETCODE_SUCCESS)
    return FALSE;

  /*
   * Allocate device input frame. This call won't actually allocate a frame,
   * but sends the incoming hardware frame index to the scaler manager
   */
  /* *INDENT-OFF* */
  retval = ni_device_alloc_frame (&scale_ctx->api_ctx,
      NI_ALIGN (filter->info.width, 2),
      NI_ALIGN (filter->info.height, 2),
      scaler_format,
      0,
      0,
      0,
      0,
      0,
      frame_surface->ui32nodeAddress,
      frame_surface->ui16FrameIdx,
      NI_DEVICE_TYPE_SCALER);
  /* *INDENT-ON* */

  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "Can't allocate device input frame %d\n", retval);
    return FALSE;
  }

  /* Allocate hardware device destination frame. This acquires a frame from
   * the pool */
  /* *INDENT-OFF* */
  retval = ni_device_alloc_frame (&scale_ctx->api_ctx,
      NI_ALIGN (w, 2),
      NI_ALIGN (h, 2),
      convertGstVideoFormatToGC620Format (GST_VIDEO_FORMAT_BGRP),
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
        "Can't allocate device output frame %d\n", retval);
    return FALSE;
  }

  /* Set the new frame index */
  retval = ni_device_session_read_hwdesc (&scale_ctx->api_ctx,
      &scale_ctx->api_dst_frame, NI_DEVICE_TYPE_SCALER);
  if (retval < 0) {
    GST_ERROR_OBJECT (filter, "Failed to read hwdesc, retval=%d\n", retval);
    return FALSE;
  }

  new_frame_surface =
      (niFrameSurface1_t *) scale_ctx->api_dst_frame.data.frame.p_data[3];

  *downscale_bgr_surface = new_frame_surface;

  return TRUE;
}

static gboolean
ni_hwframe_convert_format (GstNiQuadraBgr * filter,
    niFrameSurface1_t * frame_surface, niFrameSurface1_t ** rgba_frame_surface)
{
  HwFormatContext *format_ctx = filter->format_ctx;
  int retval = 0;
  int input_format;
  int output_format =
      convertGstVideoFormatToGC620Format (GST_VIDEO_FORMAT_RGBA);
  niFrameSurface1_t *new_frame_surface = NULL;

  GST_DEBUG_OBJECT (filter,
      "Input surface frame index for format conversion %d\n",
      frame_surface->ui16FrameIdx);

  input_format =
      convertGstVideoFormatToGC620Format (filter->info.finfo->format);

  retval = ni_frame_buffer_alloc_hwenc (&format_ctx->api_dst_frame.data.frame,
      filter->info.width, filter->info.height, 0);
  if (retval != NI_RETCODE_SUCCESS)
    return FALSE;

  /*
   * Allocate device input frame for format conversion.
   * This call won't actually allocate a frame, but sends the incoming
   * hardware frame index to the scaler manager.
   */
  /* *INDENT-OFF* */
  retval = ni_device_alloc_frame (&format_ctx->api_ctx,
      NI_ALIGN (filter->info.width, 2),
      NI_ALIGN (filter->info.height, 2),
      input_format,
      0,
      0,
      0,
      0,
      0,
      0,
      frame_surface->ui16FrameIdx,
      NI_DEVICE_TYPE_SCALER);
  /* *INDENT-ON* */

  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter,
        "Can't allocate device input frame for format conversion %d\n", retval);
    return FALSE;
  }

  /* Allocate hardware device destination frame. This acquires a frame from
   * the pool that was allocated in the initialization stage.
   * We must avoid the 2D operation for the BG mode.
   */
  retval = ni_device_alloc_frame (&format_ctx->api_ctx,
      NI_ALIGN (filter->info.width, 2), NI_ALIGN (filter->info.height, 2),
      output_format, NI_SCALER_FLAG_IO, 0, 0, 0, 0, 0, -1,
      NI_DEVICE_TYPE_SCALER);

  if (retval != NI_RETCODE_SUCCESS) {
    GST_DEBUG_OBJECT (filter,
        "Can't allocate device output frame for format conversion %d\n",
        retval);
    return FALSE;
  }

  /* Set the new frame index */
  retval = ni_device_session_read_hwdesc (&format_ctx->api_ctx,
      &format_ctx->api_dst_frame, NI_DEVICE_TYPE_SCALER);
  if (retval < 0) {
    GST_ERROR_OBJECT (filter,
        "Can't get the scaler output frame index %d\n", retval);
    return FALSE;
  }

  new_frame_surface =
      (niFrameSurface1_t *) format_ctx->api_dst_frame.data.frame.p_data[3];
  new_frame_surface->bit_depth = 1;
  new_frame_surface->encoding_type = 1;
  new_frame_surface->ui16width = filter->info.width;
  new_frame_surface->ui16height = filter->info.height;
  *rgba_frame_surface = new_frame_surface;

  return TRUE;
}

static gboolean
ni_hwframe_converted_download (GstNiQuadraBgr * filter,
    niFrameSurface1_t * hwdl_frame_surface)
{
  int retval = 0;
  HwFormatContext *format_ctx = filter->format_ctx;

  GST_DEBUG_OBJECT (filter,
      "Input surface frame index for HWDL conversion %d\n",
      hwdl_frame_surface->ui16FrameIdx);
  format_ctx->api_ctx.is_auto_dl = false;

  retval = ni_device_session_hwdl (&format_ctx->api_ctx,
      &filter->p_dl_frame, hwdl_frame_surface);
  if (retval <= 0) {
    GST_ERROR_OBJECT (filter, "HWDL ERROR %d\n", retval);
    return FALSE;
  }

  return TRUE;
}

static gboolean
ni_get_mask2 (GstNiQuadraBgr * filter, ni_session_data_io_t * p_raw_mask_data)
{
  uint8_t Y_MIN = 255;
  uint8_t Y_MAX = 0;
  uint8_t *p_mask_raw_start = p_raw_mask_data->data.packet.p_data;

  int loop_length = 256 * 144;
  uint8_t *p_mask_raw = p_mask_raw_start;
  uint8_t ui8AiTensorL;
  uint8_t ui8AiTensorR;
  uint8_t *aui8SmallMask = filter->aui8SmallMask;

  for (int i = 0; i < loop_length; i++) {
    ui8AiTensorL = *(p_mask_raw + 1);
    ui8AiTensorR = *p_mask_raw;

    if (ui8AiTensorL < ui8AiTensorR) {
      aui8SmallMask[i] = Y_MAX;
    } else {
      aui8SmallMask[i] = Y_MIN;
    }
    p_mask_raw += 2;
  }

  for (int i = 0; i < 30; i++) {
    GST_DEBUG_OBJECT (filter, "mask[%d] 0x%x\n", i, aui8SmallMask[i]);
  }

  return TRUE;
}

static gboolean
ni_upscale_alpha2RGBMTA (GstNiQuadraBgr * filter)
{
  uint8_t *aui8SmallMask = filter->aui8SmallMask;
  uint8_t *p_rgbmta = filter->p_dl_frame.data.frame.p_data[0];
  uint32_t destWidth = filter->info.width;
  uint32_t destHeight = filter->info.height;

  uint16_t src_x, src_y;
  uint32_t dst_w_aligned = NI_ALIGN (destWidth, 16);
  for (uint32_t dst_y = 0; dst_y < destHeight; dst_y++) {
    src_y = (uint16_t) floorf ((((float) dst_y) / ((float) destHeight)) *
        ((float) 144));
    uint32_t *p32_dst =
        (uint32_t *) (p_rgbmta + dst_y * dst_w_aligned * sizeof (uint32_t));
    for (uint32_t dst_x = 0; dst_x < destWidth; dst_x++) {
      src_x = (uint16_t) floorf ((((float) dst_x) / ((float) destWidth)) *
          ((float) 256));
      //need to read write
      if (aui8SmallMask[src_y * 256 + src_x]) {
        //do nothing
      } else {
        *p32_dst = 0;
      }
      p32_dst += 1;
    }
  }

  return TRUE;
}

static gboolean
ni_bgr_process (GstNiQuadraBgr * filter, ni_session_data_io_t * p_dst_pkt)
{
  if (!filter->skipInference) {
    ni_get_mask2 (filter, p_dst_pkt);
  }

  if (!ni_upscale_alpha2RGBMTA (filter)) {
    GST_ERROR_OBJECT (filter, "failed to get mask data.\n");
    return FALSE;
  }

  return TRUE;
}

static GstFlowReturn
gst_niquadra_bgr_chain (GstPad * pad, GstObject * parent, GstBuffer * inbuf)
{
  GstNiQuadraBgr *filter = GST_NIQUADRABGR (parent);
  GstMemory *in_mem = NULL;
  niFrameSurface1_t *in_surface = NULL;
  gint dev_idx = -1;
  GstAllocator *alloc = NULL;
  GstMemory *out_mem = NULL;
  GstBuffer *outbuf = NULL;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  int dst_stride[4];
  bool isSemiPlanar;
  ni_session_data_io_t dst_session_data;
  ni_session_data_io_t *p_dst_session_data = &dst_session_data;
  ni_session_data_io_t *p_src_session_data;

  p_src_session_data = &filter->src_session_io_data;
  dst_stride[0] = NI_ALIGN (filter->info.width, 16) * 4;
  dst_stride[1] = 0;
  dst_stride[2] = 0;
  dst_stride[3] = 0;
  isSemiPlanar = false;

  if (!p_src_session_data->data.frame.p_buffer) {
    p_src_session_data->data.frame.extra_data_len =
        NI_APP_ENC_FRAME_META_DATA_SIZE;
    ni_frame_buffer_alloc_pixfmt (&p_src_session_data->data.frame,
        NI_PIX_FMT_RGBA, filter->info.width, filter->info.height, dst_stride, 1,
        (int) p_src_session_data->data.frame.extra_data_len);
  }

  outbuf = gst_buffer_new ();

  /* ai roi */
  ni_roi_network_t *network = NULL;
  int retval = 0;
  AiContext *ai_ctx = NULL;
  niFrameSurface1_t *downscale_bgr_surface = NULL;
  niFrameSurface1_t *rgba_frame_surface = NULL;

  filter->framecount++;

  in_mem = gst_buffer_peek_memory (inbuf, 0);
  dev_idx = gst_deviceid_from_ni_hw_memory (in_mem);
  in_surface = gst_surface_from_ni_hw_memory (in_mem);
  if (!in_surface) {
    GST_ERROR_OBJECT (filter,
        "Impossible to convert between the formats supported by the filter\n");
    if (inbuf) {
      gst_buffer_unref (inbuf);
    }
    return GST_FLOW_ERROR;
  }
#define CLEAN_SCALE 1
#define CLEAN_FORMAT 2

  gint clean_scale_format = 0;

  if (!filter->initialized) {
    filter->skip_random_offset = 1;

    if (!ni_bgr_config_output (filter)) {
      GST_ERROR_OBJECT (filter, "failed to config output\n");
      flow_ret = GST_FLOW_ERROR;
      goto fail;
    }

    if (!ni_bgr_config_input (filter, dev_idx)) {
      GST_ERROR_OBJECT (filter, "failed to config input\n");
      flow_ret = GST_FLOW_ERROR;
      goto fail;
    }
  }

  ai_ctx = filter->ai_ctx;
  network = &filter->network;
  retval =
      ni_ai_packet_buffer_alloc (&ai_ctx->api_dst_pkt.data.packet,
      &network->raw);

  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "failed to allocate packet\n");
    return GST_FLOW_ERROR;
  }

  if ((filter->skip == 0)
      || ((filter->framecount - 1) % (filter->skip + 1) == 0)) {
    if (filter->skip_random_offset) {
      filter->framecount += ai_ctx->api_ctx.session_id % (filter->skip + 1);
      GST_DEBUG_OBJECT (filter, "BGR SPACE sid %u fc %u\n",
          ai_ctx->api_ctx.session_id, filter->framecount);
      filter->skip_random_offset = 0;
    }
    filter->skipInference = false;
  } else {
    filter->skipInference = true;
    GST_DEBUG_OBJECT (filter,
        "Inference skip, framecount %d\n", filter->framecount);
  }

  {
    if (!filter->skipInference) {
      if (!ni_hwframe_scale (filter, in_surface, network->netw, network->neth,
              &downscale_bgr_surface)) {
        GST_ERROR_OBJECT (filter, "Error run hwframe scale\n");
        flow_ret = GST_FLOW_ERROR;
        goto fail;
      }
    }

    if (!filter->skipInference) {
      /* allocate output buffer */
      retval = ni_device_alloc_frame (&ai_ctx->api_ctx, 0, 0, 0, 0, 0, 0, 0, 0,
          downscale_bgr_surface->ui32nodeAddress,
          downscale_bgr_surface->ui16FrameIdx, NI_DEVICE_TYPE_AI);
      if (retval != NI_RETCODE_SUCCESS) {
        GST_ERROR_OBJECT (filter, "failed to alloc hw input frame\n");
        flow_ret = GST_FLOW_ERROR;
        clean_scale_format |= CLEAN_SCALE;
        goto fail;
      }
    }

    if (!ni_hwframe_convert_format (filter, in_surface, &rgba_frame_surface)) {
      GST_ERROR_OBJECT (filter,
          "Error in runnig hwframe for format conversion\n");
      flow_ret = GST_FLOW_ERROR;
      goto fail;
    }

    if (!ni_hwframe_converted_download (filter, rgba_frame_surface)) {
      GST_ERROR_OBJECT (filter,
          "Error in downloading hwframe for format conversion\n");
      clean_scale_format |= CLEAN_FORMAT;
      flow_ret = GST_FLOW_ERROR;
      goto fail;
    }

    ni_hwframe_buffer_recycle (rgba_frame_surface,
        rgba_frame_surface->device_handle);
    if (!filter->skipInference) {
      do {
        retval = ni_device_session_read (&ai_ctx->api_ctx,
            &ai_ctx->api_dst_pkt, NI_DEVICE_TYPE_AI);
        if (retval < 0) {
          GST_ERROR_OBJECT (filter, "read hwdesc retval %d\n", retval);
          flow_ret = GST_FLOW_ERROR;
          clean_scale_format |= CLEAN_SCALE;
          goto fail;
        } else if (retval > 0) {
          if (!ni_bgr_process (filter, &ai_ctx->api_dst_pkt)) {
            GST_ERROR_OBJECT (filter, "failed to process tensor\n");
            clean_scale_format |= CLEAN_SCALE;
            flow_ret = GST_FLOW_ERROR;
            goto fail;
          }
        }
        ni_usleep (100);
      } while (retval == 0);

      ni_hwframe_buffer_recycle (downscale_bgr_surface,
          downscale_bgr_surface->device_handle);
    } else {
      if (!ni_bgr_process (filter, &ai_ctx->api_dst_pkt)) {
        GST_ERROR_OBJECT (filter, "failed to process tensor\n");
        flow_ret = GST_FLOW_ERROR;
        goto fail;
      }
    }
  }

  memset (p_dst_session_data, 0, sizeof (dst_session_data));

  retval =
      ni_frame_buffer_alloc (&p_dst_session_data->data.frame,
      filter->info.width, filter->info.height, 0, 1,
      filter->upload_ctx.bit_depth_factor, 1, !isSemiPlanar);

  if (retval < 0) {
    GST_ERROR_OBJECT (filter, "alloc output error\n");
    return GST_FLOW_ERROR;
  }

  memcpy (p_src_session_data->data.frame.p_data[0], filter->p_rgba_data,
      filter->rgba_frame_size);

  niFrameSurface1_t *dst_surf;
  ni_frame_t *xfme;
  xfme = &p_dst_session_data->data.frame;
  dst_surf =
      (niFrameSurface1_t *) (xfme->p_buffer + xfme->data_len[0] +
      xfme->data_len[1] + xfme->data_len[2]);

  retval = ni_device_session_hwup (&filter->upload_ctx,
      p_src_session_data, dst_surf);
  if (retval < 0) {
    GST_ERROR_OBJECT (filter, "Failed to upload frame\n");
    return GST_FLOW_ERROR;
  }

  dst_surf->ui16width = filter->info.width;
  dst_surf->ui16height = filter->info.height;
  dst_surf->ui32nodeAddress = 0;
  dst_surf->encoding_type =
      isSemiPlanar ? NI_PIXEL_PLANAR_FORMAT_SEMIPLANAR :
      NI_PIXEL_PLANAR_FORMAT_PLANAR;

  alloc = gst_allocator_find (GST_NIQUADRA_MEMORY_TYPE_NAME);
  out_mem = gst_niquadra_allocator_alloc (alloc,
      &filter->upload_ctx, dst_surf, filter->upload_ctx.hw_id, &filter->info);
  gst_buffer_append_memory (outbuf, out_mem);
  gst_object_unref (alloc);

  GST_DEBUG_OBJECT (filter, "niquadrabgr in index=%d, out index=%d\n",
      in_surface->ui16FrameIdx, dst_surf->ui16FrameIdx);

  ni_frame_buffer_free (&p_dst_session_data->data.frame);

  outbuf = gst_buffer_make_writable (outbuf);
  gst_buffer_copy_into (outbuf, inbuf,
      GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_META, 0, -1);
  gst_buffer_unref (inbuf);
  flow_ret = gst_pad_push (filter->srcpad, outbuf);

  return flow_ret;

fail:
  if (clean_scale_format & CLEAN_SCALE) {
    ni_hwframe_buffer_recycle (downscale_bgr_surface,
        downscale_bgr_surface->device_handle);
  }
  if (clean_scale_format & CLEAN_FORMAT) {
    ni_hwframe_buffer_recycle (rgba_frame_surface,
        rgba_frame_surface->device_handle);
  }
  return flow_ret;
}

static void
gst_niquadrabgr_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiQuadraBgr *self;

  g_return_if_fail (GST_IS_NIQUADRABGR (object));
  self = GST_NIQUADRABGR (object);

  switch (prop_id) {
    case PROP_NB:
      self->nb_file = g_strdup (g_value_get_string (value));
      break;
    case PROP_SKIP:
      self->skip = g_value_get_uint (value);
      break;
    case PROP_KEEP_ALIVE_TIMEOUT:
      self->keep_alive_timeout = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadrabgr_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiQuadraBgr *self;

  g_return_if_fail (GST_IS_NIQUADRABGR (object));
  self = GST_NIQUADRABGR (object);

  switch (prop_id) {
    case PROP_NB:
      GST_OBJECT_LOCK (self);
      g_value_set_string (value, self->nb_file);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_SKIP:
      GST_OBJECT_LOCK (self);
      g_value_set_uint (value, self->skip);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_KEEP_ALIVE_TIMEOUT:
      GST_OBJECT_LOCK (self);
      g_value_set_uint (value, self->keep_alive_timeout);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadrabgr_class_init (GstNiQuadraBgrClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gobject_class->set_property = gst_niquadrabgr_set_property;
  gobject_class->get_property = gst_niquadrabgr_get_property;

  g_object_class_install_property (gobject_class, PROP_NB,
      g_param_spec_string ("nb", "NB",
          "File path of AI module network binary",
          NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_SKIP,
      g_param_spec_uint ("skip", "Skip",
          "Number of skip frames between inference", 0, G_MAXUINT, 0,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_KEEP_ALIVE_TIMEOUT,
      g_param_spec_uint ("keep-alive-timeout", "TIMEOUT",
          "Specify a custom session keep alive timeout in seconds. Defult is 3.",
          NI_MIN_KEEP_ALIVE_TIMEOUT, NI_MAX_KEEP_ALIVE_TIMEOUT,
          NI_DEFAULT_KEEP_ALIVE_TIMEOUT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (element_class, &src_factory);
  gst_element_class_add_static_pad_template (element_class, &sink_factory);

  gst_element_class_set_static_metadata (element_class,
      "NETINT Quadra BGR filter", "Filter/Effect/Video/NIBgr",
      "Bgr Netint Quadra", "Simon Shi <simon.shi@netint.cn>");

  gobject_class->dispose = gst_niquadrabgr_dispose;
}

static gboolean
niquadrabgr_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadrabgr_debug, "niquadrabgr",
      0, "niquadrabgr");

  return gst_element_register (plugin, "niquadrabgr", GST_RANK_NONE,
      GST_TYPE_NIQUADRABGR);
}
