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
 *  \file   GstNiQuadraAiPre.c
 *
 *  \brief  Implement of NetInt Quadra hardware aipre filter.
 ******************************************************************************/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/video.h>
#include <unistd.h>

#include "niquadra.h"
#include "ni_device_api.h"
#include "gstniquadramemory.h"
#include "gstniquadrautils.h"
#include "gstniquadrabaseconvert.h"

GST_DEBUG_CATEGORY_STATIC (gst_niquadraaipre_debug);
#define GST_CAT_DEFAULT gst_niquadraaipre_debug

enum
{
  PROP_0,
  PROP_NB,
  PROP_DEVID,
  PROP_MODE,
  PROP_TIMEOUT,
  PROP_LAST
};

typedef struct _ni_ai_pre_network_layer
{
  int32_t width;
  int32_t height;
  int32_t channel;
  int32_t classes;
  int32_t component;
  int32_t output_number;
  float *output;
} ni_ai_pre_network_layer_t;

typedef struct _ni_ai_pre_network
{
  int32_t netw;
  int32_t neth;
  int32_t net_out_w;
  int32_t net_out_h;
  ni_network_data_t raw;
  ni_ai_pre_network_layer_t *layers;
} ni_ai_pre_network_t;

typedef struct AiContext
{
  ni_session_context_t api_ctx;
  ni_session_data_io_t api_dst_frame;
  ni_session_data_io_t api_src_frame;
} AiContext;

typedef struct _GstNiQuadraAiPre
{
  GstNiQuadraBaseConvert parent;

  AiContext *ai_ctx;
  ni_ai_pre_network_t network;
  gchar *nb_file;               /* path to network binary */

  guint ai_timeout;
  gint channel_mode;
  gint devid;

  guint extra_frames;
  gint downstream_card;
  gboolean hw_mode;
} GstNiQuadraAiPre;

typedef struct _GstNiQuadraAiPreClass
{
  GstNiQuadraBaseConvertClass parent_class;
} GstNiQuadraAiPreClass;

#define GST_TYPE_NIQUADRAAIPRE \
  (gst_niquadraaipre_get_type())
#define GST_NIQUADRAAIPRE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRAAIPRE,GstNiQuadraAiPre))
#define GST_NIQUADRAAIPRE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRAAIPRE,GstNiQuadraAiPre))
#define GST_IS_NIQUADRAAIPRE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRAAIPRE))
#define GST_IS_NIQUADRAAIPRE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRAAIPRE))

static gboolean niquadraaipre_element_init (GstPlugin * plugin);

#define gst_niquadraaipre_parent_class parent_class

GType gst_niquadraaipre_get_type (void);

G_DEFINE_TYPE (GstNiQuadraAiPre, gst_niquadraaipre,
    GST_TYPE_NI_QUADRA_BASE_CONVERT);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadraaipre, niquadraaipre_element_init);

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
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (SUPPORTED_FORMATS) "; "
        GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY, SUPPORTED_FORMATS))
    );

static GstCaps *
gst_niquadraaipre_fixate_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GstNiQuadraAiPre *self = GST_NIQUADRAAIPRE (base);
  GstVideoInfo info;

  GST_DEBUG_OBJECT (base,
      "trying to fixate othercaps %" GST_PTR_FORMAT " based on caps %"
      GST_PTR_FORMAT, othercaps, caps);

  if (direction == GST_PAD_SINK) {
    GstStructure *outs;
    gint w = 0, h = 0;
    gboolean is_downstream_configured = FALSE;

    if (!gst_video_info_from_caps (&info, caps)) {
      return FALSE;
    }

    outs = gst_caps_get_structure (othercaps, 0);
    if ((gst_structure_get_int (outs, "width", &w)) ||
        (gst_structure_get_int (outs, "height", &h))) {
      is_downstream_configured = TRUE;
    }

    if (!is_downstream_configured) {
      GST_WARNING_OBJECT (self, "downstream must configure dimension");
    }
  }

  othercaps = GST_NI_QUADRA_BASE_CONVERT_CLASS
      (parent_class)->fixate_size (base, direction, caps, othercaps);
  if (gst_caps_get_size (othercaps) == 1) {
    gint i;
    const gchar *format_fields[] = { "format", "colorimetry", "chroma-site" };
    GstStructure *format_struct = gst_caps_get_structure (caps, 0);
    GstStructure *fixated_struct;

    othercaps = gst_caps_make_writable (othercaps);
    fixated_struct = gst_caps_get_structure (othercaps, 0);

    for (i = 0; i < G_N_ELEMENTS (format_fields); i++) {
      const gchar *field = format_fields[i];
      const gchar *new_value;
      const gchar *old_value = NULL;

      if (!gst_structure_has_field (format_struct, field)) {
        gst_structure_remove_field (fixated_struct, field);
        continue;
      }

      new_value = gst_structure_get_string (format_struct, field);

      if (gst_structure_has_field (fixated_struct, field)) {
        gst_structure_fixate_field_string (fixated_struct, field, new_value);
        old_value = gst_structure_get_string (fixated_struct, field);
      }

      if (gst_structure_has_field (fixated_struct, field) &&
          g_strcmp0 (old_value, new_value) != 0 &&
          g_strcmp0 (field, "format") == 0) {
        GST_ERROR_OBJECT (self, "%s mismatch: %s vs %s",
            field, new_value, GST_STR_NULL (old_value));
        gst_caps_unref (othercaps);
        return gst_caps_new_empty ();
      } else {
        gst_structure_set (fixated_struct, field,
            G_TYPE_STRING, new_value, NULL);
      }
    }
  }

  return othercaps;
}

static void
cleanup_ai_context (GstNiQuadraAiPre * filter)
{
  int retval = 0;
  AiContext *ai_ctx = filter->ai_ctx;

  if (ai_ctx) {
    ni_session_context_t *p_ctx = &ai_ctx->api_ctx;

    ni_frame_buffer_free (&ai_ctx->api_src_frame.data.frame);

    retval = ni_device_session_close (&ai_ctx->api_ctx, 1, NI_DEVICE_TYPE_AI);
    if (retval != NI_RETCODE_SUCCESS) {
      GST_ERROR_OBJECT (filter, "%s: failed to close ai session. retval %d\n",
          __func__, retval);
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
    if (ai_ctx->api_ctx.hw_action != NI_CODEC_HW_ENABLE) {
      ni_packet_buffer_free (&ai_ctx->api_dst_frame.data.packet);
    } else {
      ni_frame_buffer_free (&ai_ctx->api_dst_frame.data.frame);
    }
    ni_device_session_context_clear (&ai_ctx->api_ctx);
    g_free (ai_ctx);
    filter->ai_ctx = NULL;
  }
}

static void
ni_destroy_network (GstNiQuadraAiPre * filter, ni_ai_pre_network_t * network)
{
  if (network) {
    int i;
    if (network->layers) {
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
}

static void
gst_niquadraaipre_finalize (GObject * obj)
{
  GstNiQuadraAiPre *filter = GST_NIQUADRAAIPRE (obj);

  cleanup_ai_context (filter);
  ni_destroy_network (filter, &filter->network);
  if (filter->nb_file) {
    g_free (filter->nb_file);
    filter->nb_file = NULL;
  }

  G_OBJECT_CLASS (gst_niquadraaipre_parent_class)->finalize (obj);
}

static void
gst_niquadraaipre_query_downstream_allocation (GstNiQuadraAiPre * self,
    GstBaseTransform * trans, GstCaps * caps)
{
  GstQuery *query;
  GstPad *srcpad;
  guint i;
  GType gtype;
  const GstStructure *params = NULL;
  gboolean gotit;

  self->extra_frames = 0;
  self->downstream_card = -1;

  srcpad = gst_element_get_static_pad (GST_ELEMENT (trans), "src");
  if (!srcpad) {
    return;
  }

  query = gst_query_new_allocation (caps, TRUE);

  if (gst_pad_peer_query (srcpad, query) == TRUE) {
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
  }

  gst_query_unref (query);
  gst_object_unref (srcpad);
}

static gboolean
init_ai_context (GstNiQuadraAiPre * filter, gint dev_idx)
{
  GstNiQuadraBaseConvert *parent = GST_NI_QUADRA_BASE_CONVERT (filter);
  int retval = 0;
  AiContext *ai_ctx;
  ni_ai_pre_network_t *network = &filter->network;
  int gc620_pixfmt;
  ni_pix_fmt_t niPixFmt;

  if ((filter->nb_file == NULL) || (access (filter->nb_file, R_OK) != 0)) {
    GST_ERROR_OBJECT (filter, "invalid network binary path\n");
    return FALSE;
  }

  ai_ctx = g_malloc (sizeof (AiContext));
  if (!ai_ctx) {
    GST_ERROR_OBJECT (filter, "failed to allocate ai context\n");
    return FALSE;
  }
  filter->ai_ctx = ai_ctx;

  memset (&ai_ctx->api_dst_frame, 0, sizeof (ni_session_data_io_t));
  memset (&ai_ctx->api_src_frame, 0, sizeof (ni_session_data_io_t));

  retval = ni_device_session_context_init (&ai_ctx->api_ctx);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "ai session context init failure\n");
    return FALSE;
  }

  if (filter->hw_mode) {
    ai_ctx->api_ctx.session_id = NI_INVALID_SESSION_ID;
    ai_ctx->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
    ai_ctx->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;
    ai_ctx->api_ctx.hw_action = NI_CODEC_HW_ENABLE;
    ai_ctx->api_ctx.hw_id = dev_idx;
  } else {
    ai_ctx->api_ctx.hw_id = filter->devid;
  }
  ai_ctx->api_ctx.device_type = NI_DEVICE_TYPE_AI;
  ai_ctx->api_ctx.keep_alive_timeout = parent->keep_alive_timeout;

  retval = ni_device_session_open (&ai_ctx->api_ctx, NI_DEVICE_TYPE_AI);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "failed to open ai session. retval %d\n", retval);
    goto failed_out;
  }

  retval = ni_ai_config_network_binary (&ai_ctx->api_ctx, &network->raw,
      filter->nb_file);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "failed to configure ai session. retval %d\n",
        retval);
    goto failed_out;
  }

  if (!filter->hw_mode) {
    return TRUE;
  }

  int options;
  options = NI_AI_FLAG_IO | NI_AI_FLAG_PC;
  int pool_size = DEFAULT_NI_FILTER_POOL_SIZE;
  // If the downstream element is on a different card,
  // Allocate extra frames suggested by the downstream element
  if (ai_ctx->api_ctx.hw_id != filter->downstream_card) {
    pool_size += filter->extra_frames;
    GST_INFO_OBJECT (filter,
        "Increase frame pool by %d \n", filter->extra_frames);
  }

  niPixFmt =
      convertGstVideoFormatToNIPix (GST_VIDEO_INFO_FORMAT (&parent->in_info));
  gc620_pixfmt = convertNIPixToGC620Format (niPixFmt);

  /* Allocate a pool of frames by the scaler */
  /* *INDENT-OFF* */
  retval = ni_device_alloc_frame (&ai_ctx->api_ctx,
      NIALIGN (GST_VIDEO_INFO_WIDTH (&parent->out_info), 2),
      NIALIGN (GST_VIDEO_INFO_HEIGHT (&parent->out_info), 2),
      gc620_pixfmt,
      options,
      0,                        // rec width
      0,                        // rec height
      0,                        // rec X pos
      0,                        // rec Y pos
      pool_size,                // rgba color/pool size
      0,                        // frame index
      NI_DEVICE_TYPE_AI);
  /* *INDENT-ON* */
  if (retval < 0) {
    GST_ERROR_OBJECT (filter, "Init frame pool error\n");
    goto failed_out;
  }

  retval = ni_frame_buffer_alloc_hwenc (&ai_ctx->api_dst_frame.data.frame,
      NIALIGN (GST_VIDEO_INFO_WIDTH (&parent->out_info), 2),
      NIALIGN (GST_VIDEO_INFO_HEIGHT (&parent->out_info), 2), 0);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "failed to allocate ni dst frame\n");
    goto failed_out;
  }

  return TRUE;

failed_out:
  cleanup_ai_context (filter);
  return FALSE;
}

static gboolean
ni_create_network (GstNiQuadraAiPre * filter, ni_ai_pre_network_t * network)
{
  int i;
  ni_network_data_t *ni_network = &network->raw;

  GST_DEBUG_OBJECT (filter, "network input number %d, output number %d\n",
      ni_network->input_num, ni_network->output_num);

  if (ni_network->input_num == 0 || ni_network->output_num == 0) {
    GST_ERROR_OBJECT (filter, "invalid network layer\n");
    return FALSE;
  }
  // create network and its layers.
  network->layers = g_new0 (ni_ai_pre_network_layer_t, ni_network->output_num);
  if (!network->layers) {
    GST_ERROR_OBJECT (filter, "cannot allocate network layer memory\n");
    return FALSE;
  }
  memset (network->layers, 0,
      sizeof (ni_ai_pre_network_layer_t) * ni_network->output_num);

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
          "failed to allocate network layer %d output buffer", i);
      goto failed_out;
    }

    GST_DEBUG_OBJECT (filter, "network layer %d: w %d, h %d, ch %d, co %d, "
        "cl %d\n", i, network->layers[i].width, network->layers[i].height,
        network->layers[i].channel, network->layers[i].component,
        network->layers[i].classes);
  }

  network->netw = ni_network->linfo.in_param[0].sizes[1];
  network->neth = ni_network->linfo.in_param[0].sizes[2];
  network->net_out_w = ni_network->linfo.out_param[0].sizes[1];
  network->net_out_h = ni_network->linfo.out_param[0].sizes[2];

  return TRUE;

failed_out:
  ni_destroy_network (filter, network);
  return FALSE;
}

static gboolean
ni_ai_pre_config_input (GstNiQuadraAiPre * filter, gint dev_idx)
{
  GstNiQuadraBaseConvert *parent = GST_NI_QUADRA_BASE_CONVERT (filter);
  gint in_w = GST_VIDEO_INFO_WIDTH (&parent->in_info);
  gint in_h = GST_VIDEO_INFO_HEIGHT (&parent->in_info);
  gint out_w = GST_VIDEO_INFO_WIDTH (&parent->out_info);
  gint out_h = GST_VIDEO_INFO_HEIGHT (&parent->out_info);

  if (parent->initialized)
    return TRUE;

  if (!init_ai_context (filter, dev_idx)) {
    GST_ERROR_OBJECT (filter, "failed to initialize ai context\n");
    return FALSE;
  }

  if (!ni_create_network (filter, &filter->network)) {
    goto fail_out;
  }

  if (filter->channel_mode == 0) {
    if ((filter->network.netw != in_w && filter->network.neth != in_h)
        && filter->network.netw != NIALIGN (in_w, 128)) {
      GST_ERROR_OBJECT (filter, "Model not match input,"
          "model resolution=%dx%d, input resolution=%dx%d\n",
          filter->network.netw, filter->network.neth, in_w, in_h);
      goto fail_out;
    }

    if ((filter->network.net_out_w != out_w &&
            filter->network.net_out_h != out_h) &&
        filter->network.net_out_w != NIALIGN (out_w, 128)) {
      GST_ERROR_OBJECT (filter, "Model not match output,"
          "model resolution=%dx%d, output resolution=%dx%d\n",
          filter->network.net_out_w, filter->network.net_out_h, out_w, out_h);
      goto fail_out;
    }
  } else if (filter->channel_mode == 1) {
    if (filter->network.raw.input_num != 1) {
      GST_ERROR_OBJECT (filter, "Model not match input, the model 1 should "
          "have only one input layer\n");
      goto fail_out;
    } else if (filter->network.neth != 1) {
      // Netint Y channel only model's height on parser is allways 1.
      GST_ERROR_OBJECT (filter, "Model not match input for model 1, "
          "please make sure your model is for only Y channel\n");
      goto fail_out;
    }
  }

  parent->initialized = TRUE;
  return TRUE;

fail_out:
  cleanup_ai_context (filter);
  ni_destroy_network (filter, &filter->network);
  return FALSE;
}

static gboolean
gst_to_niframe_copy (ni_frame_t * dst, GstVideoFrame * frame, int nb_planes)
{
  int dst_stride[4], src_height[4], hpad[4], vpad[4];
  int i, j, h;
  uint8_t *src_line, *dst_line, YUVsample, *sample, *dest;
  uint16_t lastidx;
  bool tenBit;
  uint32_t src_size = 0;
  ni_pix_fmt_t niPixFmt =
      convertGstVideoFormatToNIPix (frame->info.finfo->format);

  switch (niPixFmt) {
    case NI_PIX_FMT_YUV420P:
      dst_stride[0] = NI_ALIGN (frame->info.width, 128);
      dst_stride[1] = NI_ALIGN ((frame->info.width / 2), 128);
      dst_stride[2] = dst_stride[1];
      dst_stride[3] = 0;
      hpad[0] =
          ni_max (dst_stride[0] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0), 0);
      hpad[1] =
          ni_max (dst_stride[1] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 1), 0);
      hpad[2] =
          ni_max (dst_stride[2] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 2), 0);
      hpad[3] = 0;

      src_height[0] = frame->info.height;
      src_height[1] = NI_ALIGN (frame->info.height, 2) / 2;
      src_height[2] = NI_ALIGN (frame->info.height, 2) / 2;
      src_height[3] = 0;

      vpad[0] = NI_ALIGN (src_height[0], 2) - src_height[0];
      vpad[1] = NI_ALIGN (src_height[1], 2) - src_height[1];
      vpad[2] = NI_ALIGN (src_height[2], 2) - src_height[2];
      vpad[3] = 0;

      tenBit = false;
      break;
    case NI_PIX_FMT_YUV420P10LE:
      dst_stride[0] = NI_ALIGN (frame->info.width * 2, 128);
      dst_stride[1] = NI_ALIGN (frame->info.width, 128);
      dst_stride[2] = dst_stride[1];
      dst_stride[3] = 0;
      hpad[0] =
          ni_max (dst_stride[0] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0), 0);
      hpad[1] =
          ni_max (dst_stride[1] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 1), 0);
      hpad[2] =
          ni_max (dst_stride[2] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 2), 0);
      hpad[3] = 0;

      src_height[0] = frame->info.height;
      src_height[1] = NI_ALIGN (frame->info.height, 2) / 2;
      src_height[2] = NI_ALIGN (frame->info.height, 2) / 2;
      src_height[3] = 0;

      vpad[0] = NI_ALIGN (src_height[0], 2) - src_height[0];
      vpad[1] = NI_ALIGN (src_height[1], 2) - src_height[1];
      vpad[2] = NI_ALIGN (src_height[2], 2) - src_height[2];
      vpad[3] = 0;

      tenBit = true;
      break;
    case NI_PIX_FMT_NV12:
      dst_stride[0] = NI_ALIGN (frame->info.width, 128);
      dst_stride[1] = dst_stride[0];
      dst_stride[2] = 0;
      dst_stride[3] = 0;
      hpad[0] =
          ni_max (dst_stride[0] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0), 0);
      hpad[1] =
          ni_max (dst_stride[1] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 1), 0);
      hpad[2] = 0;
      hpad[3] = 0;

      src_height[0] = frame->info.height;
      src_height[1] = NI_ALIGN (frame->info.height, 2) / 2;
      src_height[2] = 0;
      src_height[3] = 0;

      vpad[0] = NI_ALIGN (src_height[0], 2) - src_height[0];
      vpad[1] = NI_ALIGN (src_height[1], 2) - src_height[1];
      vpad[2] = 0;
      vpad[3] = 0;

      tenBit = false;
      break;
    case NI_PIX_FMT_NV16:
      dst_stride[0] = NI_ALIGN (frame->info.width, 64);
      dst_stride[1] = dst_stride[0];
      dst_stride[2] = 0;
      dst_stride[3] = 0;
      hpad[0] = 0;
      hpad[1] = 0;
      hpad[2] = 0;
      hpad[3] = 0;

      src_height[0] = frame->info.height;
      src_height[1] = frame->info.height;
      src_height[2] = 0;
      src_height[3] = 0;

      vpad[0] = 0;
      vpad[1] = 0;
      vpad[2] = 0;
      vpad[3] = 0;

      tenBit = false;
      break;
    case NI_PIX_FMT_P010LE:
      dst_stride[0] = NI_ALIGN (frame->info.width * 2, 128);
      dst_stride[1] = dst_stride[0];
      dst_stride[2] = 0;
      dst_stride[3] = 0;
      hpad[0] =
          ni_max (dst_stride[0] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0), 0);
      hpad[1] =
          ni_max (dst_stride[1] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 1), 0);
      hpad[2] = 0;
      hpad[3] = 0;

      src_height[0] = frame->info.height;
      src_height[1] = NI_ALIGN (frame->info.height, 2) / 2;
      src_height[2] = 0;
      src_height[3] = 0;

      vpad[0] = NI_ALIGN (src_height[0], 2) - src_height[0];
      vpad[1] = NI_ALIGN (src_height[1], 2) - src_height[1];
      vpad[2] = 0;
      vpad[3] = 0;

      tenBit = true;
      break;
    case NI_PIX_FMT_RGBA:
    case NI_PIX_FMT_BGRA:
    case NI_PIX_FMT_ABGR:
    case NI_PIX_FMT_ARGB:
    case NI_PIX_FMT_BGR0:
      dst_stride[0] = NI_ALIGN (frame->info.width, 16) * 4;
      dst_stride[1] = 0;
      dst_stride[2] = 0;
      dst_stride[3] = 0;
      hpad[0] =
          ni_max (dst_stride[0] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0), 0);
      hpad[1] = 0;
      hpad[2] = 0;
      hpad[3] = 0;

      src_height[0] = frame->info.height;
      src_height[1] = 0;
      src_height[2] = 0;
      src_height[3] = 0;

      vpad[0] = 0;
      vpad[1] = 0;
      vpad[2] = 0;
      vpad[3] = 0;

      tenBit = false;
      break;
    case NI_PIX_FMT_BGRP:
      dst_stride[0] = NI_ALIGN (frame->info.width, 16) * 4;
      dst_stride[1] = 0;
      dst_stride[2] = 0;
      dst_stride[3] = 0;
      hpad[0] =
          ni_max (dst_stride[0] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0), 0);
      hpad[1] =
          ni_max (dst_stride[1] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 1), 0);
      hpad[2] =
          ni_max (dst_stride[2] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 2), 0);
      hpad[3] = 0;

      src_height[0] = frame->info.height;
      src_height[1] = frame->info.height;
      src_height[2] = frame->info.height;
      src_height[3] = 0;

      vpad[0] = 0;
      vpad[1] = 0;
      vpad[2] = 0;
      vpad[3] = 0;

      tenBit = false;
      break;
    case NI_PIX_FMT_YUYV422:
    case NI_PIX_FMT_UYVY422:
      dst_stride[0] = NI_ALIGN (frame->info.width, 16) * 2;
      dst_stride[1] = 0;
      dst_stride[2] = 0;
      dst_stride[3] = 0;
      hpad[0] =
          ni_max (dst_stride[0] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0), 0);
      hpad[1] = 0;
      hpad[2] = 0;
      hpad[3] = 0;

      src_height[0] = frame->info.height;
      src_height[1] = 0;
      src_height[2] = 0;
      src_height[3] = 0;

      vpad[0] = 0;
      vpad[1] = 0;
      vpad[2] = 0;
      vpad[3] = 0;

      tenBit = false;
      break;
    default:
      GST_ERROR ("Pixel format %d not supported\n", niPixFmt);
      return FALSE;
  }

  for (i = 0; i < nb_planes; i++) {
    src_size += (dst_stride[i] + vpad[i]) * src_height[i];
  }
  if (dst->buffer_size < src_size) {
    GST_ERROR ("gst_to_niframe_copy failed, please check the input/output "
        "frame reslotion and the module reslotion\n");
    return FALSE;
  }
  dst_line = dst->p_buffer;
  for (i = 0; i < nb_planes; i++) {
    src_line = GST_VIDEO_FRAME_PLANE_DATA (frame, i);
    for (h = 0; h < src_height[i]; h++) {
      memcpy (dst_line, src_line, ni_min (GST_VIDEO_FRAME_PLANE_STRIDE (frame,
                  i), dst_stride[i]));

      if (hpad[i]) {
        lastidx = GST_VIDEO_FRAME_PLANE_STRIDE (frame, i);

        if (tenBit) {
          sample = &src_line[lastidx - 2];
          dest = &dst_line[lastidx];

          /* two bytes per sample */
          for (j = 0; j < hpad[i] / 2; j++) {
            memcpy (dest, sample, 2);
            dest += 2;
          }
        } else {
          YUVsample = dst_line[lastidx - 1];
          memset (&dst_line[lastidx], YUVsample, hpad[i]);
        }
      }

      src_line += GST_VIDEO_FRAME_PLANE_STRIDE (frame, i);
      dst_line += dst_stride[i];
    }

    /* Extend the height by cloning the last line */
    src_line = dst_line - dst_stride[i];
    for (h = 0; h < vpad[i]; h++) {
      memcpy (dst_line, src_line, dst_stride[i]);
      dst_line += dst_stride[i];
    }
  }

  return TRUE;
}

static gboolean
ni_to_gstframe_copy (GstVideoFrame * dst, const ni_packet_t * src,
    int nb_planes)
{
  int src_linesize[4], src_height[4];
  int i, h;
  uint8_t *src_line, *dst_line;
  uint32_t dst_size = 0;
  ni_pix_fmt_t niPixFmt =
      convertGstVideoFormatToNIPix (dst->info.finfo->format);
  switch (niPixFmt) {
    case NI_PIX_FMT_YUV420P:
      src_linesize[0] = NI_ALIGN (dst->info.width, 128);
      src_linesize[1] = NI_ALIGN (dst->info.width / 2, 128);
      src_linesize[2] = src_linesize[1];
      src_linesize[3] = 0;

      src_height[0] = dst->info.height;
      src_height[1] = NI_ALIGN (dst->info.height, 2) / 2;
      src_height[2] = src_height[1];
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_YUV420P10LE:
      src_linesize[0] = NI_ALIGN (dst->info.width * 2, 128);
      src_linesize[1] = NI_ALIGN (dst->info.width, 128);
      src_linesize[2] = src_linesize[1];
      src_linesize[3] = 0;

      src_height[0] = dst->info.height;
      src_height[1] = NI_ALIGN (dst->info.height, 2) / 2;
      src_height[2] = src_height[1];
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_NV12:
      src_linesize[0] = NI_ALIGN (dst->info.width, 128);
      src_linesize[1] = NI_ALIGN (dst->info.width, 128);
      src_linesize[2] = 0;
      src_linesize[3] = 0;

      src_height[0] = dst->info.height;
      src_height[1] = NI_ALIGN (dst->info.height, 2) / 2;
      src_height[2] = 0;
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_NV16:
      src_linesize[0] = NI_ALIGN (dst->info.width, 64);
      src_linesize[1] = NI_ALIGN (dst->info.width, 64);
      src_linesize[2] = 0;
      src_linesize[3] = 0;

      src_height[0] = dst->info.height;
      src_height[1] = dst->info.height;
      src_height[2] = 0;
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_YUYV422:
    case NI_PIX_FMT_UYVY422:
      src_linesize[0] = NI_ALIGN (dst->info.width, 16) * 2;
      src_linesize[1] = 0;
      src_linesize[2] = 0;
      src_linesize[3] = 0;

      src_height[0] = dst->info.height;
      src_height[1] = 0;
      src_height[2] = 0;
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_P010LE:
      src_linesize[0] = NI_ALIGN (dst->info.width * 2, 128);
      src_linesize[1] = NI_ALIGN (dst->info.width * 2, 128);
      src_linesize[2] = 0;
      src_linesize[3] = 0;

      src_height[0] = dst->info.height;
      src_height[1] = NI_ALIGN (dst->info.height, 2) / 2;
      src_height[2] = 0;
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_RGBA:
    case NI_PIX_FMT_BGRA:
    case NI_PIX_FMT_ABGR:
    case NI_PIX_FMT_ARGB:
    case NI_PIX_FMT_BGR0:
      src_linesize[0] = NI_ALIGN (dst->info.width, 16) * 4;
      src_linesize[1] = 0;
      src_linesize[2] = 0;
      src_linesize[3] = 0;

      src_height[0] = dst->info.height;
      src_height[1] = 0;
      src_height[2] = 0;
      src_height[3] = 0;
      break;
    case NI_PIX_FMT_BGRP:
      src_linesize[0] = NI_ALIGN (dst->info.width, 32);
      src_linesize[1] = NI_ALIGN (dst->info.width, 32);
      src_linesize[2] = NI_ALIGN (dst->info.width, 32);
      src_linesize[3] = 0;

      src_height[0] = dst->info.height;
      src_height[1] = dst->info.height;
      src_height[2] = dst->info.height;
      src_height[3] = 0;
      break;
    default:
      GST_ERROR ("Unsupported pixel format %d\n", niPixFmt);
      return FALSE;
  }
  int line_sizes[4];
  if (!gst_image_fill_linesizes (line_sizes, niPixFmt, dst->info.width, 1))
    return FALSE;

  for (i = 0; i < nb_planes; i++) {
    dst_size += ni_min (src_linesize[i], line_sizes[i]) * src_height[i];
  }
  if (dst_size > src->data_len) {
    GST_ERROR ("ni_to_gstframe_copy failed, please check the input/output "
        "frame reslotion and the module reslotion\n");
    return FALSE;
  }
  src_line = src->p_data;
  for (i = 0; i < nb_planes; i++) {
    dst_line = dst->data[i];

    for (h = 0; h < src_height[i]; h++) {
      memcpy (dst_line, src_line, ni_min (src_linesize[i], line_sizes[i]));
      dst_line += ni_min (src_linesize[i], line_sizes[i]);
      src_line += src_linesize[i];
    }
  }

  return TRUE;
}

static gboolean
gst_niquadraaipre_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstNiQuadraAiPre *self = GST_NIQUADRAAIPRE (trans);
  GstNiQuadraBaseConvert *parent = GST_NI_QUADRA_BASE_CONVERT (trans);
  GstCapsFeatures *features;

  if (parent->initialized) {
    cleanup_ai_context (self);
    ni_destroy_network (self, &self->network);
    parent->initialized = FALSE;
  }

  if (!GST_BASE_TRANSFORM_CLASS (parent_class)->set_caps (trans, incaps,
          outcaps)) {
    return FALSE;
  }

  features = gst_caps_get_features (incaps, 0);
  if (features && gst_caps_features_contains (features,
          GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY)) {
    self->hw_mode = TRUE;
  } else {
    self->hw_mode = FALSE;
  }

  return TRUE;
}

static GstFlowReturn
gst_niquadraaipre_transform (GstBaseTransform * trans, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstNiQuadraAiPre *filter = GST_NIQUADRAAIPRE (trans);
  GstNiQuadraBaseConvert *parent = GST_NI_QUADRA_BASE_CONVERT (trans);
  GstFlowReturn flow_ret = GST_FLOW_OK;
  int retval = 0;
  GstAllocator *alloc = NULL;
  GstBuffer *tmpbuf = NULL;
  GstMemory *out_mem = NULL;
  GstMemory *in_mem = NULL;
  niFrameSurface1_t *in_surface = NULL;
  niFrameSurface1_t *out_surface = NULL;
  gint dev_idx = -1;
  ni_session_data_io_t *p_session_data = NULL;
  int gc620_pixfmt;
  ni_pix_fmt_t niPixFmt;
  AiContext *ai_ctx;
  GstClock *clock;
  GstClockTime time_now, time_end;
  ni_ai_pre_network_t *network;
  int nb_planes;
  GstVideoFrame iframe = { 0 };
  GstVideoFrame oframe = { 0 };
  GstVideoFormat format = GST_VIDEO_INFO_FORMAT (&parent->in_info);
  gint out_w = GST_VIDEO_INFO_WIDTH (&parent->out_info);
  gint out_h = GST_VIDEO_INFO_HEIGHT (&parent->out_info);

  clock = gst_system_clock_obtain ();

  in_mem = gst_buffer_peek_memory (inbuf, 0);
  if (filter->hw_mode == true) {
    in_surface = gst_surface_from_ni_hw_memory (in_mem);
    dev_idx = gst_deviceid_from_ni_hw_memory (in_mem);
    if (in_surface == NULL) {
      GST_ERROR_OBJECT (filter, "Impossible to convert between the formats "
          "supported by the filter\n");
      flow_ret = GST_FLOW_ERROR;
      goto failed_out;
    }
  }

  if (!parent->initialized) {
    GstCaps *out_caps = gst_video_info_to_caps (&parent->out_info);
    if (filter->hw_mode) {
      gst_caps_set_features_simple (out_caps,
          gst_caps_features_from_string
          (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY));
    }
    gst_niquadraaipre_query_downstream_allocation (filter, trans, out_caps);
    gst_caps_unref (out_caps);

    if (!ni_ai_pre_config_input (filter, dev_idx)) {
      GST_ERROR_OBJECT (filter, "Failed to config input\n");
      flow_ret = GST_FLOW_ERROR;
      goto failed_out;
    }
  }

  ai_ctx = filter->ai_ctx;
  if (filter->hw_mode) {
    p_session_data = &ai_ctx->api_dst_frame;

    niPixFmt = convertGstVideoFormatToNIPix (format);
    gc620_pixfmt = convertNIPixToGC620Format (niPixFmt);
    niFrameSurface1_t dst_surface = { 0 };

    time_now = gst_clock_get_time (clock);
    do {
      if (filter->channel_mode) {
        retval = ni_device_alloc_dst_frame (&(ai_ctx->api_ctx), &dst_surface,
            NI_DEVICE_TYPE_AI);
      } else {
        retval = ni_device_alloc_frame (&ai_ctx->api_ctx,
            NIALIGN (out_w, 2), NIALIGN (out_h, 2),
            gc620_pixfmt, NI_AI_FLAG_IO, 0, 0, 0, 0, 0, -1, NI_DEVICE_TYPE_AI);
      }

      if (retval < 0) {
        GST_ERROR_OBJECT (filter, "failed to alloc hw output frame\n");
        flow_ret = GST_FLOW_ERROR;
        goto failed_out;
      }

      time_end = gst_clock_get_time (clock);
      if (GST_CLOCK_DIFF (time_now,
              time_end) > filter->ai_timeout * 1000 * 1000 * 1000) {
        GST_ERROR_OBJECT (filter, "alloc hw output timeout %" G_GINT64_FORMAT
            " > ai_timeout %u\n", GST_CLOCK_DIFF (time_now, time_end),
            filter->ai_timeout * 1000 * 1000 * 1000);
        flow_ret = GST_FLOW_ERROR;
        goto failed_out;
      }
    } while (retval != NI_RETCODE_SUCCESS);

    if (filter->channel_mode) {
      // copy input hw frame to dst hw frame
      ni_frameclone_desc_t frame_clone_desc = { 0 };
      frame_clone_desc.ui16DstIdx = dst_surface.ui16FrameIdx;
      frame_clone_desc.ui16SrcIdx = in_surface->ui16FrameIdx;
      if (format == GST_VIDEO_FORMAT_I420) {
        frame_clone_desc.ui32Offset = NI_VPU_ALIGN128 (out_w) *
            NI_VPU_CEIL (out_h, 2);
        frame_clone_desc.ui32Size = NI_VPU_ALIGN128 (out_w / 2) *
            NI_VPU_CEIL (out_h, 2);
        retval = ni_device_clone_hwframe (&ai_ctx->api_ctx, &frame_clone_desc);
        if (retval != NI_RETCODE_SUCCESS) {
          GST_ERROR_OBJECT (filter, "failed to clone hw input frame\n");
          flow_ret = GST_FLOW_ERROR;
          goto failed_out;
        }
      } else {
        GST_ERROR_OBJECT (filter, "Error: support yuv420p only, "
            "current fmt %d\n", format);
        flow_ret = GST_FLOW_ERROR;
        goto failed_out;
      }
    }

    /* set input buffer */
    retval = ni_device_alloc_frame (&ai_ctx->api_ctx, 0, 0, 0, 0, 0, 0, 0, 0,
        in_surface->ui32nodeAddress, in_surface->ui16FrameIdx,
        NI_DEVICE_TYPE_AI);
    if (retval != NI_RETCODE_SUCCESS) {
      GST_ERROR_OBJECT (filter, "failed to alloc hw input frame\n");
      flow_ret = GST_FLOW_ERROR;
      goto failed_out;
    }

    /* Set the new frame index */
    time_now = gst_clock_get_time (clock);
    do {
      retval = ni_device_session_read_hwdesc (&ai_ctx->api_ctx,
          &ai_ctx->api_dst_frame, NI_DEVICE_TYPE_AI);
      if (retval < 0) {
        GST_ERROR_OBJECT (filter, "failed to read hwdesc, retval=%d\n", retval);
        flow_ret = GST_FLOW_ERROR;
        goto failed_out;
      }

      time_end = gst_clock_get_time (clock);
      if (GST_CLOCK_DIFF (time_now,
              time_end) > filter->ai_timeout * 1000 * 1000 * 1000) {
        GST_ERROR_OBJECT (filter, "alloc hw output timeout %" G_GINT64_FORMAT
            " > ai_timeout %u\n", GST_CLOCK_DIFF (time_now, time_end),
            filter->ai_timeout * 1000 * 1000 * 1000);
        flow_ret = GST_FLOW_ERROR;
        goto failed_out;
      }
    } while (retval != NI_RETCODE_SUCCESS);

    out_surface = (niFrameSurface1_t *) p_session_data->data.frame.p_data[3];
    out_surface->dma_buf_fd = 0;
    out_surface->ui32nodeAddress = 0;
    out_surface->ui16width = out_w;
    out_surface->ui16height = out_h;

    ni_set_bit_depth_and_encoding_type (&out_surface->bit_depth,
        &out_surface->encoding_type, niPixFmt);

    tmpbuf = gst_buffer_make_writable (outbuf);
    alloc = gst_allocator_find (GST_NIQUADRA_MEMORY_TYPE_NAME);
    out_mem = gst_niquadra_allocator_alloc (alloc, &ai_ctx->api_ctx,
        out_surface, ai_ctx->api_ctx.hw_id, &parent->out_info);
    gst_buffer_append_memory (tmpbuf, out_mem);
    gst_object_unref (alloc);

    gst_buffer_copy_into (tmpbuf, inbuf,
        GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_META, 0, -1);
  } else {
    network = &filter->network;
    gst_video_frame_map (&iframe, &parent->in_info, inbuf, GST_MAP_READ);
    time_now = gst_clock_get_time (clock);
    retval = ni_ai_frame_buffer_alloc (&ai_ctx->api_src_frame.data.frame,
        &network->raw);
    if (retval != NI_RETCODE_SUCCESS) {
      GST_ERROR_OBJECT (filter, "cannot allocate ai frame\n");
      flow_ret = GST_FLOW_ERROR;
      goto unmap_inframe;
    }
    nb_planes = iframe.info.finfo->n_planes;
    if (filter->channel_mode) {
      if (format != GST_VIDEO_FORMAT_I420) {
        GST_ERROR_OBJECT (filter, "Error: support I420 only, current fmt %d\n",
            format);
        flow_ret = GST_FLOW_ERROR;
        goto unmap_inframe;
      }
      nb_planes = 1;            // only copy Y data
    }
    if (!gst_to_niframe_copy (&ai_ctx->api_src_frame.data.frame, &iframe,
            nb_planes)) {
      GST_ERROR_OBJECT (filter, "ai_pre cannot copy frame\n");
      flow_ret = GST_FLOW_ERROR;
      goto unmap_inframe;
    }

    /* write frame */
    do {
      retval = ni_device_session_write (&ai_ctx->api_ctx,
          &ai_ctx->api_src_frame, NI_DEVICE_TYPE_AI);
      if (retval < 0) {
        GST_ERROR_OBJECT (filter, "failed to write ai session : "
            "retval %d\n", retval);
        flow_ret = GST_FLOW_ERROR;
        goto unmap_inframe;
      }

      time_end = gst_clock_get_time (clock);
      if (GST_CLOCK_DIFF (time_now,
              time_end) > filter->ai_timeout * 1000 * 1000 * 1000) {
        GST_ERROR_OBJECT (filter, "write sw frame to AI timeout "
            "%" G_GINT64_FORMAT " > ai_timeout %u\n",
            GST_CLOCK_DIFF (time_now, time_end),
            filter->ai_timeout * 1000 * 1000 * 1000);
        flow_ret = GST_FLOW_ERROR;
        goto unmap_inframe;
      }
    } while (retval == 0);

    retval = ni_ai_packet_buffer_alloc (&ai_ctx->api_dst_frame.data.packet,
        &network->raw);
    if (retval != NI_RETCODE_SUCCESS) {
      GST_ERROR_OBJECT (filter, "failed to allocate ni packet\n");
      flow_ret = GST_FLOW_ERROR;
      goto unmap_inframe;
    }

    time_now = gst_clock_get_time (clock);
    do {
      retval = ni_device_session_read (&ai_ctx->api_ctx,
          &ai_ctx->api_dst_frame, NI_DEVICE_TYPE_AI);
      if (retval < 0) {
        GST_ERROR_OBJECT (filter, "read AI data retval %d\n", retval);
        flow_ret = GST_FLOW_ERROR;
        goto unmap_inframe;
      } else if (retval > 0) {
        time_end = gst_clock_get_time (clock);
        if (GST_CLOCK_DIFF (time_now,
                time_end) > filter->ai_timeout * 1000 * 1000 * 1000) {
          GST_ERROR_OBJECT (filter, "read sw frame from AI timeout "
              "%" G_GINT64_FORMAT " > ai_timeout %u\n",
              GST_CLOCK_DIFF (time_now, time_end),
              filter->ai_timeout * 1000 * 1000 * 1000);
          flow_ret = GST_FLOW_ERROR;
          goto unmap_inframe;
        }
      }
    } while (retval == 0);

    tmpbuf = gst_buffer_make_writable (outbuf);
    {
      GstMemory *mem = gst_allocator_alloc (NULL, parent->out_info.size, NULL);
      if (mem == NULL) {
        flow_ret = GST_FLOW_ERROR;
        goto unmap_inframe;
      }
      gst_buffer_append_memory (tmpbuf, mem);
    }

    gst_buffer_memset (tmpbuf, 0, 0, parent->out_info.size);
    gst_video_frame_map (&oframe, &parent->out_info, tmpbuf,
        GST_MAP_READ | GST_MAP_WRITE);
    nb_planes = oframe.info.finfo->n_planes;

    if (filter->channel_mode) {
      if (format != GST_VIDEO_FORMAT_I420) {
        GST_ERROR_OBJECT (filter, "Error: support I420 only, current fmt %d\n",
            format);
        flow_ret = GST_FLOW_ERROR;
        goto unmap_outframe;
      }
      nb_planes = 1;            // only copy Y data
    }
    if (!ni_to_gstframe_copy (&oframe, &ai_ctx->api_dst_frame.data.packet,
            nb_planes)) {
      GST_ERROR_OBJECT (filter, "ai_pre cannot copy ai frame to gstframe\n");
      flow_ret = GST_FLOW_ERROR;
      goto unmap_outframe;
    }

    if (filter->channel_mode) {
      // copy U/V data from the input sw frame
      memcpy (oframe.data[1], iframe.data[1],
          iframe.info.height * GST_VIDEO_FRAME_PLANE_STRIDE (&iframe, 1) / 2);
      memcpy (oframe.data[2], iframe.data[2],
          iframe.info.height * GST_VIDEO_FRAME_PLANE_STRIDE (&iframe, 2) / 2);
    }
    gst_video_frame_unmap (&iframe);
    gst_video_frame_unmap (&oframe);
    if (!gst_buffer_copy_into (tmpbuf, inbuf,
            GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_META, 0, -1)) {
      flow_ret = GST_FLOW_ERROR;
    }
  }

  gst_object_unref (clock);
  return flow_ret;

unmap_outframe:
  gst_video_frame_unmap (&oframe);
unmap_inframe:
  gst_video_frame_unmap (&iframe);
failed_out:
  gst_object_unref (clock);
  return flow_ret;
}

static void
gst_niquadraaipre_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiQuadraAiPre *self;

  g_return_if_fail (GST_IS_NIQUADRAAIPRE (object));
  self = GST_NIQUADRAAIPRE (object);

  switch (prop_id) {
    case PROP_NB:
      g_free (self->nb_file);
      self->nb_file = g_strdup (g_value_get_string (value));
      break;
    case PROP_DEVID:
      self->devid = g_value_get_int (value);
      break;
    case PROP_MODE:
      self->channel_mode = g_value_get_boolean (value);
      break;
    case PROP_TIMEOUT:
      self->ai_timeout = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadraaipre_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiQuadraAiPre *self;

  g_return_if_fail (GST_IS_NIQUADRAAIPRE (object));
  self = GST_NIQUADRAAIPRE (object);

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
    case PROP_MODE:
      GST_OBJECT_LOCK (self);
      g_value_set_boolean (value, self->channel_mode);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_TIMEOUT:
      GST_OBJECT_LOCK (self);
      g_value_set_uint (value, self->ai_timeout);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadraaipre_class_init (GstNiQuadraAiPreClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class->set_property = gst_niquadraaipre_set_property;
  gobject_class->get_property = gst_niquadraaipre_get_property;
  gobject_class->finalize = gst_niquadraaipre_finalize;

  g_object_class_install_property (gobject_class, PROP_NB,
      g_param_spec_string ("nb", "NB",
          "File path of AI module network binary",
          NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_DEVID,
      g_param_spec_int ("devid", "DevId",
          "Specific the device id of quadra hardware", -1, G_MAXINT,
          0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_MODE,
      g_param_spec_boolean ("mode", "Mode",
          "Specify the processing channel of the network, 0: YUV channels, 1: Y channel only",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_NAME |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_TIMEOUT,
      g_param_spec_uint ("timeout", "TIMEOUT",
          "Specify a custom timeout in seconds. Defult is 3.",
          NI_MIN_KEEP_ALIVE_TIMEOUT, NI_MAX_KEEP_ALIVE_TIMEOUT,
          NI_DEFAULT_KEEP_ALIVE_TIMEOUT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (element_class, &src_factory);
  gst_element_class_add_static_pad_template (element_class, &sink_factory);

  gst_element_class_set_static_metadata (element_class,
      "NETINT Quadra AIPRE filter", "Filter/Effect/Video/NIAiPre",
      "AiPre Netint Quadra", "Minglong Zhang<Minglong.Zhang@netint.cn>");

  trans_class->fixate_caps = gst_niquadraaipre_fixate_caps;
  trans_class->set_caps = gst_niquadraaipre_set_caps;
  trans_class->transform = gst_niquadraaipre_transform;
}

static gboolean
niquadraaipre_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadraaipre_debug, "niquadraaipre", 0,
      "niquadraaipre");

  return gst_element_register (plugin, "niquadraaipre", GST_RANK_NONE,
      GST_TYPE_NIQUADRAAIPRE);
}

static void
gst_niquadraaipre_init (GstNiQuadraAiPre * filter)
{
  filter->ai_timeout = NI_DEFAULT_KEEP_ALIVE_TIMEOUT;
  filter->nb_file = NULL;
  filter->devid = 0;
  filter->channel_mode = 0;

  filter->downstream_card = -1;
  filter->extra_frames = 0;
  filter->hw_mode = FALSE;
}
