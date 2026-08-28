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
 *  \file   gstniquadradec.c
 *
 *  \brief  Implement of NetInt Quadra common decoder.
 ******************************************************************************/

#include "config.h"

#include <stdlib.h>
#include <ni_util.h>
#include <ni_av_codec.h>
#include <math.h>

#include "gstniquadradec.h"
#include "gstniquadramemory.h"
#include "gstniquadrautils.h"
#include "gstnieval.h"
#include "gstnimetadata.h"

GST_DEBUG_CATEGORY_STATIC (gst_niquadradec_debug);
#define GST_CAT_DEFAULT gst_niquadradec_debug

#define GST_NI_CAPS_MAKE(format) \
  GST_VIDEO_CAPS_MAKE (format) ", " \
  "interlace-mode = (string) progressive"


#define GST_NI_CAPS_MAKE_WITH_DMABUF_FEATURE(dmaformat) ""

#define GST_NI_CAPS_STR(format,dmaformat) \
  GST_NI_CAPS_MAKE (format) "; " \
  GST_NI_CAPS_MAKE_WITH_DMABUF_FEATURE (dmaformat)

#define REQUIRED_POOL_MAX_BUFFERS       32
#define DEFAULT_STRIDE_ALIGN            31
#define DEFAULT_ALLOC_PARAM             { 0, DEFAULT_STRIDE_ALIGN, 0, 0, }

#define COMMON_FORMAT  "{ I420, I420_10, NV12, P010_10LE }"

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_NI_CAPS_STR (COMMON_FORMAT, COMMON_FORMAT))
    );

#define gst_niquadradec_parent_class parent_class
G_DEFINE_TYPE (GstNiquadraDec, gst_niquadradec, GST_TYPE_VIDEO_DECODER);

static inline void
gst_xcoder_strncpy (char *dst, const char *src, int max)
{
  if (dst && src && max) {
    snprintf (dst, max, "%s", src);
  }
}

static void
gst_niquadradec_close_decoder (GstNiquadraDec * nidec)
{
  if (!nidec->opened) {
    return;
  }

  int ret = 0;
  ni_session_context_t *p_ctx =
      gst_niquadra_context_get_session_context (nidec->context);
  nidec->opened = FALSE;

  GST_DEBUG_OBJECT (nidec, "close_decoder\n");

  if (p_ctx) {
    ret = ni_device_session_close (p_ctx, nidec->eos, NI_DEVICE_TYPE_DECODER);
    if (ret != NI_RETCODE_SUCCESS) {
      GST_ERROR_OBJECT (nidec, "ni session close error[%d]", ret);
    }
  }
  gst_object_unref (nidec->context);
  nidec->context = NULL;
}

static void
gst_niquadradec_set_context (GstElement * element, GstContext * context)
{
  GST_ELEMENT_CLASS (parent_class)->set_context (element, context);
}

static gboolean
gst_niquadradec_start (GstVideoDecoder * decoder)
{
  GstNiquadraDec *thiz = GST_NIQUADRADEC (decoder);
  GstNiquadraDecClass *decoder_class = GST_NIQUADRADEC_GET_CLASS (decoder);

  GST_OBJECT_LOCK (decoder);
  gst_niquadradec_close_decoder (thiz);
  thiz->context = NULL;

  if (decoder_class->prepare)
    decoder_class->prepare (thiz);
  GST_OBJECT_UNLOCK (decoder);
  return TRUE;
}

static gboolean
gst_niquadradec_close (GstVideoDecoder * decoder)
{
  GstNiquadraDec *nienc = GST_NIQUADRADEC (decoder);
  gst_niquadradec_close_decoder (nienc);

  return TRUE;
}

static NiPluginError
parse_decoder_param (ni_decoder_input_params_t * pdec_param)
{
  int i, ret = 0;
  double res;
  double var_values[VAR_VARS_NB];

  if (pdec_param == NULL) {
    return NI_PLUGIN_EINVAL;
  }

  for (i = 0; i < NI_MAX_NUM_OF_DECODER_OUTPUTS; i++) {
    /*Set output width and height */
    var_values[VAR_IN_W] = var_values[VAR_IW] = pdec_param->crop_whxy[i][0];
    var_values[VAR_IN_H] = var_values[VAR_IH] = pdec_param->crop_whxy[i][1];
    var_values[VAR_OUT_W] = var_values[VAR_OW] = pdec_param->crop_whxy[i][0];
    var_values[VAR_OUT_H] = var_values[VAR_OH] = pdec_param->crop_whxy[i][1];
    if (pdec_param->cr_expr[i][0][0] && pdec_param->cr_expr[i][1][0]) {
      if (ni_expr_parse_and_eval (&res, pdec_param->cr_expr[i][0], var_names,
              var_values, NULL, NULL, NULL, NULL, NULL, 0, NULL) < 0) {
        return NI_PLUGIN_EINVAL;
      }
      var_values[VAR_OUT_W] = var_values[VAR_OW] = (double) floor (res);
      if (ni_expr_parse_and_eval (&res, pdec_param->cr_expr[i][1], var_names,
              var_values, NULL, NULL, NULL, NULL, NULL, 0, NULL) < 0) {
        return NI_PLUGIN_EINVAL;
      }
      var_values[VAR_OUT_H] = var_values[VAR_OH] = (double) floor (res);
      /* evaluate again ow as it may depend on oh */
      ret = ni_expr_parse_and_eval (&res, pdec_param->cr_expr[i][0], var_names,
          var_values, NULL, NULL, NULL, NULL, NULL, 0, NULL);
      if (ret < 0) {
        return NI_PLUGIN_EINVAL;
      }
      var_values[VAR_OUT_W] = var_values[VAR_OW] = (double) floor (res);
      pdec_param->crop_whxy[i][0] = (int) var_values[VAR_OUT_W];
      pdec_param->crop_whxy[i][1] = (int) var_values[VAR_OUT_H];
    }
    /*Set output crop offset X,Y */
    if (pdec_param->cr_expr[i][2][0]) {
      ret = ni_expr_parse_and_eval (&res, pdec_param->cr_expr[i][2], var_names,
          var_values, NULL, NULL, NULL, NULL, NULL, 0, NULL);
      if (ret < 0) {
        return NI_PLUGIN_EINVAL;
      }
      var_values[VAR_X] = res;
      pdec_param->crop_whxy[i][2] = floor (var_values[VAR_X]);
    }
    if (pdec_param->cr_expr[i][3][0]) {
      ret = ni_expr_parse_and_eval (&res, pdec_param->cr_expr[i][3], var_names,
          var_values, NULL, NULL, NULL, NULL, NULL, 0, NULL);
      if (ret < 0) {
        return NI_PLUGIN_EINVAL;
      }
      var_values[VAR_Y] = res;
      pdec_param->crop_whxy[i][3] = floor (var_values[VAR_Y]);
    }
    /*Set output Scale */
    /*Reset OW and OH to next lower even number */
    var_values[VAR_OUT_W] = var_values[VAR_OW] =
        (double) (pdec_param->crop_whxy[i][0] -
        (pdec_param->crop_whxy[i][0] % 2));
    var_values[VAR_OUT_H] = var_values[VAR_OH] =
        (double) (pdec_param->crop_whxy[i][1] -
        (pdec_param->crop_whxy[i][1] % 2));
    if (pdec_param->sc_expr[i][0][0] && pdec_param->sc_expr[i][1][0]) {
      if (ni_expr_parse_and_eval (&res, pdec_param->sc_expr[i][0], var_names,
              var_values, NULL, NULL, NULL, NULL, NULL, 0, NULL) < 0) {
        return NI_PLUGIN_EINVAL;
      }
      pdec_param->scale_wh[i][0] = ceil (res);
      ret = ni_expr_parse_and_eval (&res, pdec_param->sc_expr[i][1], var_names,
          var_values, NULL, NULL, NULL, NULL, NULL, 0, NULL);
      if (ret < 0) {
        return NI_PLUGIN_EINVAL;
      }
      pdec_param->scale_wh[i][1] = ceil (res);
    }
  }
  return NI_PLUGIN_OK;
}

static NiPluginError
parse_symbolic_decoder_param (GstNiquadraDec * nidec)
{
  ni_xcoder_params_t *p_param =
      gst_niquadra_context_get_xcoder_param (nidec->context);
  ni_decoder_input_params_t *pdec_param = &p_param->dec_input_params;
  return parse_decoder_param (pdec_param);
}

static NiPluginError
gst_niquadradec_xcoder_setup_decoder (GstNiquadraDec * nidec)
{
  ni_retcode_t ret = NI_RETCODE_SUCCESS;
  GstNiquadraDecClass *klass = GST_NIQUADRADEC_GET_CLASS (nidec);
  if (nidec->opened) {
    gst_niquadradec_close_decoder (nidec);
  }
  if (!nidec->context) {
    nidec->context = gst_niquadra_context_new ();
  }
  ni_xcoder_params_t *p_param =
      gst_niquadra_context_get_xcoder_param (nidec->context);
  ni_session_context_t *p_session =
      gst_niquadra_context_get_session_context (nidec->context);
  ni_log_level_t xcoder_log_level = NI_LOG_NONE;
  switch (gst_debug_get_default_threshold ()) {
    case GST_LEVEL_INFO:
      xcoder_log_level = NI_LOG_INFO;
      break;
    case GST_LEVEL_DEBUG:
      xcoder_log_level = NI_LOG_DEBUG;
      break;
    case GST_LEVEL_ERROR:
      xcoder_log_level = NI_LOG_ERROR;
      break;
    case GST_LEVEL_TRACE:
      xcoder_log_level = NI_LOG_TRACE;
      break;
    default:
      xcoder_log_level = NI_LOG_INFO;
      break;
  }
  ni_log_set_level (xcoder_log_level);

  GST_DEBUG_OBJECT (nidec, "XCoder decode init,level=%d\n", xcoder_log_level);

  // Set decoder format such as h264/h265/vp9/jpeg
  klass->configure (nidec);

  GST_DEBUG_OBJECT (nidec, "width: %d height: %d fps: %d/%d, pix_fmt: %d\n",
      nidec->input_state->info.width, nidec->input_state->info.height,
      nidec->input_state->info.fps_n, nidec->input_state->info.fps_d,
      nidec->input_state->info.finfo->format);

  nidec->out_width = nidec->width = nidec->input_state->info.width;
  nidec->out_height = nidec->height = nidec->input_state->info.height;
  nidec->pix_fmt = nidec->input_state->info.finfo->format;

  if (0 == nidec->input_state->info.width
      || 0 == nidec->input_state->info.height) {
    GST_ERROR_OBJECT (nidec, "Error probing input stream\n");
    return NI_PLUGIN_FAILURE;
  }

  GST_DEBUG_OBJECT (nidec, "field_order = %d\n",
      nidec->input_state->info.interlace_mode);
  if (nidec->input_state->info.interlace_mode >
      GST_VIDEO_INTERLACE_MODE_PROGRESSIVE) {
    GST_ERROR_OBJECT (nidec, "interlaced video not supported!\n");
    return NI_PLUGIN_FAILURE;
  }

  nidec->offset = 0LL;

  nidec->draining = 0;

  GstStructure *str = gst_caps_get_structure (nidec->input_state->caps, 0);
  const GValue *depth_v, *profile_v;
  const gchar *profile = NULL;
  guint depth = 8;
  if ((profile_v = gst_structure_get_value (str, "profile"))) {
    profile = g_value_get_string (profile_v);
  }
  if ((depth_v = gst_structure_get_value (str, "bit-depth-luma"))) {
    depth = g_value_get_uint (depth_v);
  }
  if (depth == 10 && g_strcmp0 (profile, "main-10") == 0) {
    p_session->bit_depth_factor = 2;
    p_session->src_bit_depth = 10;
  } else {
    p_session->bit_depth_factor = 1;
    p_session->src_bit_depth = 8;
  }

  if (ni_decoder_init_default_params (p_param, nidec->input_state->info.fps_n,
          nidec->input_state->info.fps_d, nidec->input_state->info.finfo->bits,
          nidec->input_state->info.width,
          nidec->input_state->info.height) < 0) {

    GST_ERROR_OBJECT (nidec, "Error setting params");
    g_object_unref (nidec->context);
    nidec->context = NULL;
    return NI_PLUGIN_FAILURE;
  }

  if (nidec->xcoder_opts) {
    gchar *xcoder_opts = g_strdup (nidec->xcoder_opts);
    if (ni_retrieve_decoder_params (xcoder_opts, p_param, p_session)) {
      GST_ERROR_OBJECT (nidec, "Set decoder params error\n");
      g_free (xcoder_opts);
      g_object_unref (nidec->context);
      nidec->context = NULL;
      return NI_PLUGIN_FAILURE;
    }
    g_free (xcoder_opts);
  }

  if (nidec->codec_format == NI_CODEC_FORMAT_H264) {
    ni_decoder_input_params_t *p_dec_input_param = &(p_param->dec_input_params);
    const gint USER_DATA_UNREGISTERED_SEI_PAYLOAD_TYPE = 5;

    if (nidec->custom_sei_type == USER_DATA_UNREGISTERED_SEI_PAYLOAD_TYPE ||
        p_dec_input_param->custom_sei_passthru ==
        USER_DATA_UNREGISTERED_SEI_PAYLOAD_TYPE) {
      nidec->enable_user_data_sei_passthru = 0;
      p_dec_input_param->enable_user_data_sei_passthru = 0;
    } else {
      p_dec_input_param->enable_user_data_sei_passthru =
          nidec->enable_user_data_sei_passthru;
    }
  }

  if (parse_symbolic_decoder_param (nidec) < 0) {
    GST_ERROR_OBJECT (nidec, "Error parse symbolic decoder params");
    g_object_unref (nidec->context);
    nidec->context = NULL;
    return NI_PLUGIN_FAILURE;
  }

  if (p_param->dec_input_params.hwframes) {
    p_session->hw_action = NI_CODEC_HW_ENABLE;
    nidec->hardware_mode = TRUE;
  } else {
    p_session->hw_action = NI_CODEC_HW_NONE;
    nidec->hardware_mode = FALSE;
  }

  nidec->started = 0;
  ni_session_data_io_t *api_pkt =
      gst_niquadra_context_get_data_pkt (nidec->context);
  memset (api_pkt, 0, sizeof (ni_packet_t));
  nidec->pkt_nal_bitmap = 0;

  uint32_t xcoder_timeout = p_param->dec_input_params.keep_alive_timeout;
  if (xcoder_timeout != NI_DEFAULT_KEEP_ALIVE_TIMEOUT) {
    p_session->keep_alive_timeout = xcoder_timeout;
  } else {
    p_session->keep_alive_timeout = nidec->keep_alive_timeout;
  }
  GST_DEBUG_OBJECT (nidec, "Custom NVME Keep Alive Timeout set to %d\n",
      p_session->keep_alive_timeout);
  p_session->hw_id = nidec->dev_dec_idx;
  p_session->decoder_low_delay = nidec->low_delay =
      p_param->dec_input_params.decoder_low_delay;
  gst_xcoder_strncpy (p_session->blk_dev_name, nidec->dev_blk_name,
      NI_MAX_DEVICE_NAME_LEN);

  p_session->p_session_config = p_param;

  p_session->session_id = NI_INVALID_SESSION_ID;

  // assign the card GUID in the encoder context and let session open
  // take care of the rest
  p_session->device_handle = NI_INVALID_DEVICE_HANDLE;
  p_session->blk_io_handle = NI_INVALID_DEVICE_HANDLE;
  p_session->codec_format = nidec->codec_format;

  ret = ni_device_session_open (p_session, NI_DEVICE_TYPE_DECODER);
  if (ret != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (nidec, "Failed to open decoder (status = %d)\n", ret);
    gst_niquadradec_close_decoder (nidec);
    return NI_PLUGIN_FAILURE;
  } else {
    nidec->dev_xcoder_name = p_session->dev_xcoder_name;
    nidec->blk_xcoder_name = p_session->blk_xcoder_name;
    nidec->dev_dec_idx = p_session->hw_id;

    GST_DEBUG_OBJECT (nidec,
        "XCoder %s.%d (inst: %d) %p opened successfully\n",
        nidec->dev_xcoder_name, nidec->dev_dec_idx, p_session->session_id,
        p_session);

    nidec->current_pts = 0;
    nidec->opened = TRUE;
  }

  return NI_PLUGIN_OK;
}

static gboolean
gst_niquadradec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state)
{
  GstNiquadraDec *nidec = GST_NIQUADRADEC (decoder);
  GstNiquadraDecClass *klass = GST_NIQUADRADEC_GET_CLASS (nidec);
  NiPluginError ret = NI_PLUGIN_OK;

  // Judge if the input_state is equal to saved.
  if (nidec->input_state) {
    /* mark for re-negotiation if display resolution or any other video info
     * changes like framerate. */
    if (!gst_video_info_is_equal (&nidec->input_state->info, &state->info)) {
      GST_INFO_OBJECT (nidec, "Schedule renegotiation as video info changed");
    }
    gst_video_codec_state_unref (nidec->input_state);
  }
  nidec->input_state = gst_video_codec_state_ref (state);

  if (nidec->started == TRUE) {
    GST_DEBUG_OBJECT (nidec, "set format, sequence change\n");
    return TRUE;
  }

  ret = gst_niquadradec_xcoder_setup_decoder (nidec);
  if (ret != NI_PLUGIN_OK) {
    GST_ERROR_OBJECT (nidec, "XCoder init failure[%d]\n", ret);
    return FALSE;
  }

  if (klass->parse_format)
    return klass->parse_format (nidec, state);

  return TRUE;
}

static gboolean
mastering_display_metadata_aux_to_gst (ni_mastering_display_metadata_t * ni_aux,
    GstVideoMasteringDisplayInfo * gst)
{
  const guint64 chroma_scale = 50000;
  const guint64 luma_scale = 10000;
  gint i;

  for (i = 0; i < G_N_ELEMENTS (gst->display_primaries); i++) {
    gst->display_primaries[i].x = (guint16) gst_util_uint64_scale (chroma_scale,
        ni_aux->display_primaries[i][0].num,
        ni_aux->display_primaries[i][0].den);
    gst->display_primaries[i].y =
        (guint16) gst_util_uint64_scale (chroma_scale,
        ni_aux->display_primaries[i][1].num,
        ni_aux->display_primaries[i][1].den);
  }

  gst->white_point.x = (guint16) gst_util_uint64_scale (chroma_scale,
      ni_aux->white_point[0].num, ni_aux->white_point[0].den);
  gst->white_point.y = (guint16) gst_util_uint64_scale (chroma_scale,
      ni_aux->white_point[1].num, ni_aux->white_point[1].den);


  gst->max_display_mastering_luminance =
      (guint32) gst_util_uint64_scale (luma_scale,
      ni_aux->max_luminance.num, ni_aux->max_luminance.den);
  gst->min_display_mastering_luminance =
      (guint32) gst_util_uint64_scale (luma_scale,
      ni_aux->min_luminance.num, ni_aux->min_luminance.den);

  return TRUE;
}

static NiPluginError
gst_niquadradec_reset (GstNiquadraDec * nidec)
{
  NiPluginError ret = NI_PLUGIN_OK;
  ni_session_context_t *p_session =
      gst_niquadra_context_get_session_context (nidec->context);
  ni_session_data_io_t *p_pkt =
      gst_niquadra_context_get_data_pkt (nidec->context);
  GST_DEBUG_OBJECT (nidec, "XCode decode reset\n");

  ni_device_session_close (p_session, nidec->eos, NI_DEVICE_TYPE_DECODER);

  ni_device_close (p_session->device_handle);
  ni_device_close (p_session->blk_io_handle);
  p_session->device_handle = NI_INVALID_DEVICE_HANDLE;
  p_session->blk_io_handle = NI_INVALID_DEVICE_HANDLE;

  ni_packet_buffer_free (&(p_pkt->data.packet));
  gint64 bcp_current_pts = nidec->current_pts;
  ret = gst_niquadradec_xcoder_setup_decoder (nidec);
  nidec->current_pts = bcp_current_pts;
  p_session->session_run_state = SESSION_RUN_STATE_RESETTING;
  return ret;
}

static NiPluginError
gst_niquadradec_send_frame (GstNiquadraDec * decoder, GstMapInfo * info,
    GstBuffer * input_buffer, int *send_pkt_size)
{
  int need_draining = 0;
  size_t size = 0;
  int res = 0;
  NiPluginError ret = NI_PLUGIN_OK;
  int sent = 0;
  int send_size = 0;
  int new_packet = 0;
  int extra_prev_size = 0;

  if (!decoder->context) {
    GST_ERROR_OBJECT (decoder, "The decoder context remains uninitialized\n");
    return NI_PLUGIN_FAILURE;
  }

  if (!send_pkt_size) {
    return NI_PLUGIN_EINVAL;
  } else {
    *send_pkt_size = 0;
  }

  ni_session_data_io_t *api_pkt =
      gst_niquadra_context_get_data_pkt (decoder->context);
  ni_session_context_t *p_session =
      gst_niquadra_context_get_session_context (decoder->context);
  ni_xcoder_params_t *p_param =
      gst_niquadra_context_get_xcoder_param (decoder->context);
  ni_packet_t *xpkt = &(api_pkt->data.packet);
  int svct_skip_packet = decoder->svct_skip_next_packet;

  size = info->size;

  if (decoder->flushing && input_buffer) {
    GST_ERROR_OBJECT (decoder, "Decoder is flushing and cannot accept new"
        "buffer until all output buffer have been released\n");
    return NI_PLUGIN_FAILURE;
  }

  if (info->size == 0) {
    need_draining = 1;
  }

  if (decoder->draining && decoder->eos) {
    GST_DEBUG_OBJECT (decoder, "Decoder is draining, eos\n");
    return NI_PLUGIN_EOF;
  }

  if (xpkt->data_len == 0) {
    memset (xpkt, 0, sizeof (ni_packet_t));
    if (!input_buffer) {
      xpkt->pts = 0;
      xpkt->dts = 0;
    } else {
      xpkt->pts = (long long) input_buffer->pts;
      xpkt->dts = (long long) input_buffer->dts;
    }
    xpkt->flags = 0;
    xpkt->video_width = decoder->input_state->info.width;
    xpkt->video_height = decoder->input_state->info.height;
    xpkt->p_data = NULL;
    xpkt->data_len = info->size;

    GST_DEBUG_OBJECT (decoder,
        "size=%ld,prev_size=%d", info->size, p_session->prev_size);
    decoder->svct_skip_next_packet = 0;
    // If there was lone custom sei in the last packet and the firmware would
    // fail to recoginze it. So passthrough the custom sei here.
    if (decoder->lone_pkt_size > 0) {
      // No need to check the return value here because the lone_sei_pkt was
      // parsed before. Here it is only to extract the SEI data.
      // xcoder_packet_parse(avctx, s, &s->lone_sei_pkt, xpkt);
      ni_dec_packet_parse (p_session, p_param, decoder->lone_pkt_data,
          decoder->lone_pkt_size, xpkt, decoder->low_delay,
          decoder->codec_format, decoder->pkt_nal_bitmap,
          decoder->custom_sei_type, &decoder->svct_skip_next_packet,
          &decoder->is_lone_sei_pkt);
    }

    res = ni_dec_packet_parse (p_session, p_param, info->data, info->size, xpkt,
        decoder->low_delay, decoder->codec_format, decoder->pkt_nal_bitmap,
        decoder->custom_sei_type, &decoder->svct_skip_next_packet,
        &decoder->is_lone_sei_pkt);
    if (res != 0) {
      ret = NI_PLUGIN_FAILURE;
      goto fail;
    }

    if (svct_skip_packet) {
      GST_DEBUG_OBJECT (decoder,
          "ff_xcoder_dec_send packet: pts=%" G_GINT64_FORMAT ","
          " size=%" G_GSIZE_FORMAT "\n", (gint64) xpkt->pts, info->size);
      xpkt->data_len = 0;
      *send_pkt_size = info->size;
      return NI_PLUGIN_OK;
    }
    // If the current packet is a lone SEI, save it to be sent with the next
    // packet. And also check if getting the first packet containing key frame
    // in decoder low delay mode.
    if (decoder->is_lone_sei_pkt) {
      if (decoder->lone_pkt_data) {
        g_free (decoder->lone_pkt_data);
      }
      decoder->lone_pkt_data = g_malloc0 (info->size);
      decoder->lone_pkt_size = info->size;
      memcpy (decoder->lone_pkt_data, info->data, info->size);
      xpkt->data_len = 0;
      free (xpkt->p_custom_sei_set);
      xpkt->p_custom_sei_set = NULL;
      if (decoder->low_delay
          && !(decoder->pkt_nal_bitmap & NI_GENERATE_ALL_NAL_HEADER_BIT)) {
        // Packets before the IDR is sent cannot be decoded. So
        // set packet num to zero here.
        p_session->decoder_low_delay = decoder->low_delay;
        p_session->pkt_num = 0;
        decoder->pkt_nal_bitmap |= NI_GENERATE_ALL_NAL_HEADER_BIT;
        GST_DEBUG_OBJECT (decoder,
            "ff_xcoder_dec_send got first IDR in decoder low delay "
            "mode, delay time %dms, pkt_nal_bitmap %d\n",
            decoder->low_delay, decoder->pkt_nal_bitmap);
      }
      GST_DEBUG_OBJECT (decoder,
          "ff_xcoder_dec_send pkt lone SEI, saved, "
          "and return %" G_GSIZE_FORMAT "\n", info->size);
      *send_pkt_size = info->size;
      return NI_PLUGIN_OK;
    }
    // Send the previous saved lone SEI packet to the decoder
    if (decoder->lone_pkt_size > 0) {
      GST_DEBUG_OBJECT (decoder,
          "ff_xcoder_dec_send copy over lone SEI data size: %d\n",
          decoder->lone_pkt_size);
      memcpy (p_session->p_leftover + p_session->prev_size,
          decoder->lone_pkt_data, decoder->lone_pkt_size);
      p_session->prev_size += decoder->lone_pkt_size;
      g_free (decoder->lone_pkt_data);
      decoder->lone_pkt_size = 0;
    }

    if (info->size + p_session->prev_size > 0) {
      ni_packet_buffer_alloc (xpkt, (info->size + p_session->prev_size));
      if (!xpkt->p_data) {
        ret = NI_PLUGIN_ENOMEM;
        goto fail;
      }
    }
    new_packet = 1;
  } else {
    send_size = xpkt->data_len;
  }

  GST_DEBUG_OBJECT (decoder, "xcoder_dec_send: pkt->size=%ld\n", info->size);

  if (decoder->started == 0) {
    GST_DEBUG_OBJECT (decoder, "set start of stream\n");
    xpkt->start_of_stream = 1;
    decoder->started = 1;
  }

  if (need_draining && !decoder->draining) {
    GST_DEBUG_OBJECT (decoder, "Sending End Of Stream signal\n");
    xpkt->end_of_stream = 1;
    xpkt->data_len = 0;

    GST_DEBUG_OBJECT (decoder,
        "ni_packet_copy before: size=%ld, s->prev_size=%d, send_size=%d (end of stream)\n",
        info->size, p_session->prev_size, send_size);
    if (new_packet) {
      extra_prev_size = p_session->prev_size;
      send_size =
          ni_packet_copy (xpkt->p_data, info->data, info->size,
          p_session->p_leftover, &p_session->prev_size);
      // increment offset of data sent to decoder and save it
      xpkt->pos = (long long) decoder->offset;
      decoder->offset += info->size + extra_prev_size;
    }

    GST_DEBUG_OBJECT (decoder,
        "ni_packet_copy after: size=%" G_GSIZE_FORMAT
        ", s->prev_size=%d, send_size=%d, xpkt->data_len=%" G_GUINT32_FORMAT
        " (end of stream)\n", info->size, p_session->prev_size, send_size,
        xpkt->data_len);

    if (send_size < 0) {
      GST_DEBUG_OBJECT (decoder, "Failed to copy pkt (status = "
          "%d)\n", send_size);
      ret = NI_PLUGIN_FAILURE;
      goto fail;
    }
    xpkt->data_len += extra_prev_size;

    sent = 0;
    if (xpkt->data_len > 0) {
      sent =
          ni_device_session_write (p_session, api_pkt, NI_DEVICE_TYPE_DECODER);
    }
    if (sent < 0) {
      GST_ERROR_OBJECT (decoder,
          "Failed to send eos signal (status = %d)\n", sent);
      if (NI_RETCODE_ERROR_VPU_RECOVERY == sent) {
        ret = gst_niquadradec_reset (decoder);
        if (NI_PLUGIN_OK == ret) {
          ret = NI_PLUGIN_EAGAIN;
        } else {
          ret = NI_PLUGIN_FAILURE;
        }
      } else {
        ret = NI_PLUGIN_FAILURE;
      }
      goto fail;
    }
    GST_DEBUG_OBJECT (decoder, "Queued eos (status = %d) ts=%" G_GUINT64_FORMAT,
        sent, (guint64) xpkt->pts);
    decoder->draining = 1;

    ni_device_session_flush (p_session, NI_DEVICE_TYPE_DECODER);
  } else {
    GST_DEBUG_OBJECT (decoder,
        "ni_packet_copy after: size=%" G_GSIZE_FORMAT
        ", s->prev_size=%d, send_size=%d\n", info->size, p_session->prev_size,
        send_size);

    if (new_packet) {
      extra_prev_size = p_session->prev_size;
      send_size =
          ni_packet_copy (xpkt->p_data, info->data, info->size,
          p_session->p_leftover, &p_session->prev_size);
      // increment offset of data sent to decoder and save it
      xpkt->pos = (long long) decoder->offset;
      decoder->offset += info->size + extra_prev_size;
    }

    GST_DEBUG_OBJECT (decoder,
        "ni_packet_copy after: size=%" G_GSIZE_FORMAT
        ", s->prev_size=%d, send_size=%d, xpkt->data_len=%" G_GUINT32_FORMAT
        "\n", info->size, p_session->prev_size, send_size, xpkt->data_len);

    if (send_size < 0) {
      GST_ERROR_OBJECT (decoder,
          "Failed to copy pkt (status = " "%d)\n", send_size);
      ret = NI_PLUGIN_FAILURE;
      goto fail;
    }

    xpkt->data_len += extra_prev_size;

    sent = 0;
    if (xpkt->data_len > 0) {
      sent =
          ni_device_session_write (p_session, api_pkt, NI_DEVICE_TYPE_DECODER);
      GST_DEBUG_OBJECT (decoder,
          "ff_xcoder_dec_send pts=%" PRIi64 ", dts=%" PRIi64 ", pos=%"
          PRIi64 ", sent=%d\n", input_buffer->pts, input_buffer->dts,
          input_buffer->offset, sent);
    }

    if (sent < 0) {
      GST_ERROR_OBJECT (decoder,
          "Failed to send compressed pkt (status = " "%d)\n", sent);
      if (NI_RETCODE_ERROR_VPU_RECOVERY == sent) {
        ret = gst_niquadradec_reset (decoder);
        if (NI_PLUGIN_OK == ret) {
          ret = NI_PLUGIN_EAGAIN;
        } else {
          ret = NI_PLUGIN_FAILURE;
        }
      } else {
        ret = NI_PLUGIN_FAILURE;
      }
      goto fail;
    } else if (sent == 0) {
      GST_DEBUG_OBJECT (decoder, "Queued input buffer size=0\n");
    } else if (sent < size) {   /* partial sent; keep trying */
      GST_DEBUG_OBJECT (decoder, "Queued input buffer size=%d\n", sent);
    }
  }

  if (xpkt->data_len == 0) {
    /* if this packet is done sending, free any sei buffer. */
    free (xpkt->p_custom_sei_set);
    xpkt->p_custom_sei_set = NULL;
  }

  if (sent != 0) {
    //keep the current pkt to resend next time
    ni_packet_buffer_free (xpkt);
    *send_pkt_size = sent;
    return NI_PLUGIN_OK;
  } else {
    return NI_PLUGIN_EAGAIN;
  }

fail:
  ni_packet_buffer_free (xpkt);
  free (xpkt->p_custom_sei_set);
  xpkt->p_custom_sei_set = NULL;
  decoder->draining = 1;
  decoder->eos = 1;

  return ret;
}

static NiPluginError
gst_niquadradec_recv_frame (GstNiquadraDec * decoder,
    ni_session_data_io_t * p_ni_frame)
{
  ni_session_context_t *p_session =
      gst_niquadra_context_get_session_context (decoder->context);
  ni_xcoder_params_t *p_param =
      gst_niquadra_context_get_xcoder_param (decoder->context);
  NiPluginError res = NI_PLUGIN_OK;
  int ret = 0;
  int cropped_width, cropped_height;

  if (decoder->draining && decoder->eos) {
    return NI_PLUGIN_EOF;
  }

  if (decoder->hardware_mode) {
    ret =
        ni_device_session_read_hwdesc (p_session, p_ni_frame,
        NI_DEVICE_TYPE_DECODER);
  } else {
    ret =
        ni_device_session_read (p_session, p_ni_frame, NI_DEVICE_TYPE_DECODER);
  }

  if (ret == 0) {
    decoder->eos = p_ni_frame->data.frame.end_of_stream;
    if (decoder->hardware_mode) {
      ni_frame_buffer_free (&(p_ni_frame->data.frame));
    } else {
      ni_decoder_frame_buffer_free (&(p_ni_frame->data.frame));
    }
    return NI_PLUGIN_EAGAIN;
  } else if (ret > 0) {
    if (p_ni_frame->data.frame.flags & 0x0004) {
      GST_DEBUG_OBJECT (decoder,
          "Current frame is dropped when AV_PKT_FLAG_DISCARD is set\n");
      if (decoder->hardware_mode) {
        ni_frame_buffer_free (&(p_ni_frame->data.frame));
      } else {
        ni_decoder_frame_buffer_free (&(p_ni_frame->data.frame));
      }
      return NI_PLUGIN_EAGAIN;
    }

    GST_DEBUG_OBJECT (decoder,
        "Got output buffer pts=%lld dts=%lld eos=%d sos=%d\n",
        p_ni_frame->data.frame.pts, p_ni_frame->data.frame.dts,
        p_ni_frame->data.frame.end_of_stream,
        p_ni_frame->data.frame.start_of_stream);

    decoder->eos = p_ni_frame->data.frame.end_of_stream;

    decoder->out_width = cropped_width = p_ni_frame->data.frame.video_width;
    decoder->out_height = cropped_height = p_ni_frame->data.frame.video_height;

    if (cropped_width != decoder->width || cropped_height != decoder->height) {
      GST_DEBUG_OBJECT (decoder,
          "Decoder sequence change %dx%d -----> %dx%d ",
          decoder->width, decoder->height, cropped_width, cropped_height);
      decoder->width = cropped_width;
      decoder->height = cropped_height;
      decoder->configured = false;
    }

    if (convertNIPixToGstVideoFormat (p_session->pixel_format) !=
        decoder->pix_fmt) {
      decoder->pix_fmt = convertNIPixToGstVideoFormat (p_session->pixel_format);
      decoder->configured = false;
    }

    if (decoder->configured) {
      if (p_param->dec_input_params.enable_out1 > 0) {
        niFrameSurface1_t *p_surface =
            (niFrameSurface1_t *) (p_ni_frame->data.frame.p_buffer +
            p_ni_frame->data.frame.data_len[0] +
            p_ni_frame->data.frame.data_len[1] +
            p_ni_frame->data.frame.data_len[2] + sizeof (niFrameSurface1_t));
        if ((p_surface->ui16width != decoder->out1_width)
            || (p_surface->ui16height != decoder->out1_height)) {
          decoder->configured = FALSE;
        }
      }

      if (p_param->dec_input_params.enable_out2 > 0) {
        niFrameSurface1_t *p_surface =
            (niFrameSurface1_t *) (p_ni_frame->data.frame.p_buffer +
            p_ni_frame->data.frame.data_len[0] +
            p_ni_frame->data.frame.data_len[1] +
            p_ni_frame->data.frame.data_len[2] +
            2 * sizeof (niFrameSurface1_t));
        if ((p_surface->ui16width != decoder->out2_width)
            || (p_surface->ui16height != decoder->out2_height)) {
          decoder->configured = FALSE;
        }
      }
    }

    GST_DEBUG_OBJECT (decoder,
        "ff_xcoder_dec_receive: frame->pts=%" G_GINT64_FORMAT
        ", frame->pkt_dts=%" G_GINT64_FORMAT,
        (gint64) p_ni_frame->data.frame.pts,
        (gint64) p_ni_frame->data.frame.dts);

    if (decoder->hardware_mode) {
      ni_device_session_copy (p_session, &decoder->hw_api_ctx);
    }
  } else {
    GST_DEBUG_OBJECT (decoder,
        "Failed to get output buffer (status = %d)\n", ret);

    if (NI_RETCODE_ERROR_VPU_RECOVERY == ret) {
      GST_DEBUG_OBJECT (decoder,
          "ff_xcoder_dec_receive VPU recovery, need to reset ..\n");
      if (decoder->hardware_mode) {
        ni_frame_buffer_free (&(p_ni_frame->data.frame));
      } else {
        ni_decoder_frame_buffer_free (&(p_ni_frame->data.frame));
      }

      res = gst_niquadradec_reset (decoder);
      if (NI_PLUGIN_OK == res) {
        return NI_PLUGIN_EAGAIN;
      }
    }

    return NI_PLUGIN_FAILURE;
  }

  return NI_PLUGIN_OK;
}

static NiPluginError
gst_niquadradec_alloc_frame (GstNiquadraDec * decoder,
    ni_session_data_io_t * p_ni_frame)
{
  ni_session_context_t *p_session =
      gst_niquadra_context_get_session_context (decoder->context);
  ni_retcode_t ret = NI_RETCODE_SUCCESS;
  int alloc_mem, height, actual_width;
  int frame_planar;

  memset (p_ni_frame, 0, sizeof (ni_session_data_io_t));

  height =
      (int) (p_session->active_video_height > 0 ? p_session->active_video_height
      : decoder->input_state->info.height);
  actual_width =
      (int) (p_session->actual_video_width > 0 ? p_session->actual_video_width
      : decoder->input_state->info.width);

  // allocate memory only after resolution is known (buffer pool set up)
  alloc_mem = (p_session->active_video_width > 0 &&
      p_session->active_video_height > 0 ? 1 : 0);
  frame_planar = NI_PIXEL_PLANAR_FORMAT_PLANAR;

  if (decoder->hardware_mode) {
    ret = ni_frame_buffer_alloc (&(p_ni_frame->data.frame), actual_width,
        height, (p_session->codec_format == NI_CODEC_FORMAT_H264),
        1, p_session->bit_depth_factor, 3, frame_planar);
  } else {
    ret = ni_decoder_frame_buffer_alloc (p_session->dec_fme_buf_pool,
        &(p_ni_frame->data.frame), alloc_mem, actual_width, height,
        (p_session->codec_format == NI_CODEC_FORMAT_H264),
        p_session->bit_depth_factor, frame_planar);
  }

  if (NI_RETCODE_SUCCESS != ret) {
    return NI_PLUGIN_FAILURE;
  }

  return NI_PLUGIN_OK;
}

typedef struct
{
  GstVideoFormat format;
  ni_pix_fmt_t fmt;
} PixToFmt;

static const PixToFmt pixtofmttable[] = {
  /* GST_VIDEO_FORMAT_I420, */
  {GST_VIDEO_FORMAT_I420, NI_PIX_FMT_YUV420P},
  {GST_VIDEO_FORMAT_UYVY, NI_PIX_FMT_UYVY422},
  {GST_VIDEO_FORMAT_YUY2, NI_PIX_FMT_YUYV422},

  /* GST_VIDEO_FORMAT_RGBA, */
  {GST_VIDEO_FORMAT_RGBA, NI_PIX_FMT_RGBA},
  /* GST_VIDEO_FORMAT_BGRA, */
  {GST_VIDEO_FORMAT_BGRA, NI_PIX_FMT_BGRA},
  /* GST_VIDEO_FORMAT_ARGB, */
  {GST_VIDEO_FORMAT_ARGB, NI_PIX_FMT_ARGB},
  /* GST_VIDEO_FORMAT_ABGR, */
  {GST_VIDEO_FORMAT_ABGR, NI_PIX_FMT_ABGR},
  {GST_VIDEO_FORMAT_NV12, NI_PIX_FMT_NV12},
  {GST_VIDEO_FORMAT_I420_10LE, NI_PIX_FMT_YUV420P10LE},
  {GST_VIDEO_FORMAT_P010_10LE, NI_PIX_FMT_P010LE},
  {GST_VIDEO_FORMAT_NV16, NI_PIX_FMT_NV16},
#if ENABLE_NI_GST_FRAMEWORK_PATCH
  {GST_VIDEO_FORMAT_NI_QUAD_8_4L4, NI_PIX_FMT_8_TILED4X4},
  {GST_VIDEO_FORMAT_NI_QUAD_10_4L4, NI_PIX_FMT_10_TILED4X4},
#endif
};

GstVideoFormat
gst_niquadradec_pixfmt_to_videoformat (ni_pix_fmt_t pixel_format)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (pixtofmttable); i++)
    if (pixtofmttable[i].fmt == pixel_format)
      return pixtofmttable[i].format;

  GST_WARNING ("Unknown pixel format %d", pixel_format);
  return GST_VIDEO_FORMAT_UNKNOWN;
}

static gboolean
retrieve_frame (GstNiquadraDec * decoder,
    ni_session_data_io_t * p_ni_frame, GstVideoCodecFrame * out_frame)
{
  ni_frame_t *xfme = &p_ni_frame->data.frame;
  out_frame->pts = p_ni_frame->data.frame.pts;
  out_frame->dts = p_ni_frame->data.frame.dts;
  out_frame->output_buffer->pts = p_ni_frame->data.frame.pts;
  out_frame->output_buffer->dts = p_ni_frame->data.frame.dts;

  if (!GST_CLOCK_TIME_IS_VALID (out_frame->output_buffer->pts)) {
    GstClockTime duration;
    if (decoder->output_state) {
      GstVideoInfo *info = &decoder->output_state->info;
      if (info->fps_n > 0 && info->fps_d > 0) {
        duration = gst_util_uint64_scale (GST_SECOND, info->fps_d, info->fps_n);
      } else {
        duration = gst_util_uint64_scale (GST_SECOND, 1, 30);
      }
    } else {
      duration = gst_util_uint64_scale (GST_SECOND, 1, 30);
    }
    out_frame->pts = (GstClockTime) decoder->current_pts;
    out_frame->output_buffer->pts = (GstClockTime) decoder->current_pts;
    decoder->current_pts += (int64_t) duration;
    GST_LOG_OBJECT (decoder,
        "Generated PTS %" G_GUINT64_FORMAT " for frame without timestamp",
        out_frame->output_buffer->pts);
  } else {
    decoder->current_pts = (int64_t) out_frame->output_buffer->pts;
  }

  ni_aux_data_t *aux_data = NULL;
  ni_dec_retrieve_aux_data (xfme);

  if (xfme->sei_user_data_unreg_offset) {
    if ((aux_data = ni_frame_get_aux_data (xfme, NI_FRAME_AUX_DATA_UDU_SEI))) {
      GST_DEBUG_OBJECT (decoder, "Found UDU data size %d", aux_data->size);

      /* do not add UDU if it already exists */
      if (gst_buffer_get_video_sei_user_data_unregistered_meta
          (out_frame->input_buffer) == NULL) {
        out_frame->output_buffer =
            gst_buffer_make_writable (out_frame->output_buffer);
        gst_buffer_add_video_sei_user_data_unregistered_meta
            (out_frame->output_buffer, aux_data->data, aux_data->data,
            aux_data->size);
      } else {
        GST_DEBUG_OBJECT (decoder,
            "UDU already exists: will not add new udu meta");
      }
    }
  } else {
    GstVideoSEIUserDataUnregisteredMeta *udu_meta =
        gst_buffer_get_video_sei_user_data_unregistered_meta
        (out_frame->input_buffer);
    if (udu_meta) {
      gboolean removed = gst_buffer_remove_meta (out_frame->input_buffer,
          (GstMeta *) udu_meta);
      if (!removed) {
        GST_WARNING_OBJECT (decoder, "Failed to remove existing UDU meta");
      }
    }
  }
  // closed caption
  GstVideoCaptionMeta *existing_meta =
      gst_buffer_get_video_caption_meta (out_frame->input_buffer);
  if (existing_meta) {
    gboolean removed = gst_buffer_remove_meta (out_frame->input_buffer,
        (GstMeta *) existing_meta);
    if (!removed) {
      GST_WARNING_OBJECT (decoder,
          "Failed to remove existing Video Caption Meta");
    }
  }

  aux_data =
      ni_frame_get_aux_data (&p_ni_frame->data.frame, NI_FRAME_AUX_DATA_A53_CC);
  if (aux_data) {
    out_frame->input_buffer =
        gst_buffer_make_writable (out_frame->input_buffer);
    gst_buffer_add_video_caption_meta (out_frame->input_buffer,
        GST_VIDEO_CAPTION_TYPE_CEA708_RAW, aux_data->data, aux_data->size);
    ni_frame_wipe_aux_data (&p_ni_frame->data.frame);
  }
  // hdr10+
  if ((aux_data = ni_frame_get_aux_data (&p_ni_frame->data.frame,
              NI_FRAME_AUX_DATA_HDR_PLUS))) {
    if (aux_data) {
      if (!gst_buffer_get_meta (out_frame->input_buffer,
              GST_VIDEO_HDR_PLUS_META_API_TYPE)) {
        out_frame->output_buffer =
            gst_buffer_make_writable (out_frame->output_buffer);
        gst_buffer_add_video_hdr_plus_meta (out_frame->output_buffer,
            aux_data->data, aux_data->size);
      } else {
        GST_LOG_OBJECT (decoder,
            "HDR10 PLUS meta already exists: will not add new meta");
      }
    }
  }
  // remember to clean up auxiliary data of ni_frame after their use
  ni_frame_wipe_aux_data (xfme);
  if (xfme->p_custom_sei_set) {
    GST_DEBUG_OBJECT (decoder, "Found Netint Custom data !\n");
    if (!gst_buffer_get_meta (out_frame->input_buffer,
            GST_NETINT_PRIVATE_META_API_TYPE)) {
      out_frame->output_buffer =
          gst_buffer_make_writable (out_frame->output_buffer);
      gst_buffer_add_netint_private_meta (out_frame->output_buffer,
          (const guint8 *) xfme->p_custom_sei_set,
          sizeof (ni_custom_sei_set_t));
    } else {
      GST_LOG_OBJECT (decoder,
          "Netint Custom meta already exists: will not add new meta");
    }
  }
  return TRUE;
}

static gboolean
gst_niquadradec_negotiate (GstNiquadraDec * decoder,
    ni_session_data_io_t * p_ni_frame, GstVideoCodecFrame * out_frame)
{
  GstVideoFormat fmt;
  GstVideoInfo *in_info, *out_info;
  GstVideoCodecState *output_state;
  gint fps_n = 0, fps_d = 1;
  gint in_fps_n = 0, in_fps_d = 1;
  GstStructure *in_s;
  const gchar *s;
  ni_session_context_t *p_ctx =
      gst_niquadra_context_get_session_context (decoder->context);
  ni_xcoder_params_t *p_param =
      gst_niquadra_context_get_xcoder_param (decoder->context);

  fmt = gst_niquadradec_pixfmt_to_videoformat (p_ctx->pixel_format);

  if (decoder->configured)
    return TRUE;
  decoder->configured = true;

  output_state =
      gst_video_decoder_set_output_state (GST_VIDEO_DECODER (decoder), fmt,
      decoder->width, decoder->height, decoder->input_state);

  if (decoder->output_state)
    gst_video_codec_state_unref (decoder->output_state);

  decoder->output_state = output_state;

  in_info = &decoder->input_state->info;
  out_info = &decoder->output_state->info;

  in_s = gst_caps_get_structure (decoder->input_state->caps, 0);

  out_info->interlace_mode = GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;

  if ((s = gst_structure_get_string (in_s, "multiview-mode")))
    GST_VIDEO_INFO_MULTIVIEW_MODE (out_info) =
        gst_video_multiview_mode_from_caps_string (s);
  else
    GST_VIDEO_INFO_MULTIVIEW_MODE (out_info) = GST_VIDEO_MULTIVIEW_MODE_NONE;

  gst_structure_get_flagset (in_s, "multiview-flags",
      &GST_VIDEO_INFO_MULTIVIEW_FLAGS (out_info), NULL);

  // Add metadata here
  if (!gst_structure_has_field (in_s, "colorimetry")
      || in_info->colorimetry.primaries == GST_VIDEO_COLOR_PRIMARIES_UNKNOWN) {
    out_info->colorimetry.primaries = gst_video_color_primaries_from_iso
        (p_ni_frame->data.frame.color_primaries);
  }

  if (!gst_structure_has_field (in_s, "colorimetry")
      || in_info->colorimetry.transfer == GST_VIDEO_TRANSFER_UNKNOWN) {
    out_info->colorimetry.transfer =
        gst_video_transfer_function_from_iso (p_ni_frame->data.frame.color_trc);
  }

  if (!gst_structure_has_field (in_s, "colorimetry")
      || in_info->colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_UNKNOWN) {
    out_info->colorimetry.matrix = gst_video_color_matrix_from_iso (1);
  }

  if (!gst_structure_has_field (in_s, "colorimetry")
      || in_info->colorimetry.range == GST_VIDEO_COLOR_RANGE_UNKNOWN) {
    if (p_ni_frame->data.frame.video_full_range_flag ==
        GST_VIDEO_COLOR_RANGE_16_235) {
      out_info->colorimetry.range = GST_VIDEO_COLOR_RANGE_0_255;
    } else if (p_ni_frame->data.frame.video_full_range_flag ==
        GST_VIDEO_COLOR_RANGE_0_255) {
      out_info->colorimetry.range = GST_VIDEO_COLOR_RANGE_16_235;
    } else {
      out_info->colorimetry.range = GST_VIDEO_COLOR_RANGE_UNKNOWN;
    }
  }

  if (gst_structure_has_field (in_s, "colorspace")) {
    const GValue *colorspace_v;
    const gchar *colorspace;
    if ((colorspace_v = gst_structure_get_value (in_s, "colorspace"))) {
      colorspace = g_value_get_string (colorspace_v);
      if (g_strcmp0 (colorspace, "sYUV") == 0) {
        out_info->colorimetry.range = GST_VIDEO_COLOR_RANGE_16_235;
      }
    }
  }

  /* try to find a good framerate */
  if ((in_info->fps_d && in_info->fps_n) ||
      GST_VIDEO_INFO_FLAG_IS_SET (in_info, GST_VIDEO_FLAG_VARIABLE_FPS)) {
    fps_n = in_info->fps_n;
    fps_d = in_info->fps_d;
  }
  if (decoder->codec_format == NI_CODEC_FORMAT_JPEG) {
    fps_n = fps_d = 1;
  }
  if (gst_structure_has_field (in_s, "framerate")) {
    gst_structure_get_fraction (in_s, "framerate", &in_fps_n, &in_fps_d);
  }
  if (in_fps_d != 0 && in_fps_n != 0) {
    out_info->fps_n = in_fps_n;
    out_info->fps_d = in_fps_d;
  } else {
    out_info->fps_n = fps_n;
    out_info->fps_d = fps_d;
  }

  GST_LOG_OBJECT (decoder, "setting framerate: %d/%d", out_info->fps_n,
      out_info->fps_d);

  if (decoder->out_width && decoder->out_height) {
    out_info->width = decoder->out_width;
    out_info->height = decoder->out_height;
  }

  /* To passing HDR information to caps directly */
  if (output_state->caps == NULL) {
    output_state->caps = gst_video_info_to_caps (out_info);
  } else {
    output_state->caps = gst_caps_make_writable (output_state->caps);
  }

  if (!gst_structure_has_field (in_s, "mastering-display-info")) {
    // retrieve side data if available
    ni_aux_data_t *aux_data = NULL;
    ni_dec_retrieve_aux_data (&p_ni_frame->data.frame);
    GstVideoMasteringDisplayInfo minfo;

    // master_display_color
    if ((aux_data =
            ni_frame_get_aux_data (&p_ni_frame->data.frame,
                NI_FRAME_AUX_DATA_MASTERING_DISPLAY_METADATA))
        && p_ni_frame->data.frame.sei_hdr_mastering_display_color_vol_len > 0) {
      if (aux_data
          &&
          mastering_display_metadata_aux_to_gst (
              (ni_mastering_display_metadata_t *) aux_data, &minfo)) {
        GST_LOG_OBJECT (decoder,
            "update mastering display info: " "Red(%u, %u) " "Green(%u, %u) "
            "Blue(%u, %u) " "White(%u, %u) " "max_luminance(%u) "
            "min_luminance(%u) ", minfo.display_primaries[0].x,
            minfo.display_primaries[0].y, minfo.display_primaries[1].x,
            minfo.display_primaries[1].y, minfo.display_primaries[2].x,
            minfo.display_primaries[2].y, minfo.white_point.x,
            minfo.white_point.y, minfo.max_display_mastering_luminance,
            minfo.min_display_mastering_luminance);

        if (!gst_video_mastering_display_info_add_to_caps (&minfo,
                output_state->caps)) {
          GST_WARNING_OBJECT (decoder,
              "Couldn't set mastering display info to caps");
        }
      }
    }
    // content-light-level
    if ((aux_data =
            ni_frame_get_aux_data (&p_ni_frame->data.frame,
                NI_FRAME_AUX_DATA_CONTENT_LIGHT_LEVEL))
        && p_ni_frame->data.frame.sei_hdr_content_light_level_info_len > 0) {
      if (aux_data) {
        GstVideoContentLightLevel cll;
        ni_content_light_level_t *ni_cll =
            (ni_content_light_level_t *) aux_data;
        cll.max_frame_average_light_level = ni_cll->max_fall;
        cll.max_content_light_level = ni_cll->max_cll;
        if (!gst_video_content_light_level_add_to_caps (&cll,
                output_state->caps)) {
          GST_WARNING_OBJECT (decoder,
              "Couldn't set content light level to caps");
        }
      }
    }
    // closed caption
    GstVideoCaptionMeta *existing_meta =
        gst_buffer_get_video_caption_meta (out_frame->input_buffer);
    if (existing_meta) {
      gboolean removed = gst_buffer_remove_meta (out_frame->input_buffer,
          (GstMeta *) existing_meta);
      if (!removed) {
        GST_WARNING_OBJECT (decoder,
            "Failed to remove existing Video Caption Meta");
      }
    }

    if ((aux_data =
            ni_frame_get_aux_data (&p_ni_frame->data.frame,
                NI_FRAME_AUX_DATA_A53_CC))) {
      out_frame->input_buffer =
          gst_buffer_make_writable (out_frame->input_buffer);
      gst_buffer_add_video_caption_meta (out_frame->input_buffer,
          GST_VIDEO_CAPTION_TYPE_CEA708_RAW, aux_data->data, aux_data->size);

      ni_frame_wipe_aux_data (&p_ni_frame->data.frame);
    }
  }

  if (decoder->hardware_mode) {
    gst_caps_set_features_simple (output_state->caps,
        gst_caps_features_from_string
        (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY));
  }

  if (p_param->dec_input_params.enable_out1 > 0) {
    niFrameSurface1_t *p_surface =
        (niFrameSurface1_t *) (p_ni_frame->data.frame.p_buffer +
        p_ni_frame->data.frame.data_len[0] +
        p_ni_frame->data.frame.data_len[1] +
        p_ni_frame->data.frame.data_len[2] + sizeof (niFrameSurface1_t));

    decoder->enable_out1 = TRUE;
    decoder->out1_width = p_surface->ui16width;
    decoder->out1_height = p_surface->ui16height;

    gst_caps_set_simple (output_state->caps, "enable_out1", G_TYPE_INT,
        1, NULL);
    gst_caps_set_simple (output_state->caps, "out1_width", G_TYPE_INT,
        p_surface->ui16width, NULL);
    gst_caps_set_simple (output_state->caps, "out1_height", G_TYPE_INT,
        p_surface->ui16height, NULL);
  }

  if (p_param->dec_input_params.enable_out2 > 0) {
    niFrameSurface1_t *p_surface =
        (niFrameSurface1_t *) (p_ni_frame->data.frame.p_buffer +
        p_ni_frame->data.frame.data_len[0] +
        p_ni_frame->data.frame.data_len[1] +
        p_ni_frame->data.frame.data_len[2] + 2 * sizeof (niFrameSurface1_t));

    decoder->enable_out2 = TRUE;
    decoder->out2_width = p_surface->ui16width;
    decoder->out2_height = p_surface->ui16height;

    gst_caps_set_simple (output_state->caps, "enable_out2", G_TYPE_INT,
        1, NULL);
    gst_caps_set_simple (output_state->caps, "out2_width", G_TYPE_INT,
        p_surface->ui16width, NULL);
    gst_caps_set_simple (output_state->caps, "out2_height", G_TYPE_INT,
        p_surface->ui16height, NULL);
  }

  if (!gst_video_decoder_negotiate (GST_VIDEO_DECODER (decoder))) {
    GST_ERROR_OBJECT (decoder, "Failed to negotiate");
    return FALSE;
  }

  return TRUE;
}

static GstVideoCodecFrame *
gst_niquadradec_find_best_frame (GstNiquadraDec * dec, GstClockTime dts)
{
  GList *frames, *iter;
  GstVideoCodecFrame *ret = NULL;

  frames = gst_video_decoder_get_frames (GST_VIDEO_DECODER (dec));
  for (iter = frames; iter; iter = g_list_next (iter)) {
    GstVideoCodecFrame *frame = (GstVideoCodecFrame *) iter->data;
    if (frame->dts == dts) {
      ret = gst_video_codec_frame_ref (frame);
      break;
    }
  }

  if (frames) {
    g_list_free_full (frames, (GDestroyNotify) gst_video_codec_frame_unref);
  }

  return ret;
}

static gboolean
gst_niquadradec_video_frame (GstNiquadraDec * decoder, GstFlowReturn * ret)
{
  NiPluginError res = NI_PLUGIN_OK;
  gboolean got_frame = FALSE;
  GstVideoInfo *output_info = NULL;
  GstBuffer *outbuf = NULL;
  GstVideoCodecFrame *dframe = NULL;
  ni_session_data_io_t ni_frame;
  memset (&ni_frame, 0, sizeof (ni_session_data_io_t));

  if (!decoder->context) {
    GST_ERROR_OBJECT (decoder, "The decoder context remains uninitialized\n");
    *ret = GST_FLOW_ERROR;
    return FALSE;
  }

  ni_session_context_t *p_ctx =
      gst_niquadra_context_get_session_context (decoder->context);
  ni_xcoder_params_t *p_param =
      gst_niquadra_context_get_xcoder_param (decoder->context);
  int num_extra_outputs = (p_param->dec_input_params.enable_out1 > 0) +
      (p_param->dec_input_params.enable_out2 > 0);

  *ret = GST_FLOW_OK;

  res = gst_niquadradec_alloc_frame (decoder, &ni_frame);
  if (res < 0) {
    GST_ERROR_OBJECT (decoder, "alloc frame error\n");
    *ret = GST_FLOW_ERROR;
    goto fail;
  }

  res = gst_niquadradec_recv_frame (decoder, &ni_frame);
  if (res == NI_PLUGIN_EAGAIN)  // No frames available at this time
    goto beach;
  else if (res == NI_PLUGIN_EOF) {
    *ret = GST_FLOW_EOS;
    GST_DEBUG_OBJECT (decoder, "Context was entirely flushed");
    goto beach;
  } else if (res < 0) {
    *ret = GST_FLOW_ERROR;
    goto fail;
  }

  got_frame = TRUE;

  dframe = gst_niquadradec_find_best_frame (decoder, ni_frame.data.frame.dts);
  if (!dframe) {
    dframe = gst_video_decoder_get_oldest_frame (GST_VIDEO_DECODER (decoder));
    if (!dframe) {
      *ret = GST_FLOW_OK;
      goto beach;
    }
  }

  if (!gst_niquadradec_negotiate (decoder, &ni_frame, dframe))
    goto negotiation_error;

  if (!decoder->hardware_mode) {
    *ret = gst_video_decoder_allocate_output_frame (GST_VIDEO_DECODER (decoder),
        dframe);
    if (*ret != GST_FLOW_OK) {
      GST_WARNING_OBJECT (decoder,
          "Failed to allocate SW output buffer, flow=%s",
          gst_flow_get_name (*ret));
      goto fail;
    }
  } else {
    outbuf = gst_buffer_new ();
    dframe->output_buffer = outbuf;
  }

  output_info = &decoder->output_state->info;

  GST_DEBUG_OBJECT (decoder,
      "Decoding pts=%" G_GUINT64_FORMAT ", dts=%" G_GUINT64_FORMAT
      ", sys_frame=%d", dframe->pts, dframe->dts, dframe->system_frame_number);

  retrieve_frame (decoder, &ni_frame, dframe);

  if (!decoder->hardware_mode) {
    int i, j;
    uint8_t *src;
    uint8_t *dst;
    GstVideoFrame vframe;
    memset (&vframe, 0, sizeof (GstVideoFrame));

    if (!gst_video_frame_map (&vframe, output_info, dframe->output_buffer,
            GST_MAP_READ | GST_MAP_WRITE)) {
      GST_ERROR_OBJECT (decoder, "video frame map failed\n");
      goto no_output;
    }

    for (i = 0; i < 3; i++) {
      src = ni_frame.data.frame.p_data[i];

      int row_stride = GST_VIDEO_FRAME_COMP_STRIDE (&vframe, i);
      int plane_height = p_ctx->active_video_height;
      int plane_width = p_ctx->active_video_width;
      int write_height = GST_VIDEO_FRAME_HEIGHT (&vframe);
      int write_width = GST_VIDEO_FRAME_WIDTH (&vframe);

      // support for 8/10 bit depth
      // plane_width is the actual Y stride size
      write_width *= p_ctx->bit_depth_factor;

      if (i == 1 || i == 2) {
        plane_height /= 2;
        // U/V stride size is multiple of 128, following the calculation
        // in ni_decoder_frame_buffer_alloc
        plane_width = (((int) (p_ctx->actual_video_width) / 2 *
                p_ctx->bit_depth_factor + 127) / 128) * 128;

        write_height /= 2;
        write_width /= 2;
      }

      dst = GST_VIDEO_FRAME_PLANE_DATA (&vframe, i);
      // apply the cropping windown in writing out the YUV frame
      // for now the windown is usually crop-left = crop-top = 0, and we
      // use this to simplify the cropping logic
      for (j = 0; j < plane_height; j++) {
        memcpy (dst, src, write_width);
        src += plane_width;
        dst += row_stride;
      }
    }

    gst_video_frame_unmap (&vframe);
  } else {
    if (p_ctx->frame_num == 1) {
      ni_device_session_copy (p_ctx, &decoder->hw_api_ctx);
    }

    GstAllocator *alloc = gst_allocator_find (GST_NIQUADRA_MEMORY_TYPE_NAME);
    GstMemory *in_mem = NULL;
    for (int i = 0; i <= num_extra_outputs; i++) {
      niFrameSurface1_t *p_data3 = NULL;

      p_data3 = (niFrameSurface1_t *) (ni_frame.data.frame.p_buffer +
          ni_frame.data.frame.data_len[0] + ni_frame.data.frame.data_len[1] +
          ni_frame.data.frame.data_len[2] + i * sizeof (niFrameSurface1_t));
      if (!p_data3) {
        *ret = GST_FLOW_ERROR;
        got_frame = FALSE;
        goto fail;
      }

      GST_DEBUG_OBJECT (decoder,
          "retrieve_frame: OUT%d data[3] trace ui16FrameIdx = [%d], device_handle=%d bitdep=%d, WxH %d x %d\n",
          i, p_data3->ui16FrameIdx, p_data3->device_handle, p_data3->bit_depth,
          p_data3->ui16width, p_data3->ui16height);

      in_mem = gst_niquadra_allocator_alloc (alloc, &decoder->hw_api_ctx,
          p_data3, decoder->dev_dec_idx, output_info);
      gst_buffer_append_memory (outbuf, in_mem);
    }
    gst_object_unref (alloc);
  }

  free (ni_frame.data.frame.p_custom_sei_set);
  ni_frame.data.frame.p_custom_sei_set = NULL;

  *ret = gst_video_decoder_finish_frame (GST_VIDEO_DECODER (decoder), dframe);

beach:
  GST_DEBUG_OBJECT (decoder, "return flow %s, got frame: %d",
      gst_flow_get_name (*ret), got_frame);

  if (decoder->hardware_mode) {
    ni_frame_buffer_free (&(ni_frame.data.frame));
  } else {
    ni_decoder_frame_buffer_free (&(ni_frame.data.frame));
  }

  return got_frame;

  /* special cases */
no_output:
  {
    GST_DEBUG_OBJECT (decoder, "no output buffer");
    if (dframe) {
      gst_video_decoder_drop_frame (GST_VIDEO_DECODER (decoder), dframe);
    }
    goto beach;
  }

negotiation_error:
  {
    gst_video_decoder_drop_frame (GST_VIDEO_DECODER (decoder), dframe);

    if (GST_PAD_IS_FLUSHING (GST_VIDEO_DECODER_SRC_PAD (decoder))) {
      *ret = GST_FLOW_FLUSHING;
      goto beach;
    }
    GST_WARNING_OBJECT (decoder, "Error negotiating format");
    *ret = GST_FLOW_NOT_NEGOTIATED;
    goto beach;
  }

fail:
  {
    GST_ERROR_OBJECT (decoder,
        "An error occurred when receiving a decoded frame.\n");
    decoder->draining = 1;
    decoder->eos = 1;
    goto beach;
  }
}

static gboolean
gst_niquadradec_frame (GstNiquadraDec * decoder, GstFlowReturn * ret)
{
  gboolean got_frame = FALSE;

  *ret = GST_FLOW_OK;
  decoder->frame_number++;

  got_frame = gst_niquadradec_video_frame (decoder, ret);

  return got_frame;
}

static GstFlowReturn
gst_niquadradec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  GstNiquadraDec *thiz = GST_NIQUADRADEC (decoder);
  GstNiquadraDecClass *klass = GST_NIQUADRADEC_GET_CLASS (thiz);
  GstFlowReturn flow_ret = GST_FLOW_OK;
  NiPluginError ret = NI_PLUGIN_OK;
  gboolean got_frame = FALSE;
  GstBuffer *input_buffer = NULL;
  gboolean input_buffer_mapped = FALSE;
  GstMapInfo map_info;
  memset (&map_info, 0, sizeof (GstMapInfo));

  // 1. Receive pkt data from upstream plugin.
  GST_DEBUG_OBJECT (thiz,
      "Received new data of size %" G_GSIZE_FORMAT ", dts %"
      GST_TIME_FORMAT ", pts:%" GST_TIME_FORMAT ",buffer pts:%" GST_TIME_FORMAT
      ", dur:%" GST_TIME_FORMAT, gst_buffer_get_size (frame->input_buffer),
      GST_TIME_ARGS (frame->dts), GST_TIME_ARGS (frame->pts),
      GST_TIME_ARGS (frame->input_buffer->pts),
      GST_TIME_ARGS (frame->duration));

  if (klass->process_buffer) {
    input_buffer = klass->process_buffer (thiz, frame->input_buffer);
  } else {
    input_buffer = gst_buffer_ref (frame->input_buffer);
  }

  if (G_UNLIKELY (!input_buffer)) {
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
  // Map upstream buffer to map_info
  if (!gst_buffer_map (input_buffer, &map_info, GST_MAP_READ)) {
    gst_buffer_unref (input_buffer);
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
  input_buffer_mapped = TRUE;

  GST_DEBUG_OBJECT (thiz,
      "Handle_frame, pts=%lu,dts=%lu, sys=%d, dec=%d",
      frame->input_buffer->pts, frame->input_buffer->dts,
      frame->system_frame_number, frame->decode_frame_number);

  GST_VIDEO_DECODER_STREAM_UNLOCK (thiz);

  // 2. send frame.
  do {
    int send_pkt_size = 0;
    ret = gst_niquadradec_send_frame (thiz, &map_info, input_buffer,
        &send_pkt_size);
    if (ret < 0 && ret != NI_PLUGIN_EAGAIN) {
      GST_ERROR_OBJECT (thiz, "niquadradec send frame error, ret=%d\n", ret);
      flow_ret = GST_FLOW_ERROR;
      GST_VIDEO_DECODER_STREAM_LOCK (thiz);
      goto error;
    }

    if (ret == NI_PLUGIN_EAGAIN) {
      g_usleep (10 * 1000);

      got_frame = gst_niquadradec_frame (thiz, &flow_ret);
      if (flow_ret != GST_FLOW_OK) {
        GST_VIDEO_DECODER_STREAM_LOCK (thiz);
        goto error;
      }
    }
  } while (ret == NI_PLUGIN_EAGAIN);

  GST_VIDEO_DECODER_STREAM_LOCK (thiz);

  gst_buffer_unmap (input_buffer, &map_info);
  gst_buffer_unref (input_buffer);
  gst_video_codec_frame_unref (frame);

  do {
    got_frame = gst_niquadradec_frame (thiz, &flow_ret);
    if (flow_ret != GST_FLOW_OK) {
      GST_LOG_OBJECT (thiz, "breaking because of flow ret %s",
          gst_flow_get_name (flow_ret));
      break;
    }
  } while (got_frame);

  return flow_ret;

error:
  if (input_buffer_mapped)
    gst_buffer_unmap (input_buffer, &map_info);
  gst_clear_buffer (&input_buffer);
  gst_video_codec_frame_unref (frame);
  return flow_ret;
}

static GstCaps *
gst_niquadradec_sink_getcaps (GstVideoDecoder * decoder, GstCaps * filter)
{
  GstCaps *caps, *tmp = NULL;

  caps = gst_pad_get_pad_template_caps (decoder->sinkpad);
  if (caps) {
    if (filter) {
      tmp = gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
      gst_caps_unref (caps);
      caps = tmp;
    }
  } else {
    caps = gst_video_decoder_proxy_getcaps (decoder, NULL, filter);
  }

  return caps;
}

static void
gst_niquadradec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiquadraDec *thiz = GST_NIQUADRADEC (object);
  GstState state;

  GST_OBJECT_LOCK (thiz);

  state = GST_STATE (thiz);
  if ((state != GST_STATE_READY && state != GST_STATE_NULL) &&
      !(pspec->flags & GST_PARAM_MUTABLE_PLAYING))
    goto wrong_state;

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (thiz);
  return;

wrong_state:
  GST_WARNING_OBJECT (thiz, "setting property in wrong state");
  GST_OBJECT_UNLOCK (thiz);
}

static void
gst_niquadradec_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstNiquadraDec *thiz = GST_NIQUADRADEC (object);

  GST_OBJECT_LOCK (thiz);
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (thiz);
}

static GstFlowReturn
gst_niquadradec_drain (GstVideoDecoder * decoder)
{
  GstNiquadraDec *thiz = GST_NIQUADRADEC (decoder);
  GstFlowReturn ret = GST_FLOW_OK;
  gboolean got_frame = FALSE;

  if (!thiz->opened || !thiz->started)
    return GST_FLOW_OK;

  do {
    got_frame = gst_niquadradec_frame (thiz, &ret);
    GST_DEBUG_OBJECT (thiz, "Drain, got=%d, ret=%d\n", got_frame, ret);
  } while (got_frame && ret == GST_FLOW_OK);

  return GST_FLOW_OK;
}

static gboolean
gst_niquadradec_flush (GstVideoDecoder * decoder)
{
  GstNiquadraDec *thiz = GST_NIQUADRADEC (decoder);
  GstFlowReturn ret = GST_FLOW_OK;
  int send_pkt_size = 0;
  G_GNUC_UNUSED gboolean got_frame = FALSE;
  GstMapInfo info;
  info.size = 0;
  info.data = NULL;

  if (!thiz->opened) {
    return TRUE;
  }
  // If eos and drining flags are already set, likely a decoding error has happend
  // Do not set flush command in this case to prevent any unexpected behavior in FW
  if (thiz->draining && thiz->eos) {
    return TRUE;
  }

  gst_niquadradec_send_frame (thiz, &info, NULL, &send_pkt_size);
  do {
    got_frame = gst_niquadradec_frame (thiz, &ret);
  } while (ret != GST_FLOW_EOS && ret != GST_FLOW_ERROR);

  ni_session_context_t *p_ctx =
      gst_niquadra_context_get_session_context (thiz->context);

  if (ni_device_dec_session_flush (p_ctx) != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "Failed to send flush to device\n");
    return FALSE;
  }

  thiz->flushing = FALSE;
  thiz->eos = FALSE;
  thiz->draining = FALSE;

  return TRUE;
}

static gboolean
gst_niquadradec_stop (GstVideoDecoder * decoder)
{
  GstNiquadraDec *thiz = GST_NIQUADRADEC (decoder);
  GstNiquadraDecClass *decoder_class = GST_NIQUADRADEC_GET_CLASS (decoder);

  gst_niquadradec_flush (decoder);

  gst_niquadradec_close_decoder (thiz);

  if (thiz->input_state) {
    gst_video_codec_state_unref (thiz->input_state);
    thiz->input_state = NULL;
  }
  if (thiz->output_state) {
    gst_video_codec_state_unref (thiz->output_state);
    thiz->output_state = NULL;
  }
  if (thiz->internal_pool) {
    gst_object_unref (thiz->internal_pool);
    thiz->internal_pool = NULL;
  }

  thiz->width = 0;
  thiz->height = 0;
  thiz->pool_width = 0;
  thiz->pool_height = 0;
  if (thiz->xcoder_opts) {
    g_free (thiz->xcoder_opts);
    thiz->xcoder_opts = NULL;
  }

  if (decoder_class->release)
    decoder_class->release (thiz);

  return TRUE;
}

static GstFlowReturn
gst_niquadradec_finish (GstVideoDecoder * decoder)
{
  GstNiquadraDec *thiz = GST_NIQUADRADEC (decoder);
  int send_pkt_size = 0;
  GstMapInfo info;
  info.size = 0;
  info.data = NULL;

  gst_niquadradec_send_frame (thiz, &info, NULL, &send_pkt_size);
  return gst_niquadradec_drain (decoder);
}

static gboolean
gst_niquadradec_decide_allocation (GstVideoDecoder * decoder, GstQuery * query)
{
  GstNiquadraDec *thiz = GST_NIQUADRADEC (decoder);
  GstVideoCodecState *state;
  GstBufferPool *pool;
  guint size, min, max;
  GstStructure *config;
  gboolean have_videometa, update_pool = FALSE;
  GstAllocator *allocator = NULL;
  GstAllocationParams params = DEFAULT_ALLOC_PARAM;

  if (!GST_VIDEO_DECODER_CLASS (parent_class)->decide_allocation (decoder,
          query))
    return FALSE;

  state = gst_video_decoder_get_output_state (decoder);

  if (gst_query_get_n_allocation_params (query) > 0) {
    gst_query_parse_nth_allocation_param (query, 0, &allocator, &params);
    params.align = MAX (params.align, DEFAULT_STRIDE_ALIGN);
  } else {
    gst_query_add_allocation_param (query, allocator, &params);
  }

  gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);

  /* Don't use pool that can't grow, as we don't know how many buffer we'll
   * need, otherwise we may stall */
  if (max != 0 && max < REQUIRED_POOL_MAX_BUFFERS) {
    gst_object_unref (pool);
    pool = gst_video_buffer_pool_new ();
    max = 0;
    update_pool = TRUE;

    /* if there is an allocator, also drop it, as it might be the reason we
     * have this limit. Default will be used */
    if (allocator) {
      gst_object_unref (allocator);
      allocator = NULL;
    }
  }

  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (config, state->caps, size, min, max);
  gst_buffer_pool_config_set_allocator (config, allocator, &params);

  have_videometa =
      gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);

  if (have_videometa)
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);

  if (have_videometa && thiz->internal_pool
      && thiz->pool_width == state->info.width
      && thiz->pool_height == state->info.height) {
    update_pool = TRUE;
    gst_object_unref (pool);
    pool = gst_object_ref (thiz->internal_pool);
    gst_structure_free (config);
    goto done;
  }

  /* configure */
  if (!gst_buffer_pool_set_config (pool, config)) {
    gboolean working_pool = FALSE;
    config = gst_buffer_pool_get_config (pool);

    if (gst_buffer_pool_config_validate_params (config, state->caps, size, min,
            max)) {
      working_pool = gst_buffer_pool_set_config (pool, config);
    } else {
      gst_structure_free (config);
    }

    if (!working_pool) {
      gst_object_unref (pool);
      pool = gst_video_buffer_pool_new ();
      config = gst_buffer_pool_get_config (pool);
      gst_buffer_pool_config_set_params (config, state->caps, size, min, max);
      gst_buffer_pool_config_set_allocator (config, NULL, &params);
      gst_buffer_pool_set_config (pool, config);
      update_pool = TRUE;
    }
  }

done:
  /* and store */
  if (update_pool)
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);

  gst_object_unref (pool);
  if (allocator)
    gst_object_unref (allocator);
  gst_video_codec_state_unref (state);

  return TRUE;
}

static gboolean
gst_niquadradec_propose_allocation (GstVideoDecoder * decoder, GstQuery * query)
{
  GstAllocationParams params;

  gst_allocation_params_init (&params);
  params.flags = GST_MEMORY_FLAG_ZERO_PADDED;
  params.align = DEFAULT_STRIDE_ALIGN;
  gst_query_add_allocation_param (query, NULL, &params);

  return GST_VIDEO_DECODER_CLASS (parent_class)->propose_allocation (decoder,
      query);
}

static void
gst_niquadradec_dispose (GObject * object)
{
  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_niquadradec_finalize (GObject * object)
{
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_niquadradec_transform_meta (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame, GstMeta * meta)
{
  const GstMetaInfo *info = meta->info;

  if (GST_VIDEO_DECODER_CLASS (parent_class)->transform_meta (decoder, frame,
          meta))
    return TRUE;

  if (!g_strcmp0 (g_type_name (info->type), "GstVideoRegionOfInterestMeta"))
    return TRUE;

  return FALSE;
}

static gboolean
gst_niquadradec_sink_event (GstVideoDecoder * decoder, GstEvent * event)
{
  GstNiquadraDec *self = GST_NIQUADRADEC (decoder);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      self->flushing = TRUE;
      break;
    default:
      break;
  }

  return GST_VIDEO_DECODER_CLASS (parent_class)->sink_event (decoder, event);
}

static void
gst_niquadradec_class_init (GstNiquadraDecClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstVideoDecoderClass *decoder_class;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  decoder_class = GST_VIDEO_DECODER_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (gst_niquadradec_debug, "niquadradec", 0,
      "niquadradec");

  gobject_class->set_property = gst_niquadradec_set_property;
  gobject_class->get_property = gst_niquadradec_get_property;
  gobject_class->dispose = gst_niquadradec_dispose;
  gobject_class->finalize = gst_niquadradec_finalize;

  element_class->set_context = gst_niquadradec_set_context;

  decoder_class->close = GST_DEBUG_FUNCPTR (gst_niquadradec_close);
  decoder_class->start = GST_DEBUG_FUNCPTR (gst_niquadradec_start);
  decoder_class->stop = GST_DEBUG_FUNCPTR (gst_niquadradec_stop);
  decoder_class->set_format = GST_DEBUG_FUNCPTR (gst_niquadradec_set_format);
  decoder_class->finish = GST_DEBUG_FUNCPTR (gst_niquadradec_finish);
  decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_niquadradec_handle_frame);
  decoder_class->getcaps = GST_DEBUG_FUNCPTR (gst_niquadradec_sink_getcaps);
  decoder_class->parse = NULL;
  decoder_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_niquadradec_decide_allocation);
  decoder_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_niquadradec_propose_allocation);
  decoder_class->flush = GST_DEBUG_FUNCPTR (gst_niquadradec_flush);
  decoder_class->drain = GST_DEBUG_FUNCPTR (gst_niquadradec_drain);
  decoder_class->transform_meta =
      GST_DEBUG_FUNCPTR (gst_niquadradec_transform_meta);
  decoder_class->sink_event = GST_DEBUG_FUNCPTR (gst_niquadradec_sink_event);

  gst_element_class_add_static_pad_template (element_class, &src_factory);
}

#define PROP_DEC_INDEX -1

static void
gst_niquadradec_init (GstNiquadraDec * thiz)
{
  thiz->input_state = NULL;
  thiz->output_state = NULL;
  thiz->internal_pool = NULL;
  thiz->keep_alive_timeout = NI_DEFAULT_KEEP_ALIVE_TIMEOUT;
  thiz->dev_dec_idx = PROP_DEC_INDEX;
  thiz->hardware_mode = FALSE;
  thiz->configured = false;
  thiz->out_width = 0;
  thiz->out_height = 0;
  thiz->context = NULL;
}

typedef struct
{
  int hw_mode;
  int configure_scale0;
  int enable_out1;
  int enable_out2;
  int w0, h0;
  int w1, h1;
  int w2, h2;
} xcoder_params_t;

static gboolean
parse_scale (const char *scale_str, int *width, int *height)
{
  if (!scale_str || !width || !height) {
    return FALSE;
  }

  char *x_pos = strchr (scale_str, 'x');
  if (!x_pos) {
    return FALSE;
  }

  *width = atoi (scale_str);
  *height = atoi (x_pos + 1);

  if (*width <= 0 || *height <= 0) {
    return FALSE;
  }

  return TRUE;
}

/* *INDENT-OFF* */
static gboolean
parse_xcoder_params (const char *params_str, xcoder_params_t *result)
/* *INDENT-ON* */

{
  char *token = NULL;
  char *saveptr = NULL;
  gboolean ret = TRUE;

  if (!params_str || (strlen (params_str) == 0) || !result) {
    GST_ERROR ("input param are invalid for parse_xcoder_params\n");
    return FALSE;
  }

  char *params_copy = strdup (params_str);
  if (!params_copy) {
    GST_ERROR ("Failed to strdup params_str\n");
    return FALSE;
  }

  token = strtok_r (params_copy, ":", &saveptr);
  while (token != NULL) {
    char *equal_pos = strchr (token, '=');
    if (!equal_pos) {
      token = strtok_r (NULL, ":", &saveptr);
      continue;
    }

    *equal_pos = '\0';
    char *key = token;
    char *value = equal_pos + 1;

    if (strcmp (key, "out") == 0) {
      if (strcmp (value, "hw") == 0) {
        result->hw_mode = 1;
      } else {
        result->hw_mode = 0;
      }
    } else if (strcmp (key, "enableOut1") == 0) {
      result->enable_out1 = atoi (value);
    } else if (strcmp (key, "enableOut2") == 0) {
      result->enable_out2 = atoi (value);
    } else if (strcmp (key, "scale0") == 0) {
      result->configure_scale0 = 1;
      if (!parse_scale (value, &result->w0, &result->h0)) {
        GST_ERROR ("Failed to parse scale0\n");
        ret = FALSE;
        goto EXIT;
      }
    } else if (strcmp (key, "scale1") == 0) {
      if (!parse_scale (value, &result->w1, &result->h1)) {
        GST_ERROR ("Failed to parse scale1\n");
        ret = FALSE;
        goto EXIT;
      }
    } else if (strcmp (key, "scale2") == 0) {
      if (!parse_scale (value, &result->w2, &result->h2)) {
        GST_ERROR ("Failed to parse scale2\n");
        ret = FALSE;
        goto EXIT;
      }
    }

    token = strtok_r (NULL, ":", &saveptr);
  }

  free (params_copy);
  params_copy = NULL;

  if (!result->hw_mode) {
    if (result->configure_scale0 || result->enable_out1 || result->enable_out2) {
      GST_ERROR ("out0/out1/out2 can not be enable in sw frame mode\n");
    }
    ret = FALSE;
    goto EXIT;
  }

  if (result->configure_scale0) {
    if (result->w0 <= 0 || result->h0 <= 0) {
      GST_ERROR ("scale0 must be set to valid value when configure scale0\n");
      ret = FALSE;
      goto EXIT;
    }
  }

  if (result->enable_out1 == 1) {
    if (result->w1 <= 0 || result->h1 <= 0) {
      GST_ERROR ("scale1 must be set to valid value when enable out1\n");
      ret = FALSE;
      goto EXIT;
    }
  }

  if (result->enable_out2 == 1) {
    if (result->w2 <= 0 || result->h2 <= 0) {
      GST_ERROR ("scale2 must be set to valid value when enable out2\n");
      ret = FALSE;
      goto EXIT;
    }
  }

EXIT:
  if (params_copy) {
    free (params_copy);
  }
  return ret;
}

static gboolean
gst_niquadradec_configure_ppu_params (GstNiquadraDec * nidec,
    xcoder_params_t * result)
{
  int ret = 0;
  ni_session_context_t *p_ctx =
      gst_niquadra_context_get_session_context (nidec->context);
  ni_xcoder_params_t *p_param =
      gst_niquadra_context_get_xcoder_param (nidec->context);
  ni_decoder_input_params_t *p_dec_input_param = &(p_param->dec_input_params);

  if (!result->hw_mode) {
    GST_ERROR_OBJECT (nidec, "Don't support to reconfig ppu in SW frame mode");
    return FALSE;
  }

  if (p_dec_input_param->mcmode) {
    GST_ERROR_OBJECT (nidec,
        "Don't support to reconfig ppu when enable MulticoreJointMode");
    return FALSE;
  }

  if ((nidec->hardware_mode && !result->hw_mode) ||
      (!nidec->hardware_mode && result->hw_mode)) {
    GST_ERROR_OBJECT (nidec,
        "Don't support to switch HW/SW frame mode dynamically");
    return FALSE;
  }

  if ((!nidec->enable_out1 && result->enable_out1) ||
      (nidec->enable_out1 && !result->enable_out1)) {
    GST_ERROR_OBJECT (nidec,
        "Don't support to disable or enable enable_out1 dynamically");
    return FALSE;
  }

  if ((!nidec->enable_out2 && result->enable_out2) ||
      (nidec->enable_out2 && !result->enable_out2)) {
    GST_ERROR_OBJECT (nidec,
        "Don't support to disable or enable enable_out2 dynamically");
    return FALSE;
  }

  if (!result->configure_scale0 && !result->enable_out1 && !result->enable_out2) {
    GST_DEBUG_OBJECT (nidec,
        "scale0/scale1/scale2 are not changed, so no need to reconfig ppu");
    return TRUE;
  }

  if ((result->w0 != nidec->out_width) ||
      (result->h0 != nidec->out_height) ||
      (result->w1 != nidec->out1_width) ||
      (result->h1 != nidec->out1_height) ||
      (result->w2 != nidec->out2_width) || (result->h2 != nidec->out2_height)) {
    ni_ppu_config_t ppu_config = { 0 };
    if (result->configure_scale0) {
      ppu_config.ppu_set_enable = 0x01;
      ppu_config.ppu_w[0] = result->w0;
      ppu_config.ppu_h[0] = result->h0;
    } else {
      ppu_config.ppu_set_enable = 0x01;
      ppu_config.ppu_w[0] = nidec->out_width;
      ppu_config.ppu_h[0] = nidec->out_height;
    }

    if (result->enable_out1) {
      ppu_config.ppu_set_enable += (0x01 << 1);
      ppu_config.ppu_w[1] = result->w1;
      ppu_config.ppu_h[1] = result->h1;
    }

    if (result->enable_out2) {
      ppu_config.ppu_set_enable += (0x01 << 2);
      ppu_config.ppu_w[2] = result->w2;
      ppu_config.ppu_h[2] = result->h2;
    }

    ret = ni_dec_reconfig_ppu_params (p_ctx, p_param, &ppu_config);
    if (ret < 0) {
      GST_ERROR_OBJECT (nidec, "Failed to reconfig ppu:%d", ret);
      return FALSE;
    }
  }

  return TRUE;
}

gboolean
gst_niquadradec_set_common_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiquadraDec *nidec = GST_NIQUADRADEC (object);
  GstState state;
  xcoder_params_t xparam = { 0 };
  gboolean ret = TRUE;

  GST_OBJECT_LOCK (nidec);
  state = GST_STATE (nidec);
  if ((state != GST_STATE_READY && state != GST_STATE_NULL) &&
      !(pspec->flags & GST_PARAM_MUTABLE_PLAYING)) {
    ret = FALSE;
    goto wrong_state;
  }

  switch (prop_id) {
    case GST_NIQUADRA_DEC_PROP_NAME:
      g_free (nidec->dev_xcoder_name);
      nidec->dev_xcoder_name = g_strdup (g_value_get_string (value));
      break;
    case GST_NIQUADRA_DEC_PROP_CARD_NUM:
      nidec->dev_dec_idx = g_value_get_int (value);
      break;
    case GST_NIQUADRA_DEC_PROP_TIMEOUT:
      nidec->keep_alive_timeout = g_value_get_uint (value);
      break;
    case GST_NIQUADRA_DEC_PROP_XCODER_PARAMS:
      g_free (nidec->xcoder_opts);
      nidec->xcoder_opts = g_strdup (g_value_get_string (value));
      if (nidec->opened) {
        if (!parse_xcoder_params (nidec->xcoder_opts, &xparam)) {
          ret = FALSE;
          goto wrong_state;
        }
        if (!gst_niquadradec_configure_ppu_params (nidec, &xparam)) {
          ret = FALSE;
          goto wrong_state;
        }
      }
      break;
    default:
      ret = FALSE;
      break;
  }
  GST_OBJECT_UNLOCK (nidec);
  return ret;

wrong_state:
  {
    GST_WARNING_OBJECT (nidec, "setting property in wrong state");
    GST_OBJECT_UNLOCK (nidec);
    return ret;
  }
}

gboolean
gst_niquadradec_get_common_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiquadraDec *nidec = GST_NIQUADRADEC (object);
  gboolean ret = TRUE;

  GST_OBJECT_LOCK (nidec);

  switch (prop_id) {
    case GST_NIQUADRA_DEC_PROP_NAME:
      g_value_set_string (value, nidec->dev_xcoder_name);
      break;
    case GST_NIQUADRA_DEC_PROP_CARD_NUM:
      g_value_set_int (value, nidec->dev_dec_idx);
      break;
    case GST_NIQUADRA_DEC_PROP_TIMEOUT:
      g_value_set_uint (value, nidec->keep_alive_timeout);
      break;
    case GST_NIQUADRA_DEC_PROP_XCODER_PARAMS:
      g_value_set_string (value, nidec->xcoder_opts);
      break;
    default:
      ret = FALSE;
      break;
  }

  GST_OBJECT_UNLOCK (nidec);
  return ret;
}

void
gst_niquadradec_install_common_properties (GstNiquadraDecClass * decoder_class)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (decoder_class);
  GParamSpec *obj_properties[GST_NIQUADRA_DEC_PROP_MAX] = { NULL, };

  obj_properties[GST_NIQUADRA_DEC_PROP_CARD_NUM] =
      g_param_spec_int ("dec", "Dec",
      "Select which decoder to use by index. First is 0, second is 1, and so on",
      -1, INT_MAX, PROP_DEC_INDEX, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NIQUADRA_DEC_PROP_NAME] =
      g_param_spec_string ("device-name", "Device-Name",
      "Select which decoder to use by NVMe block device name, e.g. /dev/nvme0n1",
      NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NIQUADRA_DEC_PROP_XCODER_PARAMS] =
      g_param_spec_string ("xcoder-params", "XCODER-PARAMS",
      "Set the XCoder configuration using a :-separated list of key=value parameters",
      NULL,
      G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NIQUADRA_DEC_PROP_TIMEOUT] =
      g_param_spec_uint ("keep-alive-timeout", "TIMEOUT",
      "Specify a custom session keep alive timeout in seconds.",
      NI_MIN_KEEP_ALIVE_TIMEOUT, NI_MAX_KEEP_ALIVE_TIMEOUT,
      NI_DEFAULT_KEEP_ALIVE_TIMEOUT,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, GST_NIQUADRA_DEC_PROP_MAX,
      obj_properties);
}
