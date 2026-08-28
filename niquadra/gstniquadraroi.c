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
 *  \file   GstNiQuadraRoi.c
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

GST_DEBUG_CATEGORY_STATIC (gst_niquadraroi_debug);
#define GST_CAT_DEFAULT gst_niquadraroi_debug

#define DEFAULT_QP_OFFSET 0.0
#define DEFAULT_OBJ_THRESH 0.25
#define DEFAULT_NMS_THRESH 0.45
#define DEFAULT_KEEP_ALIVE_TIMEOUT 3

enum
{
  PROP_0,
  PROP_NB,
  PROP_QPOFFSET,
  PROP_DEVID,
  PROP_OBJ_THRESH,
  PROP_NMS_THRESH,
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
  int32_t mask[3];
  float biases[12];
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

typedef struct box
{
  float x, y, w, h;
} box;

typedef struct detection
{
  box bbox;
  float objectness;
  int classes;
  int color;
  float *prob;
  int prob_class;
  float max_prob;
} detection;

typedef struct detetion_cache
{
  detection *dets;
  int capacity;
  int dets_num;
} detection_cache;

struct roi_box
{
  int left;
  int right;
  int top;
  int bottom;
  int color;
  float objectness;
  int cls;
};

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

typedef struct _GstNiQuadraRoi
{
  GstElement element;

  GstPad *sinkpad, *srcpad;

  gint in_width, in_height;
  GstVideoFormat outformat, informat;

  GstVideoInfo info;
  gchar *nb_file;               /* path to network binary */
  gfloat qp_offset;             /* default qp offset. */
  gboolean initialized;
  gint devid;
  gfloat obj_thresh;
  gfloat nms_thresh;

  AiContext *ai_ctx;

  ni_roi_network_t network;
  detection_cache det_cache;

  HwScaleContext *hws_ctx;
  gint keep_alive_timeout;      /* keep alive timeout setting */
  guint extra_frames;
  gint downstream_card;
} GstNiQuadraRoi;

typedef struct _GstNiQuadraRoiClass
{
  GstElementClass parent_class;
} GstNiQuadraRoiClass;

/* human face */
static int g_masks[2][3] = { {3, 4, 5}, {0, 1, 2} };
static float g_biases[] =
    { 10, 16, 25, 37, 49, 71, 85, 118, 143, 190, 274, 283 };

static int
entry_index (ni_roi_network_layer_t * l, int batch, int location, int entry)
{
  int n = location / (l->width * l->height);
  int loc = location % (l->width * l->height);
  return batch * l->output_number +
      n * l->width * l->height * (4 + l->classes + 1) +
      entry * l->width * l->height + loc;
}

static float
sigmoid (float x)
{
  return (float) (1.0 / (1.0 + (float) exp ((double) (-x))));
}

#define GST_TYPE_NIQUADRAROI \
  (gst_niquadraroi_get_type())
#define GST_NIQUADRAROI(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRAROI,GstNiQuadraRoi))
#define GST_NIQUADRAROI_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRAROI,GstNiQuadraRoi))
#define GST_IS_NIQUADRAROI(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRAROI))
#define GST_IS_NIQUADRAROI_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRAROI))

static gboolean niquadraroi_element_init (GstPlugin * plugin);

GType gst_niquadraroi_get_type (void);

G_DEFINE_TYPE (GstNiQuadraRoi, gst_niquadraroi, GST_TYPE_ELEMENT);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadraroi, niquadraroi_element_init);

#define SUPPORTED_FORMATS \
    "{ I420, YUY2, UYVY, NV12, ARGB, RGBA, AROI, ROIA, I420_10LE, P010_10LE, " \
    "NV16, ROIx, NV12_10LE32 }"

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

static GstFlowReturn gst_niquadra_roi_chain (GstPad * pad,
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
gst_niquadra_roi_sink_setcaps (GstPad * pad, GstObject * parent, GstCaps * caps)
{
  GstNiQuadraRoi *filter = GST_NIQUADRAROI (parent);
  GstStructure *structure = gst_caps_get_structure (caps, 0);
  GstCaps *src_caps;
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

  filter->informat = filter->outformat = filter->info.finfo->format;

  src_caps = gst_video_info_to_caps (&filter->info);

  GstVideoFormat fmts[] = { filter->outformat, -1 };
  gst_nifilter_caps_set_pixfmts (src_caps, fmts);

  gst_caps_set_features_simple (src_caps,
      gst_caps_features_from_string (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY));

  ret = gst_pad_set_caps (filter->srcpad, src_caps);
  gst_caps_unref (src_caps);

  return ret;
}

static gboolean
gst_niquadra_roi_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstNiQuadraRoi *filter = GST_NIQUADRAROI (parent);
  gboolean ret = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:{
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret = gst_niquadra_roi_sink_setcaps (pad, parent, caps);
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
gst_niquadraroi_init (GstNiQuadraRoi * filter)
{
  filter->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_event_function (filter->sinkpad, gst_niquadra_roi_sink_event);
  gst_pad_set_chain_function (filter->sinkpad, gst_niquadra_roi_chain);
  gst_element_add_pad (GST_ELEMENT (filter), filter->sinkpad);

  filter->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  gst_element_add_pad (GST_ELEMENT (filter), filter->srcpad);

  filter->devid = -1;
  filter->qp_offset = DEFAULT_QP_OFFSET;
  filter->nms_thresh = DEFAULT_NMS_THRESH;
  filter->obj_thresh = DEFAULT_OBJ_THRESH;
  filter->downstream_card = -1;
  filter->extra_frames = 0;
}

static void
cleanup_ai_context (GstNiQuadraRoi * filter)
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
ni_destroy_network (GstNiQuadraRoi * filter, ni_roi_network_t * network)
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
cleanup_hwframe_scale (GstNiQuadraRoi * filter)
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
gst_niquadraroi_dispose (GObject * obj)
{
  GstNiQuadraRoi *filter = GST_NIQUADRAROI (obj);

  cleanup_ai_context (filter);
  ni_destroy_network (filter, &filter->network);

  if (filter->det_cache.dets) {
    free (filter->det_cache.dets);
    filter->det_cache.dets = NULL;
  }

  cleanup_hwframe_scale (filter);

  if (filter->nb_file) {
    g_free (filter->nb_file);
    filter->nb_file = NULL;
  }

  G_OBJECT_CLASS (gst_niquadraroi_parent_class)->dispose (obj);
}

static gboolean
init_ai_context (GstNiQuadraRoi * filter, gint dev_idx)
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
    GST_ERROR_OBJECT (filter, "failed to open ai session. retval %d", retval);
    goto failed_out;
  }

  retval = ni_ai_config_network_binary (&ai_ctx->api_ctx, &network->raw,
      filter->nb_file);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter,
        "failed to configure ai session. retval %d", retval);
    goto failed_out;
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
ni_create_network (GstNiQuadraRoi * filter, ni_roi_network_t * network)
{
  int i;
  ni_network_data_t *ni_network = &network->raw;

  GST_DEBUG_OBJECT (filter,
      "network input number %d, output number %d\n",
      ni_network->input_num, ni_network->output_num);

  if (ni_network->input_num == 0 || ni_network->output_num == 0) {
    GST_ERROR_OBJECT (filter, "invalid network layer\n");
    return FALSE;
  }

  /* only support one input for now */
  if (ni_network->input_num != 1) {
    GST_ERROR_OBJECT (filter,
        "network input layer number %d not supported\n", ni_network->input_num);
    return FALSE;
  }

  /*  create network and its layers. */
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

    memcpy (network->layers[i].mask, &g_masks[i][0],
        sizeof (network->layers[i].mask));
    memcpy (network->layers[i].biases, &g_biases[0],
        sizeof (network->layers[i].biases));

    GST_DEBUG_OBJECT (filter,
        "network layer %d: w %d, h %d, ch %d, co %d, cl %d\n",
        i, network->layers[i].width, network->layers[i].height,
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
init_hwframe_scale (GstNiQuadraRoi * filter,
    GstVideoFormat format, gint dev_idx)
{
  int retval = 0;
  HwScaleContext *hws_ctx;
  int pool_size = DEFAULT_NI_FILTER_POOL_SIZE;

  hws_ctx = g_malloc (sizeof (HwScaleContext));
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
    goto failed_out;
  }

  if (hws_ctx->api_ctx.hw_id != filter->downstream_card) {
    pool_size += filter->extra_frames;
    GST_INFO_OBJECT (filter, "Increase frame pool by %d", filter->extra_frames);
  }

  /* Create scale frame pool on device */
  retval = ni_build_frame_pool (&hws_ctx->api_ctx, filter->network.netw,
      filter->network.neth, format, pool_size);
  if (retval < 0) {
    GST_ERROR_OBJECT (filter, "could not build frame pool, ret=%d\n", retval);
    ni_device_session_close (&hws_ctx->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
    goto failed_out;
  }

  return TRUE;

failed_out:
  cleanup_hwframe_scale (filter);
  return FALSE;
}

static gboolean
ni_roi_config_input (GstNiQuadraRoi * filter, gint dev_idx)
{
  if (filter->initialized)
    return TRUE;

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

  filter->initialized = TRUE;
  return TRUE;

fail_out:
  cleanup_ai_context (filter);
  ni_destroy_network (filter, &filter->network);
  return FALSE;
}

static gboolean
ni_hwframe_scale (GstNiQuadraRoi * filter, niFrameSurface1_t * frame_surface,
    int w, int h, niFrameSurface1_t ** filt_frame_surface)
{
  HwScaleContext *scale_ctx = filter->hws_ctx;
  niFrameSurface1_t *new_frame_surface = NULL;
  int scaler_format;
  int retval = 0;

  GST_DEBUG_OBJECT (filter,
      "in frame surface frameIdx %d,w=%d,h=%d",
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
  retval =
      ni_device_alloc_frame (&scale_ctx->api_ctx, NI_ALIGN (filter->info.width,
          2), NI_ALIGN (filter->info.height, 2), scaler_format, 0, 0, 0, 0, 0,
      frame_surface->ui32nodeAddress, frame_surface->ui16FrameIdx,
      NI_DEVICE_TYPE_SCALER);

  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "Can't allocate device input frame %d", retval);
    return FALSE;
  }

  /* Allocate hardware device destination frame. This acquires a frame from
   * the pool */
  retval =
      ni_device_alloc_frame (&scale_ctx->api_ctx, NI_ALIGN (w, 2), NI_ALIGN (h,
          2), convertGstVideoFormatToGC620Format (GST_VIDEO_FORMAT_BGRP),
      NI_SCALER_FLAG_IO, 0, 0, 0, 0, 0, -1, NI_DEVICE_TYPE_SCALER);

  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "Can't allocate device output frame %d", retval);
    return FALSE;
  }

  /* Set the new frame index */
  retval = ni_device_session_read_hwdesc (&scale_ctx->api_ctx,
      &scale_ctx->api_dst_frame, NI_DEVICE_TYPE_SCALER);
  if (retval < 0) {
    GST_ERROR_OBJECT (filter, "Can't allocate device output frame %d", retval);
    return FALSE;
  }

  new_frame_surface =
      (niFrameSurface1_t *) scale_ctx->api_dst_frame.data.frame.p_data[3];

  *filt_frame_surface = new_frame_surface;

  return TRUE;
}

/*
 * nw: network input width
 * nh: network input height
 * lw: layer width
 * lh: layer height
 */
static box
get_yolo_box (float *x, float *biases, int n, int index, int col,
    int row, int lw, int lh, int nw, int nh, int stride)
{
  GST_DEBUG
      ("n=%d, col=%d, row=%d, lw=%d, lh=%d, nw=%d, nh=%d, index=%d, stride=%d\n",
      n, col, row, lw, lh, nw, nh, index, stride);
  box b;

  b.x = (float) ((float) col + sigmoid (x[index + 0 * stride])) / (float) lw;
  b.y = (float) ((float) row + sigmoid (x[index + 1 * stride])) / (float) lh;
  b.w =
      (float) exp ((double) x[index + 2 * stride]) * biases[2 * n] / (float) nw;
  b.h =
      (float) exp ((double) x[index + 3 * stride]) * biases[2 * n +
      1] / (float) nh;

  b.x -= (float) (b.w / 2.0);
  b.y -= (float) (b.h / 2.0);

  return b;
}

static gboolean
get_yolo_detections (void *ctx, ni_roi_network_layer_t * l, int netw,
    int neth, float thresh, detection_cache * det_cache, int *dets_num)
{
  GstNiQuadraRoi *filter = GST_NIQUADRAROI (ctx);
  int i, n, k;
  float *predictions = l->output;
  float max_prob;
  int prob_class;
  int count = 0;
  detection *dets = det_cache->dets;
  *dets_num = 0;

  GST_DEBUG_OBJECT (filter,
      "pic %dx%d, comp=%d, class=%d, net %dx%d, thresh=%f",
      l->width, l->height, l->component, l->classes, netw, neth, thresh);
  for (i = 0; i < l->width * l->height; ++i) {
    int row = i / l->width;
    int col = i % l->width;
    for (n = 0; n < l->component; ++n) {
      int obj_index = entry_index (l, 0, n * l->width * l->height + i, 4);
      float objectness = predictions[obj_index];
      objectness = sigmoid (objectness);

      prob_class = -1;
      max_prob = thresh;
      for (k = 0; k < l->classes; k++) {
        int class_index =
            entry_index (l, 0, n * l->width * l->height + i, 4 + 1 + k);
        double prob = objectness * sigmoid (predictions[class_index]);
        if (prob >= max_prob) {
          prob_class = k;
          max_prob = (float) prob;
        }
      }

      if (prob_class >= 0) {
        box bbox;
        int box_index = entry_index (l, 0, n * l->width * l->height + i, 0);

        if (det_cache->dets_num >= det_cache->capacity) {
          dets =
              realloc (det_cache->dets,
              sizeof (detection) * (det_cache->capacity + 10));
          if (!dets) {
            GST_ERROR_OBJECT (filter,
                "failed to realloc detections capacity %d",
                det_cache->capacity);
            return FALSE;
          }
          det_cache->dets = dets;
          det_cache->capacity += 10;
          if (det_cache->capacity >= 100) {
            GST_WARNING_OBJECT (filter,
                "too many detections %d", det_cache->dets_num);
          }
        }

        GST_DEBUG_OBJECT (filter,
            "max_prob %f, class %d, n=%d, mask=%d",
            max_prob, prob_class, n, l->mask[n]);
        bbox = get_yolo_box (predictions, l->biases, l->mask[n],
            box_index, col, row, l->width, l->height,
            netw, neth, l->width * l->height);

        dets[det_cache->dets_num].max_prob = max_prob;
        dets[det_cache->dets_num].prob_class = prob_class;
        dets[det_cache->dets_num].bbox = bbox;
        dets[det_cache->dets_num].objectness = objectness;
        dets[det_cache->dets_num].classes = l->classes;
        dets[det_cache->dets_num].color = n;

        GST_DEBUG_OBJECT (filter,
            "%d, x %f, y %f, w %f, h %f",
            det_cache->dets_num,
            dets[det_cache->dets_num].bbox.x, dets[det_cache->dets_num].bbox.y,
            dets[det_cache->dets_num].bbox.w, dets[det_cache->dets_num].bbox.h);
        det_cache->dets_num++;
        count++;
      }
    }
  }
  *dets_num = count;
  return TRUE;
}

static int
nms_comparator (const void *pa, const void *pb)
{
  detection *a = (detection *) pa;
  detection *b = (detection *) pb;

  if (a->prob_class > b->prob_class)
    return 1;
  else if (a->prob_class < b->prob_class)
    return -1;
  else {
    if (a->max_prob < b->max_prob)
      return 1;
    else if (a->max_prob > b->max_prob)
      return -1;
  }
  return 0;
}

static float
overlap (float x1, float w1, float x2, float w2)
{
  float l1 = x1 - w1 / 2;
  float l2 = x2 - w2 / 2;
  float left = l1 > l2 ? l1 : l2;
  float r1 = x1 + w1 / 2;
  float r2 = x2 + w2 / 2;
  float right = r1 < r2 ? r1 : r2;
  return right - left;
}

static float
box_intersection (box a, box b)
{
  float w = overlap (a.x, a.w, b.x, b.w);
  float h = overlap (a.y, a.h, b.y, b.h);
  float area;

  if (w < 0 || h < 0)
    return 0;

  area = w * h;
  return area;
}

static float
box_union (box a, box b)
{
  float i = box_intersection (a, b);
  float u = a.w * a.h + b.w * b.h - i;
  return u;
}

static float
box_iou (box a, box b)
{
  float I = box_intersection (a, b);
  float U = box_union (a, b);
  if (I == 0 || U == 0)
    return 0;

  return I / U;
}

static NiPluginError
nms_sort (void *ctx, detection * dets, int dets_num, float nms_thresh)
{
  int i, j;
  box boxa, boxb;

  if (!dets) {
    return NI_PLUGIN_EINVAL;
  }

  for (i = 0; i < (dets_num - 1); i++) {
    int class = dets[i].prob_class;
    if (dets[i].max_prob == 0)
      continue;

    if (dets[i].prob_class != dets[i + 1].prob_class)
      continue;

    boxa = dets[i].bbox;
    for (j = i + 1; j < dets_num && dets[j].prob_class == class; j++) {
      if (dets[j].max_prob == 0)
        continue;

      boxb = dets[j].bbox;
      if (box_iou (boxa, boxb) > nms_thresh)
        dets[j].max_prob = 0;
    }
  }

  return NI_PLUGIN_OK;
}

static gboolean
resize_coords (void *ctx, detection * dets, int dets_num,
    uint32_t img_width, uint32_t img_height,
    struct roi_box **roi_box, int *roi_num)
{
  GstNiQuadraRoi *filter = GST_NIQUADRAROI (ctx);
  int i;
  int left, right, top, bot;
  struct roi_box *rbox;
  int rbox_num = 0;

  if (dets_num == 0) {
    return TRUE;
  }

  rbox = g_new0 (struct roi_box, dets_num);
  if (!rbox)
    return FALSE;

  for (i = 0; i < dets_num; i++) {
    GST_DEBUG_OBJECT (filter,
        "index %d, max_prob %f, class %d",
        i, dets[i].max_prob, dets[i].prob_class);
    if (dets[i].max_prob == 0)
      continue;

    top = (int) floor (dets[i].bbox.y * img_height + 0.5);
    left = (int) floor (dets[i].bbox.x * img_width + 0.5);
    right = (int) floor ((dets[i].bbox.x + dets[i].bbox.w) * img_width + 0.5);
    bot = (int) floor ((dets[i].bbox.y + dets[i].bbox.h) * img_height + 0.5);

    if (top < 0)
      top = 0;

    if (left < 0)
      left = 0;

    if (right > img_width)
      right = img_width;

    if (bot > img_height)
      bot = img_height;

    GST_DEBUG_OBJECT (filter,
        "top %d, left %d, right %d, bottom %d\n", top, left, right, bot);

    rbox[rbox_num].left = left;
    rbox[rbox_num].right = right;
    rbox[rbox_num].top = top;
    rbox[rbox_num].bottom = bot;
    rbox[rbox_num].cls = dets[i].prob_class;
    rbox[rbox_num].objectness = dets[i].objectness;
    rbox[rbox_num].color = dets[i].color;
    rbox_num++;
  }

  if (rbox_num == 0) {
    free (rbox);
    *roi_num = rbox_num;
    *roi_box = NULL;
  } else {
    *roi_num = rbox_num;
    *roi_box = rbox;
  }

  return TRUE;
}

static gboolean
ni_get_detections (GstNiQuadraRoi * filter, ni_roi_network_t * network,
    detection_cache * det_cache, uint32_t img_width,
    uint32_t img_height, float obj_thresh,
    float nms_thresh, struct roi_box **roi_box, int *roi_num)
{
  GST_DEBUG_OBJECT (filter, "obj=%f, nms=%f\n", obj_thresh, nms_thresh);
  int i;
  int dets_num = 0;
  detection *dets = NULL;

  *roi_box = NULL;
  *roi_num = 0;

  for (i = 0; i < network->raw.output_num; i++) {
    if (!get_yolo_detections (filter, &network->layers[i], network->netw,
            network->neth, obj_thresh, det_cache, &dets_num)) {
      GST_ERROR_OBJECT (filter, "failed to get yolo detection at layer %d", i);
      return FALSE;
    }
    GST_DEBUG_OBJECT (filter, "layer %d, yolo detections: %d", i, dets_num);
  }

  if (det_cache->dets_num == 0)
    return TRUE;

  dets = det_cache->dets;
  dets_num = det_cache->dets_num;
  for (i = 0; i < dets_num; i++) {
    GST_DEBUG_OBJECT (filter,
        "orig dets %d: x %f,y %f,w %f,h %f,c %d,p %f\n",
        i, dets[i].bbox.x, dets[i].bbox.y, dets[i].bbox.w, dets[i].bbox.h,
        dets[i].prob_class, dets[i].max_prob);
  }

  qsort (dets, dets_num, sizeof (detection), nms_comparator);
  for (i = 0; i < dets_num; i++) {
    GST_DEBUG_OBJECT (filter,
        "sorted dets %d: x %f,y %f,w %f,h %f,c %d,p %f\n",
        i, dets[i].bbox.x, dets[i].bbox.y, dets[i].bbox.w, dets[i].bbox.h,
        dets[i].prob_class, dets[i].max_prob);
  }

  nms_sort (filter, dets, dets_num, nms_thresh);

  if (!resize_coords (filter, dets, dets_num, img_width, img_height, roi_box,
          roi_num)) {
    GST_ERROR_OBJECT (filter, "cannot resize coordinates\n");
    return FALSE;
  }

  return TRUE;
}

static gboolean
ni_read_roi (GstNiQuadraRoi * filter, ni_session_data_io_t * p_dst_pkt,
    GstBuffer * inbuf, GstBuffer * outbuf, int pic_width, int pic_height)
{
  int retval = 0;
  ni_roi_network_t *network = &filter->network;
  struct roi_box *roi_box = NULL;
  int roi_num = 0;
  int i;
  int width, height;

  for (i = 0; i < network->raw.output_num; i++) {
    retval = ni_network_layer_convert_output (network->layers[i].output,
        network->layers[i].output_number * sizeof (float),
        &p_dst_pkt->data.packet, &network->raw, i);
    if (retval != NI_RETCODE_SUCCESS) {
      GST_ERROR_OBJECT (filter,
          "failed to read layer %d output. retval %d\n", i, retval);
      return FALSE;
    }
  }

  width = pic_width;
  height = pic_height;

  filter->det_cache.dets_num = 0;
  if (!ni_get_detections (filter, network, &filter->det_cache, width, height,
          filter->obj_thresh, filter->nms_thresh, &roi_box, &roi_num)) {
    GST_ERROR_OBJECT (filter, "failed to get roi.\n");
    return FALSE;
  }

  if (roi_num == 0) {
    GST_DEBUG_OBJECT (filter, "no roi available\n");
    return TRUE;
  }

  for (i = 0; i < roi_num; i++) {
    char roi_type[32];
    GstVideoRegionOfInterestMeta *rmeta;
    GstStructure *s;
    snprintf (roi_type, sizeof (roi_type), "niquadraroi%d", i);
    int roi_width = roi_box[i].right - roi_box[i].left;
    int roi_height = roi_box[i].bottom - roi_box[i].top;
    GST_DEBUG_OBJECT (filter,
        "Got roi info, x=%d,y=%d,w=%d,h=%d\n",
        roi_box[i].left, roi_box[i].top, roi_width, roi_height);
    rmeta =
        gst_buffer_add_video_region_of_interest_meta (outbuf, roi_type,
        roi_box[i].left, roi_box[i].top, roi_width, roi_height);
    s = gst_structure_new ("roi/niquadra", "delta-qp", G_TYPE_DOUBLE,
        filter->qp_offset, NULL);
    gst_video_region_of_interest_meta_add_param (rmeta, s);
  }

  free (roi_box);
  return TRUE;
}

static GstFlowReturn
gst_niquadra_roi_chain (GstPad * pad, GstObject * parent, GstBuffer * inbuf)
{
  GstNiQuadraRoi *filter = GST_NIQUADRAROI (parent);
  GstMemory *in_mem = NULL;
  niFrameSurface1_t *in_surface = NULL;
  gint dev_idx = -1;
  GstBuffer *outbuf = NULL;
  GstMemory *out_mem = NULL;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  int retval = 0;
  AiContext *ai_ctx;
  ni_roi_network_t *network;

  if (inbuf == NULL) {
    GST_ERROR_OBJECT (filter, "in frame is null");
    return GST_FLOW_ERROR;
  }

  in_mem = gst_buffer_peek_memory (inbuf, 0);
  dev_idx = gst_deviceid_from_ni_hw_memory (in_mem);
  in_surface = gst_surface_from_ni_hw_memory (in_mem);
  if (in_surface == NULL) {
    GST_ERROR_OBJECT (filter,
        "Impossible to convert between the formats supported by the filter");
    if (inbuf) {
      gst_buffer_unref (inbuf);
    }
    return GST_FLOW_ERROR;
  }

  if (!filter->initialized) {
    if (!ni_roi_config_input (filter, dev_idx)) {
      GST_ERROR_OBJECT (filter, "Failed to config input");
      return GST_FLOW_ERROR;
    }
  }

  ai_ctx = filter->ai_ctx;
  network = &filter->network;

  retval = ni_ai_packet_buffer_alloc (&ai_ctx->api_dst_pkt.data.packet,
      &network->raw);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "failed to allocate packet");
    return GST_FLOW_ERROR;
  }

  niFrameSurface1_t *filt_frame_surface;

  if (!ni_hwframe_scale (filter, in_surface, network->netw, network->neth,
          &filt_frame_surface)) {
    GST_ERROR_OBJECT (filter, "Error run hwframe scale");
    flow_ret = GST_FLOW_ERROR;
    goto failed_out;
  }

  GST_DEBUG_OBJECT (filter,
      "filter frame surface frameIdx %d", filt_frame_surface->ui16FrameIdx);

  /* allocate output buffer */
  retval = ni_device_alloc_frame (&ai_ctx->api_ctx, 0, 0, 0, 0, 0, 0, 0, 0,
      filt_frame_surface->ui32nodeAddress,
      filt_frame_surface->ui16FrameIdx, NI_DEVICE_TYPE_AI);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "failed to alloc hw input frame\n");
    flow_ret = GST_FLOW_ERROR;
    goto failed_out;
  }

  outbuf = gst_buffer_new ();
  outbuf = gst_buffer_make_writable (outbuf);

  do {
    retval = ni_device_session_read (&ai_ctx->api_ctx,
        &ai_ctx->api_dst_pkt, NI_DEVICE_TYPE_AI);
    if (retval < 0) {
      GST_ERROR_OBJECT (filter, "read hwdesc retval %d\n", retval);
      flow_ret = GST_FLOW_ERROR;
      goto failed_out;
    } else if (retval > 0) {
      if (!ni_read_roi (filter, &ai_ctx->api_dst_pkt, inbuf, outbuf,
              filter->info.width, filter->info.height)) {
        GST_ERROR_OBJECT (filter, "failed to read roi from packet\n");
        flow_ret = GST_FLOW_ERROR;
        goto failed_out;
      }
    }
  } while (retval == 0);

  ni_hwframe_buffer_recycle (filt_frame_surface,
      filt_frame_surface->device_handle);

  out_mem = gst_memory_ref (in_mem);
  gst_buffer_append_memory (outbuf, out_mem);

  gst_buffer_copy_into (outbuf, inbuf,
      GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_META, 0, -1);
  gst_buffer_unref (inbuf);

  flow_ret = gst_pad_push (filter->srcpad, outbuf);
  return flow_ret;

failed_out:
  if (inbuf) {
    gst_buffer_unref (inbuf);
  }
  return flow_ret;
}

static void
gst_niquadraroi_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiQuadraRoi *self;

  g_return_if_fail (GST_IS_NIQUADRAROI (object));
  self = GST_NIQUADRAROI (object);

  GST_OBJECT_LOCK (self);

  switch (prop_id) {
    case PROP_NB:
      g_free (self->nb_file);
      self->nb_file = g_strdup (g_value_get_string (value));
      break;
    case PROP_DEVID:
      self->devid = g_value_get_int (value);
      break;
    case PROP_KEEP_ALIVE_TIMEOUT:
      self->keep_alive_timeout = g_value_get_uint (value);
      break;
    case PROP_QPOFFSET:
      if (self->qp_offset != g_value_get_float (value)) {
        self->qp_offset = g_value_get_float (value);
      }
      break;
    case PROP_OBJ_THRESH:
      if (self->obj_thresh != g_value_get_float (value)) {
        self->obj_thresh = g_value_get_float (value);
      }
      break;
    case PROP_NMS_THRESH:
      if (self->nms_thresh != g_value_get_float (value)) {
        self->nms_thresh = g_value_get_float (value);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_niquadraroi_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiQuadraRoi *self;

  g_return_if_fail (GST_IS_NIQUADRAROI (object));
  self = GST_NIQUADRAROI (object);

  switch (prop_id) {
    case PROP_NB:
      GST_OBJECT_LOCK (self);
      g_value_set_string (value, self->nb_file);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_DEVID:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->devid);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_KEEP_ALIVE_TIMEOUT:
      GST_OBJECT_LOCK (self);
      g_value_set_uint (value, self->keep_alive_timeout);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_QPOFFSET:
      GST_OBJECT_LOCK (self);
      g_value_set_float (value, self->qp_offset);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_OBJ_THRESH:
      GST_OBJECT_LOCK (self);
      g_value_set_float (value, self->obj_thresh);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_NMS_THRESH:
      GST_OBJECT_LOCK (self);
      g_value_set_float (value, self->nms_thresh);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadraroi_class_init (GstNiQuadraRoiClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gobject_class->set_property = gst_niquadraroi_set_property;
  gobject_class->get_property = gst_niquadraroi_get_property;

  g_object_class_install_property (gobject_class, PROP_NB,
      g_param_spec_string ("nb", "NB",
          "File path of AI module network binary",
          NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_KEEP_ALIVE_TIMEOUT,
      g_param_spec_uint ("keep-alive-timeout", "Keep-alive-timeout",
          "Specify a custom session keep alive timeout in seconds",
          NI_MIN_KEEP_ALIVE_TIMEOUT, NI_MAX_KEEP_ALIVE_TIMEOUT,
          NI_DEFAULT_KEEP_ALIVE_TIMEOUT,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_QPOFFSET,
      g_param_spec_float ("qpoffset", "QpOffset",
          "Specific QP in these regions based on QP offset", -1.0, 1.0,
          DEFAULT_QP_OFFSET,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT |
          GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_DEVID,
      g_param_spec_int ("devid", "Device",
          "Specific the device id of quadra hardware", G_MININT, G_MAXINT, 0,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_OBJ_THRESH,
      g_param_spec_float ("objthresh", "ObjThresh",
          "Specific the yolov4 post processing object threshold", 0.0, 1.0,
          DEFAULT_OBJ_THRESH,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT |
          GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_NMS_THRESH,
      g_param_spec_float ("nmsthresh", "NmsThresh",
          "Specific the yolov4 post processing NMS IOU threshold", 0.0, 1.0,
          DEFAULT_NMS_THRESH,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT |
          GST_PARAM_CONTROLLABLE));
  gst_element_class_add_static_pad_template (element_class, &src_factory);
  gst_element_class_add_static_pad_template (element_class, &sink_factory);

  gst_element_class_set_static_metadata (element_class,
      "NETINT Quadra ROI filter", "Filter/Effect/Video/NIRoi",
      "Roi Netint Quadra", "Simon Shi <simon.shi@netint.cn>");

  gobject_class->dispose = gst_niquadraroi_dispose;
}

static gboolean
niquadraroi_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadraroi_debug, "niquadraroi", 0,
      "niquadraroi");

  return gst_element_register (plugin, "niquadraroi", GST_RANK_NONE,
      GST_TYPE_NIQUADRAROI);
}
