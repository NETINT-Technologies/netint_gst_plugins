/*******************************************************************************
 *
 * Copyright (C) 2024 NETINT Technologies
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
 *  \file   gstniloganenc.c
 *
 *  \brief  Implement of NetInt Logan common encoder.
 ******************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <glib/gstdio.h>

#include <gst/gst.h>
#include <gst/video/video.h>
#include <ni_util_logan.h>
#include <ni_av_codec_logan.h>
#include "gstniloganenc.h"
//#include "gstniloganhwframe.h"
#include "gstnimetadata.h"

#define GST_NILOGAN_CAPS_MAKE(format) \
  GST_VIDEO_CAPS_MAKE (format) ", " \
  "interlace-mode = (string) progressive"
#define GST_NILOGAN_CAPS_MAKE_WITH_DMABUF_FEATURE(dmaformat) ""

#define GST_NILOGAN_CAPS_STR(format,dmaformat) \
  GST_NILOGAN_CAPS_MAKE (format) "; " \
  GST_NILOGAN_CAPS_MAKE_WITH_DMABUF_FEATURE (dmaformat)

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_NILOGAN_CAPS_STR ("yuv420", "yuv420"))
    );

#define PROP_ENC_INDEX 0
#define PROP_ENC_NAME "/dev/nvme0n1"
#define PROP_ENC_TIMEOUT 3
#define PROP_ENC_XCODER "bestmodelload"

#define gst_niloganenc_parent_class parent_class
G_DEFINE_TYPE (GstNiloganEnc, gst_niloganenc, GST_TYPE_VIDEO_ENCODER);

static gint xcoder_send_frame (GstNiloganEnc * enc, GstVideoCodecFrame * frame);

static const uint8_t *
find_start_code (const uint8_t * p, const uint8_t * end, uint32_t * state)
{
  int i;

  assert (p <= end);
  if (p >= end)
    return end;

  for (i = 0; i < 3; i++) {
    uint32_t tmp = *state << 8;
    *state = tmp + *(p++);
    if (tmp == 0x100 || p == end)
      return p;
  }

  while (p < end) {
    if (p[-1] > 1)
      p += 3;
    else if (p[-2])
      p += 2;
    else if (p[-3] | (p[-1] - 1))
      p++;
    else {
      p++;
      break;
    }
  }

  p = MIN (p, end) - 4;
  *state = GST_READ_UINT32_LE (p);

  return p + 4;
}

static GstVideoCodecFrame *
gst_niloganenc_find_best_frame (GstNiloganEnc * enc, GList * frames,
    GstClockTime pts)
{
  GList *iter;
  GstVideoCodecFrame *ret = NULL;
  for (iter = frames; iter; iter = g_list_next (iter)) {
    GstVideoCodecFrame *frame = (GstVideoCodecFrame *) iter->data;

    if (frame->pts == pts) {
      ret = frame;
      break;
    }
  }

  if (ret) {
    ret = gst_video_codec_frame_ref (ret);
  }

  return ret;
}

static void
gst_niloganenc_set_context (GstElement * element, GstContext * context)
{
  GST_ELEMENT_CLASS (parent_class)->set_context (element, context);
}

static void
gst_niloganenc_dispose (GObject * object)
{
  GstNiloganEnc *thiz = GST_NILOGANENC (object);

  if (thiz->input_state)
    gst_video_codec_state_unref (thiz->input_state);
  thiz->input_state = NULL;

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

/*
 * Map to nienc.c "xcoder_logan_setup_encoder"
 * */
static gboolean
gst_niloganenc_open_xcoder (GstNiloganEnc * nienc, GstVideoCodecState * state)
{
  GstNiloganEncClass *oclass = (GstNiloganEncClass *)
      G_OBJECT_GET_CLASS (nienc);
  ni_logan_session_context_t *p_ctx =
      gst_nilogan_context_get_session_context (nienc->context);
  ni_logan_encoder_params_t *p_enc_params =
      gst_nilogan_context_get_xcoder_enc_param (nienc->context);
  ni_logan_session_data_io_t *p_pkt =
      gst_nilogan_context_get_data_pkt (nienc->context);
  ni_logan_session_data_io_t *p_frame =
      gst_nilogan_context_get_data_frame (nienc->context);

  GST_DEBUG ("XCoder encode init\n");
  gint ret = 0;

  ni_logan_encoder_params_t *pparams = NULL;
  int is_bigendian = NI_LOGAN_FRAME_LITTLE_ENDIAN;
  //ni_logan_session_run_state_t prev_state = p_ctx->session_run_state;
  // setup the encoder type such as h264/h265/av1
  oclass->configure (nienc);
  nienc->roi_data = NULL;
  nienc->firstPktArrived = 0;
  nienc->spsPpsArrived = 0;
  nienc->spsPpsHdrLen = 0;
  nienc->p_spsPpsHdr = NULL;
  nienc->reconfigCount = 0;
  nienc->latest_dts = 0;
  nienc->first_frame_pts = INT_MIN;

  if (LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING != p_ctx->session_run_state) {
    GST_DEBUG ("Session state: %d allocate frame fifo.\n",
        p_ctx->session_run_state);
  } else {
    GST_DEBUG ("Session seq change, fifo size:\n");
  }
  nienc->eos_fme_received = 0;

  ni_logan_pix_fmt_t ni_format;
  switch (state->info.finfo->format) {
    case GST_VIDEO_FORMAT_I420:
      ni_format = NI_LOGAN_PIX_FMT_YUV420P;
      break;
    case GST_VIDEO_FORMAT_I420_10LE:
      ni_format = NI_LOGAN_PIX_FMT_YUV420P10LE;
      break;
    case GST_VIDEO_FORMAT_I420_10BE:
      ni_format = NI_LOGAN_PIX_FMT_YUV420P10LE;
      is_bigendian = NI_LOGAN_FRAME_BIG_ENDIAN;
      break;
    default:
      GST_ERROR ("Ni enc don't support [%d]", state->info.finfo->format);
      return FALSE;
  }
  p_ctx->codec_format = nienc->codec_format;
  // Xcoder user Configuration
  GST_DEBUG ("fps:%d/%d,res=%dx%d,format=%d,p=%p\n", state->info.fps_n,
      state->info.fps_d, state->info.width, state->info.height, ni_format,
      p_enc_params);
  ret =
      ni_logan_encoder_init_default_params (p_enc_params, state->info.fps_n,
      state->info.fps_d, 200000, NI_LOGAN_ODD2EVEN (state->info.width),
      NI_LOGAN_ODD2EVEN (state->info.height));
  switch (ret) {
    case NI_LOGAN_RETCODE_PARAM_ERROR_WIDTH_TOO_BIG:
      GST_ERROR ("Invalid Picture Width: too big\n");
      return FALSE;
    case NI_LOGAN_RETCODE_PARAM_ERROR_WIDTH_TOO_SMALL:
      GST_ERROR ("Invalid Picture Width: too small\n");
      return FALSE;
    case NI_LOGAN_RETCODE_PARAM_ERROR_HEIGHT_TOO_BIG:
      GST_ERROR ("Invalid Picture Height: too big\n");
      return FALSE;
    case NI_LOGAN_RETCODE_PARAM_ERROR_HEIGHT_TOO_SMALL:
      GST_ERROR ("Invalid Picture Height: too small\n");
      return FALSE;
    case NI_LOGAN_RETCODE_PARAM_ERROR_AREA_TOO_BIG:
      GST_ERROR ("Invalid Picture Width x Height: exceeds %d\n",
          NI_LOGAN_MAX_RESOLUTION_AREA);
      return FALSE;
    case NI_LOGAN_RETCODE_PARAM_ERROR_PIC_WIDTH:
      GST_ERROR ("Invalid Picture Width\n");
      return FALSE;
    case NI_LOGAN_RETCODE_PARAM_ERROR_PIC_HEIGHT:
      GST_ERROR ("Invalid Picture Height\n");
      return FALSE;
    default:
      if (ret < 0) {
        int i;
        GST_ERROR ("Error setting preset or log.\n");
        GST_LOG ("Possible presets:");
        for (i = 0; g_logan_xcoder_preset_names[i]; i++)
          GST_LOG (" %s", g_logan_xcoder_preset_names[i]);
        GST_LOG ("\n");

        GST_INFO ("Possible log:");
        for (i = 0; g_logan_xcoder_log_names[i]; i++)
          GST_INFO (" %s", g_logan_xcoder_log_names[i]);
        GST_INFO ("\n");

        return FALSE;
      }
      break;
  }

  if (nienc->hardware_mode && nienc->width >= NI_LOGAN_MIN_WIDTH
      && nienc->height >= NI_LOGAN_MIN_HEIGHT) {
    p_enc_params->hwframes = 1;
  }

  /* set pixel_format */
  switch (nienc->pix_fmt) {
    case GST_VIDEO_FORMAT_I420:
      p_ctx->pixel_format = NI_LOGAN_PIX_FMT_YUV420P;
      break;
    case GST_VIDEO_FORMAT_I420_10LE:
      p_ctx->pixel_format = NI_LOGAN_PIX_FMT_YUV420P10LE;
      break;
    default:
      break;
  }

  if (nienc->xcoder_opts) {
    // check and set ni_encoder_params from --xcoder-params
    GST_DEBUG ("Xcoder params:%s\n", nienc->xcoder_opts);
    if (gst_ni_retrieve_xcoder_params (p_ctx, nienc->xcoder_opts, p_enc_params)) {
      GST_ERROR ("Error: encoder p_config parsing error\n");
      return FALSE;
    }
  }

  if (p_enc_params->enable_vfr) {
    p_enc_params->enc_input_params.frame_rate = 30;
    p_ctx->prev_fps = 30;
    p_ctx->last_change_framenum = 0;
    p_ctx->fps_change_detect_count = 0;
  }

  if (nienc->xcoder_gop) {
    if (gst_ni_retrieve_gop_params (nienc->xcoder_gop, p_enc_params)) {
      GST_ERROR ("Error: encoder p_config parsing error\n");
      return FALSE;
    }
  }
  p_ctx->p_session_config = p_enc_params;
  pparams = (ni_logan_encoder_params_t *) p_ctx->p_session_config;

  switch (pparams->enc_input_params.gop_preset_index) {
      /* dts_offset is the max number of non-reference frames in a GOP
       * (derived from x264/5 algo) In case of IBBBP the first dts of the I frame should be input_pts-(3*ticks_per_frame)
       * In case of IBP the first dts of the I frame should be input_pts-(1*ticks_per_frame)
       * thus we ensure pts>dts in all cases
       * */
    case 1 /*PRESET_IDX_ALL_I */ :
    case 2 /*PRESET_IDX_IPP */ :
    case 6 /*PRESET_IDX_IPPPP */ :
    case 9 /*PRESET_IDX_SP */ :
      nienc->dts_offset = 0;
      break;
      /* ts requires dts/pts of I frame not same when there are B frames in streams */
    case 3 /*PRESET_IDX_IBBB */ :
    case 4 /*PRESET_IDX_IBPBP */ :
    case 7 /*PRESET_IDX_IBBBB */ :
      nienc->dts_offset = 1;
      break;
    case 5 /*PRESET_IDX_IBBBP */ :
      nienc->dts_offset = 2;
      break;
    case 8 /*PRESET_IDX_RA_IB */ :
      nienc->dts_offset = 3;
      break;
    default:
      nienc->dts_offset = 7;
      GST_DEBUG ("dts offset default to 7\n");
      break;
  }

  nienc->total_frames_received = 0;
  nienc->gop_offset_count = 0;

  GST_DEBUG ("dts offset: %ld, gop_offset_count: %d, device_name=%s\n",
      nienc->dts_offset, nienc->gop_offset_count, nienc->dev_xcoder_name);

  if (0 == strcmp (nienc->dev_xcoder, LIST_DEVICES_STR)) {
    GST_ERROR ("XCoder: printing out all xcoder devices and "
        "their load, and exit ...\n");
    ni_logan_rsrc_print_all_devices_capability ();
    return GST_FLOW_ERROR;
  }
  //overwrite the nvme io size here with a custom value if it was provided
//  if (nienc->nvme_io_size > 0 && nienc->nvme_io_size % 4096 != 0) {
//    p_ctx->max_nvme_io_size = nienc->nvme_io_size;
//  }

  nienc->encoder_eof = 0;
  nienc->bit_rate = p_enc_params->bitrate;
  p_ctx->src_bit_depth = 8;
  p_ctx->src_endian = NI_LOGAN_FRAME_LITTLE_ENDIAN;
  p_ctx->roi_len = 0;
  p_ctx->roi_avg_qp = 0;
  p_ctx->bit_depth_factor = 1;
  if (p_ctx->keep_alive_timeout != NI_LOGAN_DEFAULT_KEEP_ALIVE_TIMEOUT) {
    GST_DEBUG ("Default:%d, custom=%d", NI_LOGAN_DEFAULT_KEEP_ALIVE_TIMEOUT,
        nienc->keep_alive_timeout);
    p_ctx->keep_alive_timeout = nienc->keep_alive_timeout;
  }
  //overwrite set_high_priority value here with a custom value if it was provided
  uint32_t xcoder_high_priority =
      p_enc_params->enc_input_params.set_high_priority;
  if (xcoder_high_priority != 0) {
    p_ctx->set_high_priority = xcoder_high_priority;
  } else {
    p_ctx->set_high_priority = nienc->set_high_priority;
  }
  p_ctx->set_high_priority = 1;
  GST_DEBUG ("Custom NVMe set_high_priority set to = %d\n",
      p_ctx->set_high_priority);

  nienc->total_frames_received = 0;
  nienc->encoder_eof = 0;
  nienc->roi_side_data_size = nienc->nb_rois = 0;
//  nienc->av_rois = NULL;
//  s->avc_roi_map = NULL;
//  s->hevc_sub_ctu_roi_buf = NULL;
//  s->hevc_roi_map = NULL;
  p_ctx->src_bit_depth = 8;
  p_ctx->src_endian = NI_LOGAN_FRAME_LITTLE_ENDIAN;
  p_ctx->roi_len = 0;
  p_ctx->roi_avg_qp = 0;
  p_ctx->bit_depth_factor = 1;

  if (ni_format == NI_LOGAN_PIX_FMT_YUV420P10LE) {
    p_ctx->bit_depth_factor = 2;
    p_ctx->src_bit_depth = 10;
    p_ctx->src_endian = is_bigendian;
  }

  memset (&p_pkt->data.packet, 0, sizeof (ni_logan_session_data_io_t));

  // init HDR SEI stuff
//  p_ctx->sei_hdr_content_light_level_info_len = p_ctx->light_level_data_len
//      = p_ctx->sei_hdr_mastering_display_color_vol_len =
//      p_ctx->mdcv_max_min_lum_data_len = 0;
//  p_ctx->p_master_display_meta_data = NULL;
//
//  p_ctx->active_video_width = state->info.width;
//  p_ctx->active_video_height = state->info.height;
//  p_ctx->src_bit_depth = p_ctx->bit_depth_factor;
//  //p_ctx->ori_pix_fmt = ni_format;
//  p_ctx->pixel_format = 0;

  nienc->width = state->info.width;
  nienc->height = state->info.height;

  if (nienc->width > 0 && nienc->height > 0) {
    GST_DEBUG ("ni_logan_frame_buffer_alloc: %dx%d\n",
        NI_LOGAN_ODD2EVEN (nienc->width), NI_LOGAN_ODD2EVEN (nienc->height));
    ni_logan_frame_buffer_alloc (&(p_frame->data.frame), NI_ALIGN (nienc->width,
            32), NI_ALIGN (nienc->height, 16), 0, 0, p_ctx->bit_depth_factor,
        nienc->hardware_mode);
  }

  return TRUE;
}

static void
gst_niloganenc_videoinfo_to_context (GstVideoInfo * info, GstNiloganEnc * nienc)
{
  gint i, bpp = 0;

  nienc->width = GST_VIDEO_INFO_WIDTH (info);
  nienc->height = GST_VIDEO_INFO_HEIGHT (info);
  for (i = 0; i < GST_VIDEO_INFO_N_COMPONENTS (info); i++)
    bpp += GST_VIDEO_INFO_COMP_DEPTH (info, i);
  nienc->bits_per_coded_sample = bpp;

  nienc->ticks_per_frame = 1;
  if (GST_VIDEO_INFO_FPS_N (info) == 0) {
    GST_DEBUG ("Using 25/1 framerate");
    nienc->time_base_den = 25;
    nienc->time_base_num = 1;
    nienc->fps_den = 1;
    nienc->fps_num = 25;
  } else {
    nienc->time_base_den = GST_VIDEO_INFO_FPS_N (info);
    nienc->time_base_num = GST_VIDEO_INFO_FPS_D (info);
    nienc->fps_den = GST_VIDEO_INFO_FPS_D (info);
    nienc->fps_num = GST_VIDEO_INFO_FPS_N (info);
  }

  nienc->sample_aspect_num = GST_VIDEO_INFO_PAR_N (info);
  nienc->sample_aspect_den = GST_VIDEO_INFO_PAR_D (info);
  nienc->pix_fmt = info->finfo->format;
  nienc->chroma_site = info->chroma_site;
  nienc->colorimetry = info->colorimetry;
}

static GstCaps *
gst_niloganenc_caps_new (GstNiloganEnc * nienc, const char *mimetype,
    const char *fieldname, ...)
{
  GstCaps *caps = NULL;
  va_list var_args;
  gint num, denom;
  caps = gst_caps_new_simple (mimetype, "width", G_TYPE_INT, nienc->width,
      "height", G_TYPE_INT, nienc->height, NULL);
  num = nienc->fps_num;
  denom = nienc->fps_den;

  if (!denom) {
    GST_LOG ("invalid framerate: %d/0, -> %d/1", num, num);
    denom = 1;
  }
  if (gst_util_fraction_compare (num, denom, 1000, 1) > 0) {
    GST_LOG ("excessive framerate: %d/%d, -> 0/1", num, denom);
    num = 0;
    denom = 1;
  }
  GST_LOG ("setting framerate: %d/%d", num, denom);
  gst_caps_set_simple (caps, "framerate", GST_TYPE_FRACTION, num, denom, NULL);
  if (!caps) {
    GST_DEBUG ("Creating default caps");
    caps = gst_caps_new_empty_simple (mimetype);
  }

  va_start (var_args, fieldname);
  gst_caps_set_simple_valist (caps, fieldname, var_args);
  va_end (var_args);
  return caps;
}

/*
 * Map to nienc.c "xcoder_encode_close"
 * */
static gint
gst_niloganenc_close (GstNiloganEnc * nienc)
{
  if (!nienc->context) {
    nienc->started = 0;
    return 0;
  }

  ni_logan_session_context_t *p_ctx =
      gst_nilogan_context_get_session_context (nienc->context);
  ni_logan_session_data_io_t *p_pkt =
      gst_nilogan_context_get_data_pkt (nienc->context);
  ni_logan_session_data_io_t *p_frame =
      gst_nilogan_context_get_data_frame (nienc->context);

  //gst_niloganenc_clear_all_frames (nienc);

  if (nienc->started || p_ctx->device_handle != NI_INVALID_DEVICE_HANDLE ||
      p_ctx->blk_io_handle != NI_INVALID_DEVICE_HANDLE) {
    ni_logan_retcode_t ret;
    ret =
        ni_logan_device_session_close (p_ctx, nienc->encoder_eof,
        NI_LOGAN_DEVICE_TYPE_ENCODER);
    if (NI_LOGAN_RETCODE_SUCCESS != ret) {
      GST_ERROR ("Failed to close Encoder Session(status=%d)\n", ret);
    }
  }

  if (p_ctx->device_handle != NI_INVALID_DEVICE_HANDLE) {
    ni_logan_device_close (p_ctx->device_handle);
  }
  if (p_ctx->blk_io_handle != NI_INVALID_DEVICE_HANDLE) {
    ni_logan_device_close (p_ctx->blk_io_handle);
  }

  GST_DEBUG ("XCoder encode close: session_run_state %d\n",
      p_ctx->session_run_state);

  p_ctx->device_handle = NI_INVALID_DEVICE_HANDLE;
  p_ctx->blk_io_handle = NI_INVALID_DEVICE_HANDLE;
  p_ctx->auto_dl_handle = NI_INVALID_DEVICE_HANDLE;
  p_ctx->sender_handle = NI_INVALID_DEVICE_HANDLE;

  ni_logan_frame_buffer_free (&(p_frame->data.frame));
  ni_logan_packet_buffer_free (&(p_pkt->data.packet));


  if (nienc->p_spsPpsHdr) {
    free (nienc->p_spsPpsHdr);
    nienc->p_spsPpsHdr = NULL;
  }
//  if (p_ctx->session_run_state != LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING) {
//    gst_object_unref (nienc->context);
//  }
  gst_object_unref (nienc->context);
  nienc->context = NULL;
  nienc->started = 0;
  return 0;
}

/*
 * For some elements (eg. fpsdisplay) the downstream element would try to lookup the caps from
 * upstream elements, by default function would not accept the caps, we need to implement getcaps
 * function and select the allowed caps. (Copyed from gstx264enc.c)
 * */
static GstCaps *
gst_niloganenc_sink_getcaps (GstVideoEncoder * enc, GstCaps * filter)
{
  GstCaps *supported_incaps;
  GstCaps *allowed;
  GstCaps *filter_caps, *fcaps;
  gint i, j;

  supported_incaps =
      gst_pad_get_pad_template_caps (GST_VIDEO_ENCODER_SINK_PAD (enc));

  /* Allow downstream to specify width/height/framerate/PAR constraints
   * and forward them upstream for video converters to handle
   */
  allowed = gst_pad_get_allowed_caps (enc->srcpad);

  if (!allowed || gst_caps_is_empty (allowed) || gst_caps_is_any (allowed)) {
    fcaps = supported_incaps;
    goto done;
  }

  GST_LOG_OBJECT (enc, "template caps %" GST_PTR_FORMAT, supported_incaps);
  GST_LOG_OBJECT (enc, "allowed caps %" GST_PTR_FORMAT, allowed);

  filter_caps = gst_caps_new_empty ();

  for (i = 0; i < gst_caps_get_size (supported_incaps); i++) {
    GQuark q_name =
        gst_structure_get_name_id (gst_caps_get_structure (supported_incaps,
            i));

    for (j = 0; j < gst_caps_get_size (allowed); j++) {
      const GstStructure *allowed_s = gst_caps_get_structure (allowed, j);
      const GValue *val;
      GstStructure *s;

      /* FIXME Find a way to reuse gst_video_encoder_proxy_getcaps so that
       * we do not need to copy that logic */
      s = gst_structure_new_id_empty (q_name);
      if ((val = gst_structure_get_value (allowed_s, "width")))
        gst_structure_set_value (s, "width", val);
      if ((val = gst_structure_get_value (allowed_s, "height")))
        gst_structure_set_value (s, "height", val);
      if ((val = gst_structure_get_value (allowed_s, "framerate")))
        gst_structure_set_value (s, "framerate", val);
      if ((val = gst_structure_get_value (allowed_s, "pixel-aspect-ratio")))
        gst_structure_set_value (s, "pixel-aspect-ratio", val);
      if ((val = gst_structure_get_value (allowed_s, "colorimetry")))
        gst_structure_set_value (s, "colorimetry", val);
      if ((val = gst_structure_get_value (allowed_s, "chroma-site")))
        gst_structure_set_value (s, "chroma-site", val);

      filter_caps = gst_caps_merge_structure (filter_caps, s);
    }
  }

  fcaps = gst_caps_intersect (filter_caps, supported_incaps);
  gst_caps_unref (filter_caps);
  gst_caps_unref (supported_incaps);

  if (filter) {
    GST_LOG_OBJECT (enc, "intersecting with %" GST_PTR_FORMAT, filter);
    filter_caps = gst_caps_intersect (fcaps, filter);
    gst_caps_unref (fcaps);
    fcaps = filter_caps;
  }

done:
  gst_caps_replace (&allowed, NULL);

  GST_LOG_OBJECT (enc, "proxy caps %" GST_PTR_FORMAT, fcaps);

  return fcaps;
}

static GstCaps *
gst_niloganenc_get_caps (GstVideoEncoder * encoder)
{
  GstNiloganEnc *nienc = (GstNiloganEnc *) encoder;
  GstCaps *caps = NULL;
  switch (nienc->codec_format) {
    case NI_LOGAN_CODEC_FORMAT_H264:
      caps = gst_niloganenc_caps_new (nienc, "video/x-h264",
          "alignment", G_TYPE_STRING, "au", NULL);
      gst_caps_set_simple (caps, "stream-format", G_TYPE_STRING,
          "byte-stream", NULL);
      break;
    case NI_LOGAN_CODEC_FORMAT_H265:
      caps = gst_niloganenc_caps_new (nienc, "video/x-h265",
          "alignment", G_TYPE_STRING, "au", NULL);
      gst_caps_set_simple (caps, "stream-format", G_TYPE_STRING,
          "byte-stream", NULL);
      break;
  }

  return caps;
}

/*gstreamer set the caps by set_format function*/
static gboolean
gst_niloganenc_set_format (GstVideoEncoder * encoder,
    GstVideoCodecState * state)
{
  GstCaps *other_caps;
  GstCaps *allowed_caps;
  GstCaps *icaps;
  GstVideoCodecState *output_format;
  GstNiloganEnc *nienc = (GstNiloganEnc *) encoder;

  // NI encoder don't support interlaced encoding now.
  if (GST_VIDEO_INFO_IS_INTERLACED (&state->info))
    return FALSE;

  if (nienc->started) {
    GST_DEBUG ("Format may changed when encoding process,reset here\n");
    if (nienc->input_state)
      gst_video_codec_state_unref (nienc->input_state);
    nienc->input_state = gst_video_codec_state_ref (state);
    return TRUE;
  }

  /* Set loglevel for libxcoder */
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

  GST_DEBUG_OBJECT (encoder, "Extracting common video information");
  /* fetch pix_fmt, fps, par, width, height... */
  gst_niloganenc_videoinfo_to_context (&state->info, nienc);

  /* sanitize time base */
  if (nienc->time_base_num <= 0 || nienc->time_base_den <= 0)
    goto insane_timebase;

  /* 1. Get allowed caps */
  GST_DEBUG_OBJECT (encoder, "picking an output format ...");
  allowed_caps = gst_pad_get_allowed_caps (GST_VIDEO_ENCODER_SRC_PAD (encoder));
  if (!allowed_caps) {
    GST_DEBUG_OBJECT (encoder, "... but no peer, using template caps");
    /* we need to copy because get_allowed_caps returns a ref, and
     * get_pad_template_caps doesn't */
    allowed_caps =
        gst_pad_get_pad_template_caps (GST_VIDEO_ENCODER_SRC_PAD (encoder));
  }
  GST_DEBUG_OBJECT (nienc, "chose caps %" GST_PTR_FORMAT, allowed_caps);

  /*Open xcoder */
  if (!gst_niloganenc_open_xcoder (nienc, state)) {
    goto OPEN_ERR;
  }

  GstClockTime min_pts = GST_SECOND * 60 * 60 * 1000;
  gst_video_encoder_set_min_pts (encoder, min_pts);

  other_caps = gst_niloganenc_get_caps (encoder);

  GST_DEBUG_OBJECT (nienc, "chose other caps %" GST_PTR_FORMAT, other_caps);
  if (!other_caps) {
    gst_caps_unref (allowed_caps);
    goto unsupported_codec;
  }

  icaps = gst_caps_intersect (allowed_caps, other_caps);
  gst_caps_unref (allowed_caps);
  gst_caps_unref (other_caps);
  if (gst_caps_is_empty (icaps)) {
    gst_caps_unref (icaps);
    goto unsupported_codec;
  }
  icaps = gst_caps_fixate (icaps);

  if (nienc->input_state)
    gst_video_codec_state_unref (nienc->input_state);
  nienc->input_state = gst_video_codec_state_ref (state);

  GST_DEBUG_OBJECT (nienc, "chose caps %" GST_PTR_FORMAT, icaps);

  output_format = gst_video_encoder_set_output_state (encoder, icaps, state);
  gst_video_codec_state_unref (output_format);

  return TRUE;


insane_timebase:
  {
    GST_ERROR_OBJECT (encoder, "Rejecting time base %d/%d",
        nienc->time_base_den, nienc->time_base_num);
    gst_niloganenc_close (nienc);
    return FALSE;
  }
unsupported_codec:
  {
    GST_DEBUG ("Unsupported codec - no caps found");
    gst_niloganenc_close (nienc);
    return FALSE;
  };
OPEN_ERR:
  {
    GST_ERROR ("Open codec error\n");
    gst_niloganenc_close (nienc);
    return FALSE;
  };
}

static GstFlowReturn
gst_niloganenc_start (GstVideoEncoder * encoder)
{
  GstNiloganEnc *nienc = (GstNiloganEnc *) encoder;

  GST_DEBUG ("Start\n");
  nienc->context = gst_nilogan_context_new ();

  return TRUE;
}

static gboolean
gst_niloganenc_flush (GstVideoEncoder * encoder)
{
  return TRUE;
}

static gboolean
gst_niloganenc_propose_allocation (GstVideoEncoder * encoder, GstQuery * query)
{
  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);

  return GST_VIDEO_ENCODER_CLASS (parent_class)->propose_allocation (encoder,
      query);
}

static inline void
gst_xcoder_strncpy (char *dst, const char *src, int max)
{
  if (dst && src && max) {
    *dst = '\0';
    strncpy (dst, src, max);
    *(dst + max - 1) = '\0';
  }
}

/*
 * Map to nienc.c "xcoder_encode_reset"
 * */
static int
xcoder_encode_reset (GstNiloganEnc * nienc)
{
  GST_DEBUG ("XCoder encode reset\n");
  gst_niloganenc_close (nienc);
  nienc->context = gst_nilogan_context_new ();
  return gst_niloganenc_open_xcoder (nienc, nienc->input_state);
}

/*
 * Map to nienc.c "xcoder_logan_encode_reinit"
 * */
static int
xcoder_encode_reinit (GstNiloganEnc * enc)
{
  int ret = 0;
  GST_DEBUG ("xcoder reinit\n");
  ni_logan_session_context_t *p_ctx =
      gst_nilogan_context_get_session_context (enc->context);
  ni_logan_session_run_state_t prev_state = p_ctx->session_run_state;

  enc->eos_fme_received = 0;
  enc->encoder_eof = 0;
  enc->encoder_flushing = 0;

  if (p_ctx->pts_table && p_ctx->dts_queue) {
    gst_niloganenc_close (enc);
    p_ctx->session_run_state = prev_state;
    enc->context = gst_nilogan_context_new ();
  }

  enc->started = 0;
  enc->firstPktArrived = 0;
  enc->spsPpsArrived = 0;
  enc->spsPpsHdrLen = 0;
  enc->p_spsPpsHdr = NULL;

//  GstVideoCodecFrame *current_send_frame;
//
//  GList *item = g_list_first (enc->frame_list);
//  current_send_frame = item->data;
//  enc->frame_list = g_list_delete_link(enc->frame_list, item);

  gst_niloganenc_open_xcoder (enc, enc->input_state);

  while (ret == 0 && g_list_length (enc->frame_list)) {
    p_ctx->session_run_state = LOGAN_SESSION_RUN_STATE_QUEUED_FRAME_DRAINING;
    ret = xcoder_send_frame (enc, NULL);
    // new resolution changes or buffer full should break flush.
    // if needed, add new cases here
    if (LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING == p_ctx->session_run_state) {
      GST_DEBUG ("%s(): break flush queued frames, "
          "resolution changes again, session_run_state=%d, status=%d\n",
          __FUNCTION__, p_ctx->session_run_state, p_ctx->status);
      break;
    } else if (NI_LOGAN_RETCODE_NVME_SC_WRITE_BUFFER_FULL == p_ctx->status) {
      p_ctx->session_run_state = LOGAN_SESSION_RUN_STATE_NORMAL;
      GST_DEBUG ("%s(): break flush queued frames, "
          "because of buffer full, session_run_state=%d, status=%d\n",
          __FUNCTION__, p_ctx->session_run_state, p_ctx->status);
      break;
    } else {
      p_ctx->session_run_state = LOGAN_SESSION_RUN_STATE_NORMAL;
      GST_DEBUG ("%s(): continue to flush queued frames, "
          "ret=%d\n", __FUNCTION__, ret);
    }
  }
  return ret;
}

static int
do_open_encoder_device (GstNiloganEnc * enc, GstVideoFrame * in_frame)
{
  int ret;
  int frame_width;
  int frame_height;
  int linesize_aligned;
  int height_aligned;
  int video_full_range_flag = 0;
  GstVideoInfo *info = &enc->input_state->info;
  ni_logan_session_context_t *p_ctx =
      gst_nilogan_context_get_session_context (enc->context);
  ni_logan_encoder_params_t *p_enc_params =
      gst_nilogan_context_get_xcoder_enc_param (enc->context);
  ni_logan_session_data_io_t *p_pkt =
      gst_nilogan_context_get_data_pkt (enc->context);
  //BufferInfo* in_frame = &enc->buffered_fme;
  ni_color_primaries_t ni_color_primaries = NI_COL_PRI_UNSPECIFIED;
  ni_color_transfer_characteristic_t ni_color_transfer = NI_COL_TRC_UNSPECIFIED;
  ni_color_space_t ni_color_space = NI_COL_SPC_UNSPECIFIED;

  if (in_frame->info.width > 0 && in_frame->info.height > 0) {
    frame_width = NI_LOGAN_ODD2EVEN (in_frame->info.width);
    frame_height = NI_LOGAN_ODD2EVEN (in_frame->info.height);
    if (enc->input_state->info.colorimetry.primaries !=
        GST_VIDEO_COLOR_PRIMARIES_UNKNOWN) {
      ni_color_primaries =
          map_gst_color_primaries (gst_video_color_primaries_to_iso
          (enc->input_state->info.colorimetry.primaries));
      p_enc_params->color_primaries = ni_color_primaries;
    }

    if (enc->input_state->info.colorimetry.transfer !=
        GST_VIDEO_TRANSFER_UNKNOWN) {
      ni_color_transfer =
          map_gst_color_trc (gst_video_transfer_function_to_iso
          (enc->input_state->info.colorimetry.transfer));
    }

    if (enc->input_state->info.colorimetry.matrix !=
        GST_VIDEO_COLOR_MATRIX_UNKNOWN) {
      ni_color_space =
          map_gst_color_space (gst_video_color_matrix_to_iso
          (enc->input_state->info.colorimetry.matrix));
    }
  } else {
    frame_width = NI_LOGAN_ODD2EVEN (info->width);
    frame_height = NI_LOGAN_ODD2EVEN (info->height);
    ni_color_primaries = NI_COL_PRI_UNSPECIFIED;
    ni_color_transfer = NI_COL_TRC_UNSPECIFIED;
    ni_color_space = NI_COL_SPC_UNSPECIFIED;
  }
  // if frame stride size is not as we expect it,
  // adjust using xcoder-params conf_win_right
  linesize_aligned = ((frame_width + 7) / 8) * 8;
  if (enc->codec_format == NI_LOGAN_CODEC_FORMAT_H264) {
    linesize_aligned = ((frame_width + 15) / 16) * 16;
  }

  if (linesize_aligned < NI_LOGAN_MIN_WIDTH) {
    p_enc_params->enc_input_params.conf_win_right +=
        NI_LOGAN_MIN_WIDTH - frame_width;
    linesize_aligned = NI_LOGAN_MIN_WIDTH;
  } else if (linesize_aligned > frame_width) {
    p_enc_params->enc_input_params.conf_win_right +=
        linesize_aligned - frame_width;
  }
  p_enc_params->source_width = linesize_aligned;

  height_aligned = ((frame_height + 7) / 8) * 8;
  if (enc->codec_format == NI_LOGAN_CODEC_FORMAT_H264) {
    height_aligned = ((frame_height + 15) / 16) * 16;
  }

  if (height_aligned < NI_LOGAN_MIN_HEIGHT) {
    p_enc_params->enc_input_params.conf_win_bottom +=
        NI_LOGAN_MIN_HEIGHT - frame_height;
    p_enc_params->source_height = NI_LOGAN_MIN_HEIGHT;
    height_aligned = NI_LOGAN_MIN_HEIGHT;
  } else if (height_aligned > frame_height) {
    p_enc_params->enc_input_params.conf_win_bottom +=
        height_aligned - frame_height;
    p_enc_params->source_height = height_aligned;
  }
  // DolbyVision support
  if (5 == p_enc_params->dolby_vision_profile &&
      NI_LOGAN_CODEC_FORMAT_H265 == enc->codec_format) {
    ni_color_primaries = ni_color_space = ni_color_transfer = 2;
    video_full_range_flag = 1;
  }
  // HDR HLG support
  if ((5 == p_enc_params->dolby_vision_profile &&
          NI_LOGAN_CODEC_FORMAT_H265 == enc->codec_format) ||
      ni_color_primaries == NI_COL_PRI_BT2020 ||
      ni_color_transfer == NI_COL_TRC_SMPTE2084 ||
      ni_color_transfer == NI_COL_TRC_ARIB_STD_B67 ||
      ni_color_space == NI_COL_SPC_BT2020_NCL ||
      ni_color_space == NI_COL_SPC_BT2020_CL) {
    p_enc_params->hdrEnableVUI = 1;
    // VUI setting including color setting
    ni_logan_set_vui (p_enc_params, p_ctx, ni_color_primaries,
        ni_color_transfer, ni_color_space, video_full_range_flag, info->par_n,
        info->par_d, p_ctx->codec_format);
    GST_DEBUG ("XCoder HDR color info color_primaries: %d "
        "color_trc: %d color_space %d video_full_range_flag %d sar %d/%d\n",
        ni_color_primaries, ni_color_transfer, ni_color_space,
        video_full_range_flag, info->par_n, info->par_d);
  } else {
    p_enc_params->hdrEnableVUI = 0;
    ni_logan_set_vui (p_enc_params, p_ctx, ni_color_primaries,
        ni_color_transfer, ni_color_space, video_full_range_flag, info->par_n,
        info->par_d, p_ctx->codec_format);
  }

  p_ctx->hw_id = enc->dev_enc_idx;
  p_ctx->hw_name = enc->dev_xcoder_name;
  strcpy (p_ctx->dev_xcoder, enc->dev_xcoder);

  if (in_frame->info.width > 0 && in_frame->info.height > 0) {
    GST_DEBUG ("XCoder buffered_fme"
        "width/height %dx%d conf_win_right %d  conf_win_bottom %d , "
        "color primaries %u trc %u space %u\n",
        in_frame->info.width, in_frame->info.height,
        p_enc_params->enc_input_params.conf_win_right,
        p_enc_params->enc_input_params.conf_win_bottom,
        ni_color_primaries, ni_color_transfer, ni_color_space);
    // hw todo
    if (enc->hardware_mode) {
//      ni_logan_hwframe_surface_t *surface = (ni_logan_hwframe_surface_t *)in_frame->data[3];
//      p_ctx->sender_handle = (ni_device_handle_t) surface->device_handle;
//      p_ctx->hw_action = NI_LOGAN_CODEC_HW_ENABLE;
//      av_log(avctx, AV_LOG_VERBOSE, "XCoder frame sender_handle:%p, hw_id:%d\n",
//             (void *) p_ctx->sender_handle, p_ctx->hw_id);
    }
//    if (in_frame->hw_frames_ctx && p_ctx->hw_id == -1)
//    {
//      avhwf_ctx = (AVHWFramesContext*) in_frame->hw_frames_ctx->data;
//      nif_src_ctx = avhwf_ctx->internal->priv;
//      p_ctx->hw_id = nif_src_p_ctx->hw_id;
//      av_log(avctx, AV_LOG_VERBOSE, "%s: hw_id -1 collocated to %d \n",
//             __FUNCTION__, p_ctx->hw_id);
//    }
  } else {
    GST_DEBUG ("XCoder frame width/height %dx%d conf_win_right"
        " %d  conf_win_bottom %d color primaries %u trc %u space %u\n",
        info->width, info->height,
        p_enc_params->enc_input_params.conf_win_right,
        p_enc_params->enc_input_params.conf_win_bottom, ni_color_primaries,
        ni_color_transfer, ni_color_space);
  }
  p_ctx->hw_name = enc->dev_xcoder_name;
  ret = ni_logan_device_session_open (p_ctx, NI_LOGAN_DEVICE_TYPE_ENCODER);
  // As the file handle may change we need to assign back
  enc->dev_xcoder_name = g_strdup (p_ctx->dev_xcoder_name);
  enc->blk_xcoder_name = p_ctx->blk_xcoder_name;
  enc->dev_enc_idx = p_ctx->hw_id;

  if (ret == NI_LOGAN_RETCODE_INVALID_PARAM) {
    GST_WARNING ("%s\n", p_ctx->param_err_msg);
  }
  if (ret != 0) {
    GST_ERROR ("Failed to open encoder (status = %d), "
        "critical error or resource unavailable\n", ret);
    ret = NI_IO_ERR;
    // xcoder_logan_encode_close(avctx); will be called at codec close
    return ret;
  } else {
    GST_DEBUG ("XCoder %s Index %d (inst: %d) opened "
        "successfully\n", enc->dev_xcoder_name, enc->dev_enc_idx,
        p_ctx->session_id);
  }
  enc->eos_fme_received = 0;
  return ret;
}

/*
 * Map to nienc_logan.c "xcoder_logan_send_frame"
 * */
static gint
xcoder_send_frame (GstNiloganEnc * enc, GstVideoCodecFrame * frame)
{
  GstVideoInfo *info = &enc->input_state->info;
  GstVideoFrame current_video_frame;
  ni_logan_session_context_t *p_ctx =
      gst_nilogan_context_get_session_context (enc->context);
  ni_logan_encoder_params_t *p_enc_params =
      gst_nilogan_context_get_xcoder_enc_param (enc->context);
  ni_logan_session_data_io_t *p_frame =
      gst_nilogan_context_get_data_frame (enc->context);
  bool hwframe = enc->hardware_mode;
  int format_in_use;
  int frame_width, frame_height;
  int ret = 0;
  int sent;
  int i;
  int orig_avctx_width = enc->width;
  int orig_avctx_height = enc->height;
  GstVideoCodecFrame *current_send_frame = NULL;
  // employ a ni_logan_frame_t as a data holder to convert/prepare for side data
  // of the passed in frame
  // data buffer for various SEI: HDR mastering display color volume, HDR
  // content light level, close caption, User data unregistered, HDR10+ etc.
  int send_sei_with_idr;
  uint8_t mdcv_data[NI_LOGAN_MAX_SEI_DATA];
  uint8_t cll_data[NI_LOGAN_MAX_SEI_DATA];
  uint8_t cc_data[NI_LOGAN_MAX_SEI_DATA];
  uint8_t udu_data[NI_LOGAN_MAX_SEI_DATA];
  uint8_t hdrp_data[NI_LOGAN_MAX_SEI_DATA];

  GST_DEBUG ("XCoder send frame\n");
  if (enc->encoder_flushing) {
    if (!frame && !g_list_length (enc->frame_list)) {
      GST_DEBUG ("XCoder EOF: null frame & fifo empty\n");
      return NI_EOF;
    }
  }

  if (!frame) {
    if (LOGAN_SESSION_RUN_STATE_QUEUED_FRAME_DRAINING ==
        p_ctx->session_run_state) {
      GST_DEBUG ("null frame, send queued frame\n");
    } else {
      enc->eos_fme_received = 1;
      current_video_frame.info.width = enc->width;
      current_video_frame.info.height = enc->height;
      GST_DEBUG ("null frame, ctx->eos_fme_received = 1\n");
    }
  } else {
    GST_DEBUG ("%s #%" PRIu64 "\n", __FUNCTION__, p_ctx->frame_num);

    // queue up the frame if fifo is NOT empty, or sequence change ongoing !
    if (g_list_length (enc->frame_list) ||
        LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
        p_ctx->session_run_state) {
      //ret = enqueue_logan_frame(avctx, frame);
      current_send_frame = frame;
      enc->frame_list = g_list_append (enc->frame_list, current_send_frame);

      if (LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
          p_ctx->session_run_state) {
        GST_DEBUG ("XCoder doing sequence change, frame #"
            "%" PRIu64 " queued and return 0 !\n", p_ctx->frame_num);
        return 0;
      }
    } else {
      GST_DEBUG ("set current send frame = %d\n", frame->system_frame_number);
      current_send_frame = frame;
      if (!enc->hardware_mode) {
        gst_video_frame_map (&current_video_frame, info,
            current_send_frame->input_buffer, GST_MAP_READ);
      }
//      enc->buffered_fme.frame = frame;
//      if (!enc->hardware_mode) {
//        enc->buffered_fme.mapped = FALSE;
//        if (!enc->hardware_mode) {
//          gst_video_frame_map(&enc->buffered_fme.vframe, info, frame->input_buffer,
//                              GST_MAP_READ);
//          enc->buffered_fme.mapped = TRUE;
//        }
//      }
    }
  }

  if (!g_list_length (enc->frame_list)) {
    GST_DEBUG ("no frame in fifo to send, just send/receive ..\n");
    if (enc->eos_fme_received) {
      GST_DEBUG ("no frame in fifo to send, send eos ..\n");
      // if received eos but not sent any frame, there is no need to continue the following process
      if (enc->started == 0) {
        GST_DEBUG ("session is not open, send eos, return EOF\n");
        return NI_EOF;
      }
    }
  } else {
    GList *item = g_list_first (enc->frame_list);
    current_send_frame = (GstVideoCodecFrame *) item->data;
    if (!enc->hardware_mode) {
      gst_video_frame_map (&current_video_frame, info,
          current_send_frame->input_buffer, GST_MAP_READ);
    }
    enc->frame_list = g_list_delete_link (enc->frame_list, item);
  }

//  frame_width = NI_LOGAN_ODD2EVEN(enc->buffered_fme.vframe.info.width);
//  frame_height = NI_LOGAN_ODD2EVEN(enc->buffered_fme.vframe.info.height);
  frame_width = NI_LOGAN_ODD2EVEN (current_video_frame.info.width);
  frame_height = NI_LOGAN_ODD2EVEN (current_video_frame.info.height);
  if (enc->hardware_mode) {
    frame_width = NI_LOGAN_ODD2EVEN (enc->input_state->info.width);
    frame_height = NI_LOGAN_ODD2EVEN (enc->input_state->info.height);
  }
  // leave encoder instance open to when the first frame buffer arrives so that
  // its stride size is known and handled accordingly.
  if (enc->started == 0) {
    {
      if ((ret = do_open_encoder_device (enc, &current_video_frame)) < 0) {
        return ret;
      }
    }
    p_frame->data.frame.start_of_stream = 1;
    enc->started = 1;
  } else {
    p_frame->data.frame.start_of_stream = 0;
  }

  if ((current_video_frame.info.height && current_video_frame.info.width)
      && (current_video_frame.info.height != enc->height
          || current_video_frame.info.width != enc->width)) {
    GST_DEBUG ("%s resolution change %dx%d -> %dx%d\n",
        __FUNCTION__, enc->width, enc->height, current_video_frame.info.width,
        current_video_frame.info.height);
    p_ctx->session_run_state = LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING;
    enc->eos_fme_received = 1;

    // have to queue this frame if not done so: an empty queue
    if (!g_list_length (enc->frame_list)) {
      GST_DEBUG ("%s resolution change when fifo empty, frame "
          "#%" PRIu64 " being queued, ref=%d\n", __FUNCTION__, p_ctx->frame_num,
          current_send_frame->ref_count);
      enc->frame_list = g_list_append (enc->frame_list, current_send_frame);
      if (ret < 0) {
        return ret;
      }
    }
  }
  // init aux params in ni_logan_frame_t
  ni_logan_enc_init_aux_params (p_frame);

  // employ a ni_logan_frame_t to represent decode frame when using new libxcoder
  // API to prepare side data
  ni_logan_frame_t dec_frame = { 0 };
  ni_aux_data_t *aux_data = NULL;

  if (LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING == p_ctx->session_run_state ||
      (enc->eos_fme_received && !g_list_length (enc->frame_list))) {
    GST_DEBUG ("XCoder start flushing, state=%d, eos=%d\n",
        p_ctx->session_run_state, enc->eos_fme_received);
    p_frame->data.frame.end_of_stream = 1;
    enc->encoder_flushing = 1;
  } else {
    // Set the initial value of extra_data_len
    p_frame->data.frame.extra_data_len = NI_LOGAN_APP_ENC_FRAME_META_DATA_SIZE;

    // NETINT_INTERNAL - currently only for internal testing
    if (p_enc_params->reconf_demo_mode) {
      ret = ni_logan_enc_fill_reconfig_params (p_enc_params, p_ctx,
          p_frame, enc->reconfigCount);
      if (ret < 0) {
        return ret;
      } else {
        enc->reconfigCount = ret;
      }
    }
    // NetInt support VFR by reconfig bitrate TODO

    // ROI
    uint8_t num_roi = gst_buffer_get_n_meta (current_send_frame->input_buffer,
        GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE);
    if (!p_enc_params->roi_demo_mode && num_roi > 0) {
      int roi_data_size = num_roi * sizeof (ni_region_of_interest_t);
      GstStructure *s;
      aux_data =
          ni_logan_frame_new_aux_data (&dec_frame,
          NI_FRAME_AUX_DATA_REGIONS_OF_INTEREST, roi_data_size);
      uint8_t *roi_data = g_malloc0 (roi_data_size + 1);
      int valid_roi = 0;
      for (int i = 0; i < num_roi; i++) {
        GstVideoRegionOfInterestMeta *roi;
        gpointer state = NULL;
        roi = (GstVideoRegionOfInterestMeta *)
            gst_buffer_iterate_meta_filtered (current_send_frame->input_buffer,
            &state, GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE);
        if (!roi)
          continue;
        ni_region_of_interest_t ni_roi_t;
        ni_roi_t.self_size = sizeof (ni_region_of_interest_t);
        ni_roi_t.top = roi->y;
        ni_roi_t.bottom = roi->y + roi->h;
        ni_roi_t.left = roi->x;
        ni_roi_t.right = roi->x + roi->w;

        s = gst_video_region_of_interest_meta_get_param (roi, "roi/nilogan");

        if (s) {
          double qp_offset;
          if (gst_structure_get_double (s, "delta-qp", &qp_offset)) {
            ni_roi_t.qoffset.num = qp_offset * 1000;
            ni_roi_t.qoffset.den = 1000;
          }
        }
        memcpy (roi_data + valid_roi * ni_roi_t.self_size, &ni_roi_t,
            ni_roi_t.self_size);
        valid_roi++;
      }
      roi_data_size = valid_roi * sizeof (ni_region_of_interest_t);
      if (aux_data) {
        memcpy (aux_data->data, roi_data, roi_data_size);
      }
      g_free (roi_data);
    }
    // Note: when ROI demo modes enabled, supply ROI map for the specified range
    //       frames, and 0 map for others
    if (p_enc_params->roi_demo_mode) {
      if (p_ctx->frame_num >= 300) {
        p_frame->data.frame.force_pic_qp =
            p_enc_params->enc_input_params.rc.intra_qp;
      } else if (p_ctx->frame_num >= 200) {
        p_frame->data.frame.force_pic_qp = p_enc_params->force_pic_qp_demo_mode;
      }
    }
    // SEI (HDR)
    // content light level info
    GstVideoContentLightLevel cll;
    if (gst_video_content_light_level_from_caps (&cll, enc->input_state->caps)) {
      ni_content_light_level_t metadata;

      metadata.max_cll = cll.max_content_light_level;
      metadata.max_fall = cll.max_frame_average_light_level;
      aux_data =
          ni_logan_frame_new_aux_data (&dec_frame,
          NI_FRAME_AUX_DATA_CONTENT_LIGHT_LEVEL,
          sizeof (ni_content_light_level_t));
      if (aux_data) {
        memcpy (aux_data->data, &metadata, sizeof (ni_content_light_level_t));
      }
    }
    // mastering display color volume

    GstVideoMasteringDisplayInfo minfo;
    if (gst_video_mastering_display_info_from_caps (&minfo,
            enc->input_state->caps)) {
      const int chroma_den = 50000;
      const int luma_den = 10000;
      ni_mastering_display_metadata_t metadata;

      for (i = 0; i < 3; i++) {
        metadata.display_primaries[i][0].num = minfo.display_primaries[i].x;
        metadata.display_primaries[i][0].den = chroma_den;
        metadata.display_primaries[i][1].num = minfo.display_primaries[i].y;
        metadata.display_primaries[i][1].den = chroma_den;
      }
      metadata.white_point[0].num = minfo.white_point.x;
      metadata.white_point[0].den = chroma_den;
      metadata.white_point[1].num = minfo.white_point.y;
      metadata.white_point[1].den = chroma_den;

      metadata.max_luminance.num = minfo.max_display_mastering_luminance;
      metadata.max_luminance.den = luma_den;
      metadata.min_luminance.num = minfo.min_display_mastering_luminance;
      metadata.min_luminance.den = luma_den;
      metadata.has_luminance = 1;
      metadata.has_primaries = 1;
      aux_data =
          ni_logan_frame_new_aux_data (&dec_frame,
          NI_FRAME_AUX_DATA_MASTERING_DISPLAY_METADATA,
          sizeof (ni_mastering_display_metadata_t));
      if (aux_data) {
        memcpy (aux_data->data, &metadata,
            sizeof (ni_mastering_display_metadata_t));
      }
    }
    //SEI (HDR10+)
    GstVideoHdrPlusMeta *hdr10_meta;
    gpointer iter_hdr10 = NULL;

    while ((hdr10_meta = (GstVideoHdrPlusMeta *)
            gst_buffer_iterate_meta_filtered (current_send_frame->input_buffer,
                &iter_hdr10, GST_VIDEO_HDR_PLUS_META_API_TYPE))) {
      aux_data =
          ni_logan_frame_new_aux_data (&dec_frame, NI_FRAME_AUX_DATA_HDR_PLUS,
          sizeof (ni_dynamic_hdr_plus_t));

      if (aux_data) {
        memcpy (aux_data->data, hdr10_meta->data, hdr10_meta->size);
      }
    }

    //SEI (UDU)
    GstVideoUDUMeta *udu_meta;
    gpointer iter_udu = NULL;

    while ((udu_meta = (GstVideoUDUMeta *)
            gst_buffer_iterate_meta_filtered (current_send_frame->input_buffer,
                &iter_hdr10, GST_VIDEO_UDU_META_API_TYPE))) {
      aux_data =
          ni_logan_frame_new_aux_data (&dec_frame, NI_FRAME_AUX_DATA_UDU_SEI,
          udu_meta->size);

      if (aux_data) {
        memcpy (aux_data->data, udu_meta->data, udu_meta->size);
      }
    }

    // SEI (close caption)
    GstVideoCaptionMeta *cc_meta;
    gpointer iter = NULL;

    while ((cc_meta = (GstVideoCaptionMeta *)
            gst_buffer_iterate_meta_filtered (current_send_frame->input_buffer,
                &iter, GST_VIDEO_CAPTION_META_API_TYPE))) {

      if (cc_meta->caption_type != GST_VIDEO_CAPTION_TYPE_CEA708_RAW)
        continue;

      aux_data =
          ni_logan_frame_new_aux_data (&dec_frame, NI_FRAME_AUX_DATA_A53_CC,
          cc_meta->size);
      if (aux_data) {
        memcpy (aux_data->data, cc_meta->data, cc_meta->size);
      }
    }

    p_frame->data.frame.pts = current_send_frame->pts;
    p_frame->data.frame.dts = current_send_frame->dts;
    p_frame->data.frame.video_width = NI_LOGAN_ODD2EVEN (enc->width);
    p_frame->data.frame.video_height = NI_LOGAN_ODD2EVEN (enc->height);

    p_frame->data.frame.ni_logan_pict_type = 0;

    if (p_ctx->force_frame_type) {
      if (GST_VIDEO_CODEC_FRAME_IS_FORCE_KEYFRAME (current_send_frame)) {
        p_frame->data.frame.ni_logan_pict_type = LOGAN_PIC_TYPE_FORCE_IDR;
      } else {
        p_frame->data.frame.ni_logan_pict_type = LOGAN_PIC_TYPE_P;
      }
    } else if (GST_VIDEO_CODEC_FRAME_IS_FORCE_KEYFRAME (current_send_frame)) {
      p_frame->data.frame.force_key_frame = 1;
      p_frame->data.frame.ni_logan_pict_type = LOGAN_PIC_TYPE_FORCE_IDR;
    }
    // whether should send SEI with this frame
    send_sei_with_idr =
        ni_logan_should_send_sei_with_frame (p_ctx,
        p_frame->data.frame.ni_logan_pict_type, p_enc_params);

    GST_DEBUG ("%s: #%" PRIu64 " ni_logan_pict_type %d "
        "forced_header_enable %d intraPeriod %d send_sei_with_idr: %s\n",
        __FUNCTION__,
        p_ctx->frame_num, p_frame->data.frame.ni_logan_pict_type,
        p_enc_params->enc_input_params.forced_header_enable,
        p_enc_params->enc_input_params.intra_period,
        send_sei_with_idr ? "Yes" : "No");
    // prep for auxiliary data (various SEI, ROI) in encode frame, based on the
    // data returned in decoded frame
    ni_logan_enc_prep_aux_data (p_ctx, &p_frame->data.frame, &dec_frame,
        p_ctx->codec_format, send_sei_with_idr,
        mdcv_data, cll_data, cc_data, udu_data, hdrp_data);
    // DolbyVision (HRD SEI), HEVC only for now
    uint8_t hrd_buf[NI_LOGAN_MAX_SEI_DATA];
    uint32_t hrd_sei_len = 0;   // HRD SEI size in bytes
    if (NI_LOGAN_CODEC_FORMAT_H265 == enc->codec_format
        && p_enc_params->hrd_enable) {
      if (send_sei_with_idr) {
        hrd_sei_len += ni_logan_enc_buffering_period_sei (p_enc_params, p_ctx,
            p_ctx->frame_num + 1, NI_LOGAN_MAX_SEI_DATA, hrd_buf);
      }
      //printf(" ^^^^ frame_num %u  idr %d\n", p_ctx->frame_num, send_sei_with_idr);
      // pic_timing SEI will inserted after encoding
      p_frame->data.frame.sei_total_len += hrd_sei_len;
    }

    if (p_frame->data.frame.sei_total_len > NI_LOGAN_ENC_MAX_SEI_BUF_SIZE) {
      GST_ERROR ("%s: sei total length %u exceeds maximum sei "
          "size %u.\n", __FUNCTION__, p_frame->data.frame.sei_total_len,
          NI_LOGAN_ENC_MAX_SEI_BUF_SIZE);
      ret = NI_EINVAL;
      return ret;
    }

    p_frame->data.frame.extra_data_len += p_frame->data.frame.sei_total_len;
    // FW layout requirement: leave space for reconfig data if SEI and/or ROI
    // is present
    if ((p_frame->data.frame.sei_total_len || p_frame->data.frame.roi_len)
        && !p_frame->data.frame.reconf_len) {
      p_frame->data.frame.extra_data_len +=
          sizeof (ni_logan_encoder_change_params_t);
    }

    if (p_ctx->auto_dl_handle != NI_INVALID_DEVICE_HANDLE) {
      hwframe = 0;
      convertGstVideoFormatToNIPix (enc->input_state->info.finfo->format);
      GST_DEBUG ("%s: Autodownload mode, disable hw frame\n", __FUNCTION__);
    } else {
      convertGstVideoFormatToNIPix (enc->input_state->info.finfo->format);
    }

    if (hwframe) {
      ret = sizeof (ni_logan_hwframe_surface_t);
    } else {
      ret =
          calculateSwFrameSize (current_video_frame.info.width,
          current_video_frame.info.height,
          convertGstVideoFormatToNIPix (enc->input_state->info.finfo->format));
    }

    //gst_niloganenc_queue_frame(enc, current_send_frame);

    if (hwframe) {
      // hwframe todo
//      uint8_t *dsthw;
//      const uint8_t *srchw;
//      ni_logan_frame_buffer_alloc_hwenc(&(ctx->api_fme.data.frame),
//                                        NI_LOGAN_ODD2EVEN(ctx->buffered_fme.width),
//                                        NI_LOGAN_ODD2EVEN(ctx->buffered_fme.height),
//                                        ctx->api_fme.data.frame.extra_data_len);
//      if (!ctx->api_fme.data.frame.p_data[3])
//      {
//        return AVERROR(ENOMEM);
//      }
//
//      dsthw = ctx->api_fme.data.frame.p_data[3];
//      srchw = (const uint8_t *) ctx->buffered_fme.data[3];
//      av_log(avctx, AV_LOG_TRACE, "dst=%p src=%p, len =%d\n", dsthw, srchw, ctx->api_fme.data.frame.data_len[3]);
//      memcpy(dsthw, srchw, ctx->api_fme.data.frame.data_len[3]);
//      av_log(avctx, AV_LOG_TRACE, "session_id:%u, FrameIdx:%d, %d, W-%u, H-%u, bit_depth:%d, encoding_type:%d\n",
//             ((ni_logan_hwframe_surface_t *)dsthw)->ui16SessionID,
//             ((ni_logan_hwframe_surface_t *)dsthw)->i8FrameIdx,
//             ((ni_logan_hwframe_surface_t *)dsthw)->i8InstID,
//             ((ni_logan_hwframe_surface_t *)dsthw)->ui16width,
//             ((ni_logan_hwframe_surface_t *)dsthw)->ui16height,
//             ((ni_logan_hwframe_surface_t *)dsthw)->bit_depth,
//             ((ni_logan_hwframe_surface_t *)dsthw)->encoding_type);
    } else {
      int dst_stride[NI_LOGAN_MAX_NUM_DATA_POINTERS] = { 0 };
      int dst_height_aligned[NI_LOGAN_MAX_NUM_DATA_POINTERS] = { 0 };
      int src_height[NI_LOGAN_MAX_NUM_DATA_POINTERS] = { 0 };

      src_height[0] = current_video_frame.info.height;
      src_height[1] = src_height[2] = current_video_frame.info.height / 2;

      ni_logan_get_hw_yuv420p_dim (frame_width, frame_height,
          p_ctx->bit_depth_factor,
          enc->codec_format == NI_LOGAN_CODEC_FORMAT_H264,
          dst_stride, dst_height_aligned);

      // alignment(16) extra padding for H.264 encoding
      ni_logan_encoder_frame_buffer_alloc (&(p_frame->data.frame),
          NI_LOGAN_ODD2EVEN (current_video_frame.info.width),
          NI_LOGAN_ODD2EVEN (current_video_frame.info.height),
          dst_stride,
          (enc->codec_format == NI_LOGAN_CODEC_FORMAT_H264),
          p_frame->data.frame.extra_data_len, p_ctx->bit_depth_factor);
      if (!p_frame->data.frame.p_data[0]) {
        return NI_EINVAL;
      }

      if (p_ctx->auto_dl_handle == NI_INVALID_DEVICE_HANDLE) {
        GST_DEBUG ("%s: api_fme.data_len[0]=%d,"
            "dst alloc linesize = %d/%d/%d, "
            "src height = %d/%d%d, dst height aligned = %d/%d/%d, "
            "ctx->api_fme.force_key_frame=%d, extra_data_len=%d sei_size=%u "
            "(hdr_content_light_level %u hdr_mastering_display_color_vol %u "
            "hdr10+ %u cc %u udu %u prefC %u hrd %u) "
            "reconf_size=%u roi_size=%u force_pic_qp=%u "
            "use_cur_src_as_long_term_pic %u use_long_term_ref %u\n",
            __FUNCTION__, p_frame->data.frame.data_len[0],
            dst_stride[0], dst_stride[1], dst_stride[2],
            src_height[0], src_height[1], src_height[2],
            dst_height_aligned[0], dst_height_aligned[1], dst_height_aligned[2],
            p_frame->data.frame.force_key_frame,
            p_frame->data.frame.extra_data_len,
            p_frame->data.frame.sei_total_len,
            p_frame->data.frame.sei_hdr_content_light_level_info_len,
            p_frame->data.frame.sei_hdr_mastering_display_color_vol_len,
            p_frame->data.frame.sei_hdr_plus_len,
            p_frame->data.frame.sei_cc_len,
            p_frame->data.frame.sei_user_data_unreg_len,
            p_frame->data.frame.preferred_characteristics_data_len,
            hrd_sei_len,
            p_frame->data.frame.reconf_len,
            p_frame->data.frame.roi_len,
            p_frame->data.frame.force_pic_qp,
            p_frame->data.frame.use_cur_src_as_long_term_pic,
            p_frame->data.frame.use_long_term_ref);

        uint8_t *src_data[8];
        for (i = 0; i < GST_VIDEO_FRAME_N_PLANES (&current_video_frame); i++) {
          src_data[i] = GST_VIDEO_FRAME_PLANE_DATA (&current_video_frame, i);
        }
        // YUV part of the encoder input data layout
        ni_logan_copy_hw_yuv420p ((uint8_t **) p_frame->data.frame.p_data,
            src_data, current_video_frame.info.width,
            current_video_frame.info.height,
            p_ctx->bit_depth_factor,
            dst_stride, dst_height_aligned,
            current_video_frame.info.stride, src_height);

        GST_DEBUG ("After memcpy p_data 0:0x%p, 1:0x%p, 2:0x%p"
            " len:0:%d 1:%d 2:%d\n",
            p_frame->data.frame.p_data[0],
            p_frame->data.frame.p_data[1],
            p_frame->data.frame.p_data[2],
            p_frame->data.frame.data_len[0],
            p_frame->data.frame.data_len[1], p_frame->data.frame.data_len[2]);
      } else {
        //hwframe todo
//        ni_logan_hwframe_surface_t *src_surf;
//        ni_logan_session_data_io_t *p_session_data;
//        GST_DEBUG("%s: Autodownload to be run\n", __FUNCTION__);
//        src_surf = (ni_logan_hwframe_surface_t*)ctx->buffered_fme.data[3];
//        p_session_data = &ctx->api_fme;
//
//        ret = ni_logan_device_session_hwdl(&nif_src_ctx->api_ctx, p_session_data, src_surf);
//        if (ret <= 0)
//        {
//          av_log(avctx, AV_LOG_ERROR, "nienc.c:ni_logan_hwdl_frame() failed to retrieve frame\n");
//          return AVERROR_EXTERNAL;
//        }
      }
    }

    // auxiliary data part of the encoder input data layout
    ni_logan_enc_copy_aux_data (p_ctx, &p_frame->data.frame, &dec_frame,
        p_ctx->codec_format, mdcv_data, cll_data, cc_data, udu_data, hdrp_data);
    ni_logan_frame_buffer_free (&dec_frame);

    // fill in HRD SEI if available
    if (hrd_sei_len) {
      uint8_t *dst = (uint8_t *) p_frame->data.frame.p_data[3] +
          p_frame->data.frame.data_len[3] +
          NI_LOGAN_APP_ENC_FRAME_META_DATA_SIZE;

      // skip data portions already filled in until the HRD SEI part;
      // reserve reconfig size if any of sei, roi or reconfig is present
      if (p_frame->data.frame.reconf_len ||
          p_frame->data.frame.roi_len || p_frame->data.frame.sei_total_len) {
        dst += sizeof (ni_logan_encoder_change_params_t);
      }
      // skip any of the following data types enabled, to get to HRD location:
      // - ROI map
      // - HDR mastering display color volume
      // - HDR content light level info
      // - HLG preferred characteristics SEI
      // - close caption
      // - HDR10+
      // - User data unregistered SEI
      dst += p_frame->data.frame.roi_len +
          p_frame->data.frame.sei_hdr_mastering_display_color_vol_len +
          p_frame->data.frame.sei_hdr_content_light_level_info_len +
          p_frame->data.frame.preferred_characteristics_data_len +
          p_frame->data.frame.sei_cc_len +
          p_frame->data.frame.sei_hdr_plus_len +
          p_frame->data.frame.sei_user_data_unreg_len;

      memcpy (dst, hrd_buf, hrd_sei_len);
    }
    //enc->sentFrame = 1;
  }

  sent =
      ni_logan_device_session_write (p_ctx, p_frame,
      NI_LOGAN_DEVICE_TYPE_ENCODER);
  GST_DEBUG ("%s: pts %lld dts %lld size %d sent to xcoder\n", __FUNCTION__,
      p_frame->data.frame.pts, p_frame->data.frame.dts, sent);

  if (current_send_frame) {
    gst_video_frame_unmap (&current_video_frame);
  }
  // return EIO at error
  if (NI_LOGAN_RETCODE_ERROR_VPU_RECOVERY == sent) {
    //ret = xcoder_logan_encode_reset(avctx);
    if (ret < 0) {
      GST_ERROR ("%s(): VPU recovery failed:%d, return EIO\n",
          __FUNCTION__, sent);
      ret = NI_EINVAL;
    }
    return ret;
  } else if (sent < 0) {
    GST_ERROR ("%s(): failure sent (%d) , return EIO\n", __FUNCTION__, sent);
    ret = NI_EINVAL;

    // if rejected due to sequence change in progress, revert resolution
    // setting and will do it again next time.
    if (p_frame->data.frame.start_of_stream &&
        (enc->width != orig_avctx_width || enc->height != orig_avctx_height)) {
      enc->width = orig_avctx_width;
      enc->height = orig_avctx_height;
    }
    return ret;
  } else if (sent == 0) {
    // case of sequence change in progress
    if (p_frame->data.frame.start_of_stream &&
        (enc->width != orig_avctx_width || enc->height != orig_avctx_height)) {
      enc->width = orig_avctx_width;
      enc->height = orig_avctx_height;
    }
    // when buffer_full, drop the frame and return EAGAIN if in strict timeout
    // mode, otherwise buffer the frame and it is to be sent out using encode2
    // API: queue the frame only if not done so yet, i.e. queue is empty
    // *and* it's a valid frame. ToWatch: what are other rc cases ?
    if (p_ctx->status == NI_LOGAN_RETCODE_NVME_SC_WRITE_BUFFER_FULL) {
      if (p_enc_params->strict_timeout_mode) {
        GST_ERROR ("%s: Error Strict timeout period exceeded, "
            "return EAGAIN\n", __FUNCTION__);
        ret = NI_EAGAIN;
      } else {
        GST_DEBUG ("%s: Write buffer full, returning 1\n", __FUNCTION__);
        ret = 1;
        if (frame && !g_list_length (enc->frame_list)) {
          enc->frame_list =
              g_list_prepend (enc->frame_list, current_send_frame);
          //gst_niloganenc_queue_frame(enc, current_send_frame);
        }
      }
    }
  } else {
    if (!enc->eos_fme_received && hwframe) {
      //hwframe todo
//      av_frame_ref(ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]], &ctx->buffered_fme);
//      GST_WARNING( "AVframe_index = %d popped from free head %d\n", ctx->aFree_Avframes_list[ctx->freeHead], ctx->freeHead);
//      av_log(avctx, AV_LOG_TRACE, "ctx->buffered_fme.data[3] %p sframe_pool[%d]->data[3] %p\n",
//             ctx->buffered_fme.data[3], ctx->aFree_Avframes_list[ctx->freeHead],
//             ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3]);
//      if (ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3])
//      {
//        GST_WARNING( "sframe_pool[%d] ui16FrameIdx %u, device_handle %" PRId64 ".\n",
//               ctx->aFree_Avframes_list[ctx->freeHead],
//               ((ni_logan_hwframe_surface_t*)((uint8_t*)ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3]))->i8FrameIdx,
//               ((ni_logan_hwframe_surface_t*)((uint8_t*)ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3]))->device_handle);
//        av_log(avctx, AV_LOG_TRACE, "%s: after ref sframe_pool, hw frame av_buffer_get_ref_count=%d, data[3]=%p\n",
//               __FUNCTION__, av_buffer_get_ref_count(ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->buf[0]),
//               ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3]);
//      }
//      if (deq_logan_free_frames(ctx) != 0)
//      {
//        av_log(avctx, AV_LOG_ERROR, "free frames is empty\n");
//        ret = AVERROR_EXTERNAL;
//        return ret;
//      }
    }
    // only if it's NOT sequence change flushing (in which case only the eos
    // was sent and not the first sc pkt) AND
    // only after successful sending will it be removed from fifo
    if (LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING != p_ctx->session_run_state) {
      //sequence change
//      if (! is_logan_input_fifo_empty(ctx))
//      {
//        av_fifo_drain(ctx->fme_fifo, sizeof(AVFrame));
//        GST_WARNING( "fme popped pts:%" PRId64 ", "
//                                    "fifo size: %" PRIu64 "\n",  ctx->buffered_fme.pts,
//               av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));
//      }
//      av_frame_unref(&ctx->buffered_fme);
    } else {
      GST_DEBUG ("XCoder frame(eos) sent, sequence changing! NO fifo pop !\n");
    }

    //pushing input pts in circular FIFO
    p_ctx->enc_pts_list[p_ctx->enc_pts_w_idx % NI_LOGAN_FIFO_SZ] =
        p_frame->data.frame.pts;
    p_ctx->enc_pts_w_idx++;
    ret = 0;

    // have another check before return: if no more frames in fifo to send and
    // we've got eos (NULL) frame from upper stream, flag for flushing
    if (enc->eos_fme_received && g_list_length (enc->frame_list) == 0) {
      GST_WARNING
          ("Upper stream EOS frame received, fifo empty, start flushing ..\n");
      enc->encoder_flushing = 1;
    }
    ret = GST_FLOW_OK;
  }

  if (enc->encoder_flushing) {
    GST_DEBUG ("%s flushing ..\n", __FUNCTION__);
    ret = ni_logan_device_session_flush (p_ctx, NI_LOGAN_DEVICE_TYPE_ENCODER);
  }

  GST_DEBUG ("XCoder send frame return %d \n", ret);
  return ret;
}

static ni_logan_retcode_t
ni_gst_logan_packet_buffer_alloc (ni_logan_packet_t * p_packet, int packet_size)
{
  void *p_buffer = NULL;
  int metadata_size = 0;
  int retval = NI_LOGAN_RETCODE_SUCCESS;

  metadata_size = NI_LOGAN_FW_META_DATA_SZ;

  int buffer_size =
      (((packet_size + metadata_size) / NI_LOGAN_MAX_PACKET_SZ) +
      1) * NI_LOGAN_MAX_PACKET_SZ;

  GST_DEBUG ("%s: packet_size=%d\n", __FUNCTION__, packet_size + metadata_size);

  if (!p_packet || !packet_size) {
    ni_log (NI_LOG_TRACE, "ERROR: %s(): null pointer parameters passed\n",
        __FUNCTION__);
    return NI_LOGAN_RETCODE_INVALID_PARAM;
  }

  if (buffer_size % NI_LOGAN_MEM_PAGE_ALIGNMENT) {
    buffer_size =
        ((buffer_size / NI_LOGAN_MEM_PAGE_ALIGNMENT) *
        NI_LOGAN_MEM_PAGE_ALIGNMENT) + NI_LOGAN_MEM_PAGE_ALIGNMENT;
  }

  if (p_packet->buffer_size == buffer_size) {
    p_packet->p_data = p_packet->p_buffer;
    ni_log (NI_LOG_TRACE, "%s(): reuse current p_packet buffer\n",
        __FUNCTION__);
    LRETURN;                    //Already allocated the exact size
  } else if (p_packet->buffer_size > 0) {
    ni_log (NI_LOG_TRACE, "%s(): free current p_packet buffer_size=%d\n",
        __FUNCTION__, p_packet->buffer_size);
    ni_logan_packet_buffer_free (p_packet);
  }
  ni_log (NI_LOG_TRACE, "%s(): Allocating p_frame buffer, buffer_size=%d\n",
      __FUNCTION__, buffer_size);
  p_buffer = malloc (buffer_size);
  if (p_buffer == NULL) {
    ni_log (NI_LOG_ERROR, "ERROR %d: %s(): Cannot allocate p_frame buffer.\n",
        NI_ERRNO, __FUNCTION__);
    retval = NI_LOGAN_RETCODE_ERROR_MEM_ALOC;
    LRETURN;
  }
  memset (p_buffer, 0, buffer_size);
  p_packet->buffer_size = buffer_size;
  p_packet->p_buffer = p_buffer;
  p_packet->p_data = p_packet->p_buffer;

END:

  if (NI_LOGAN_RETCODE_SUCCESS != retval) {
    ni_logan_aligned_free (p_buffer);
  }

  GST_DEBUG ("%s: exit: p_packet->buffer_size=%d\n",
      __FUNCTION__, p_packet->buffer_size);

  return retval;
}

/*
 * Map to nienc_logan.c "xcoder_logan_receive_packet"
 * */
static int
xcoder_receive_packet (GstNiloganEnc * enc, ni_logan_packet_t * xpkt,
    uint8_t * p_data, gint * size)
{
  ni_logan_session_data_io_t *p_pkt =
      gst_nilogan_context_get_data_pkt (enc->context);
  ni_logan_session_context_t *p_ctx =
      gst_nilogan_context_get_session_context (enc->context);
  ni_logan_encoder_params_t *p_enc_params =
      gst_nilogan_context_get_xcoder_enc_param (enc->context);
  int i, ret = 0;
  int recv;

  if (enc->encoder_eof) {
    GST_DEBUG ("xcoder_receive_packet: EOS\n");
    return NI_EOF;
  }

  if (ni_logan_packet_buffer_alloc (xpkt, NI_LOGAN_MAX_TX_SZ)) {
    GST_ERROR
        ("xcoder_receive_packet: packet buffer size %d allocation failed\n",
        NI_LOGAN_MAX_TX_SZ);
    return NI_MEM_ERR;
  }

  while (1) {
    xpkt->recycle_index = -1;
    recv =
        ni_logan_device_session_read (p_ctx, p_pkt,
        NI_LOGAN_DEVICE_TYPE_ENCODER);

    GST_DEBUG ("%s: xpkt.end_of_stream=%d, xpkt.data_len=%d, "
        "recv=%d, encoder_flushing=%d, encoder_eof=%d\n", __FUNCTION__,
        xpkt->end_of_stream, xpkt->data_len, recv, enc->encoder_flushing,
        enc->encoder_eof);

    if (recv <= 0) {
      enc->encoder_eof = xpkt->end_of_stream;
      if (enc->encoder_eof || xpkt->end_of_stream) {
        if (LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
            p_ctx->session_run_state) {
          // after sequence change completes, reset codec state
          GST_DEBUG ("xcoder_receive_packet 1: sequence "
              "change completed, return AVERROR(EAGAIN) and will reopen "
              "codec!\n");

          ret = xcoder_encode_reinit (enc);
          GST_DEBUG ("xcoder_receive_packet: xcoder_encode_reinit ret %d\n",
              ret);
          if (ret >= 0) {
            ret = NI_EAGAIN;
          }
          break;
        }

        ret = NI_EOF;
        GST_DEBUG
            ("xcoder_receive_packet: got encoder_eof, return AVERROR_EOF\n");
        break;
      } else {
        bool bIsReset = false;
        if (NI_LOGAN_RETCODE_ERROR_VPU_RECOVERY == recv) {
          xcoder_encode_reset (enc);
          bIsReset = true;
        }
        ret = NI_EAGAIN;
        if ((!enc->encoder_flushing && !enc->eos_fme_received) || bIsReset)     // if encode session was reset, can't read again with invalid session, must break out first
        {
          GST_DEBUG ("xcoder_receive_packet: NOT encoder_"
              "flushing, NOT eos_fme_received, return AVERROR(EAGAIN)\n");
          break;
        }
      }
    } else {
      /* got encoded data back */
      ret = 0;
      int meta_size = NI_LOGAN_FW_ENC_BITSTREAM_META_DATA_SIZE;

      //hwframe todo
//      if (enc->hardware_mode && xpkt->recycle_index >= 0 &&
//          enc->height >= NI_LOGAN_MIN_HEIGHT && enc->width >= NI_MIN_WIDTH &&
//          xpkt->recycle_index <
//          NI_GET_MAX_HWDESC_FRAME_INDEX (p_ctx->ddr_config)) {
//        GST_DEBUG ("UNREF trace ui16FrameIdx = [%d].\n", xpkt->recycle_index);
//        // enqueue the index back to free
//        xpkt->recycle_index = -1;
//      }

      if (!enc->spsPpsArrived) {
        ret = NI_EAGAIN;
        enc->spsPpsArrived = 1;
        enc->spsPpsHdrLen = recv - meta_size;
        enc->p_spsPpsHdr = g_malloc (enc->spsPpsHdrLen);
        if (!enc->p_spsPpsHdr) {
          ret = NI_MEM_ERR;
          break;
        }

        memcpy (enc->p_spsPpsHdr, (uint8_t *) xpkt->p_data + meta_size,
            xpkt->data_len - meta_size);

        // start pkt_num counter from 1 to get the real first frame
        p_ctx->pkt_num = 1;
        // for low-latency mode, keep reading until the first frame is back
        if (p_enc_params->low_delay_mode) {
          GST_DEBUG ("XCoder receive packet: low delay mode,"
              " keep reading until 1st pkt arrives\n");
          continue;
        }
        break;
      }

      GST_DEBUG ("Frame_type=%d,ret=%d,sps=%d\n", xpkt->frame_type, ret,
          enc->spsPpsArrived);

      uint8_t pic_timing_buf[NI_LOGAN_MAX_SEI_DATA];
      uint32_t pic_timing_sei_len = 0;
      uint32_t nalu_type = 0;
      const uint8_t *p_start_code;
      uint32_t stc = -1;
      uint32_t copy_len = 0;
      uint8_t *p_src = (uint8_t *) xpkt->p_data + meta_size;
      uint8_t *p_end = p_src + (xpkt->data_len - meta_size);
      int is_idr = 0;
      int64_t local_pts = xpkt->pts;
      int custom_sei_cnt = 0;
      int total_custom_sei_len = 0;
      int sei_idx = 0;
      ni_logan_all_custom_sei_t *ni_logan_all_custom_sei = NULL;
      ni_logan_custom_sei_t *ni_custom_sei = NULL;

      if (p_ctx->pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ]) {
        ni_logan_all_custom_sei =
            p_ctx->pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ];
        custom_sei_cnt = ni_logan_all_custom_sei->custom_sei_cnt;
        for (sei_idx = 0; sei_idx < custom_sei_cnt; sei_idx++) {
          total_custom_sei_len +=
              ni_logan_all_custom_sei->ni_custom_sei[sei_idx].custom_sei_size;
        }
      }

      if (p_enc_params->hrd_enable || custom_sei_cnt) {
        // if HRD or custom sei enabled, search for pic_timing or custom SEI insertion point by
        // skipping non-VCL until video data is found.
        p_start_code = p_src;
        if (NI_LOGAN_CODEC_FORMAT_H265 == enc->codec_format) {
          do {
            stc = -1;
            p_start_code = find_start_code (p_start_code, p_end, &stc);
            nalu_type = (stc >> 1) & 0x3F;
          } while (nalu_type > 31);

          // calc. length to copy
          copy_len = p_start_code - 5 - p_src;
        } else if (NI_LOGAN_CODEC_FORMAT_H264 == enc->codec_format) {
          do {
            stc = -1;
            p_start_code = find_start_code (p_start_code, p_end, &stc);
            nalu_type = stc & 0x1F;
          } while (nalu_type > 5);

          // calc. length to copy
          copy_len = p_start_code - 5 - p_src;
        } else {
          GST_ERROR ("xcoder_receive packet: codec %d not "
              "supported for SEI !\n", enc->codec_format);
        }

        if (p_enc_params->hrd_enable) {
          int is_i_or_idr;
          if (19 == nalu_type || 20 == nalu_type) {
            is_idr = 1;
          }
          is_i_or_idr = (LOGAN_PIC_TYPE_I == xpkt->frame_type ||
              LOGAN_PIC_TYPE_IDR == xpkt->frame_type ||
              LOGAN_PIC_TYPE_CRA == xpkt->frame_type);
          pic_timing_sei_len =
              ni_logan_enc_pic_timing_sei2 (p_enc_params, p_ctx, is_i_or_idr,
              is_idr, xpkt->pts, NI_LOGAN_MAX_SEI_DATA, pic_timing_buf);
          // returned pts is display number
        }
      }

      if (!enc->firstPktArrived) {
        int sizeof_spspps_attached_to_idr = enc->spsPpsHdrLen;
        enc->firstPktArrived = 1;
        if (enc->first_frame_pts == INT_MIN)
          enc->first_frame_pts = xpkt->pts;

        uint8_t *p_dst, *p_side_data;

        p_dst = p_data;
        if (sizeof_spspps_attached_to_idr) {
          memcpy (p_dst, enc->p_spsPpsHdr, enc->spsPpsHdrLen);
          *size += enc->spsPpsHdrLen;
          p_dst += enc->spsPpsHdrLen;
        }

        if (pic_timing_sei_len || custom_sei_cnt) {
          // copy buf_period
          memcpy (p_dst, p_src, copy_len);
          p_dst += copy_len;
          *size += copy_len;

          for (i = 0; i < custom_sei_cnt; i++) {
            ni_custom_sei = &ni_logan_all_custom_sei->ni_custom_sei[sei_idx];
            if (ni_custom_sei->custom_sei_loc ==
                NI_LOGAN_CUSTOM_SEI_LOC_AFTER_VCL) {
              break;
            }
            memcpy (p_dst, ni_custom_sei->custom_sei_data,
                ni_custom_sei->custom_sei_size);
            p_dst += ni_custom_sei->custom_sei_size;
            *size += ni_custom_sei->custom_sei_size;
            sei_idx++;
          }

          // copy pic_timing
          if (pic_timing_sei_len) {
            memcpy (p_dst, pic_timing_buf, pic_timing_sei_len);
            p_dst += pic_timing_sei_len;
            *size += pic_timing_sei_len;
          }
          // copy the IDR data
          memcpy (p_dst, p_src + copy_len,
              xpkt->data_len - meta_size - copy_len);
          p_dst += xpkt->data_len - meta_size - copy_len;
          *size += xpkt->data_len - meta_size - copy_len;

          // copy custom sei after slice
          for (; i < custom_sei_cnt; i++) {
            ni_custom_sei = &ni_logan_all_custom_sei->ni_custom_sei[sei_idx];
            memcpy (p_dst, ni_custom_sei->custom_sei_data,
                ni_custom_sei->custom_sei_size);
            p_dst += ni_custom_sei->custom_sei_size;
            *size += ni_custom_sei->custom_sei_size;
          }
        } else {
          memcpy (p_dst, (uint8_t *) xpkt->p_data + meta_size,
              xpkt->data_len - meta_size);
          *size += xpkt->data_len - meta_size;
        }

        // free buffer
        if (custom_sei_cnt) {
          free (p_ctx->pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ]);
          p_ctx->pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ] = NULL;
        }
      } else {
        // insert header when intraRefresh is enabled and forced header mode is 1 (all key frames)
        // for every intraRefreshMinPeriod key frames, pkt counting starts from 1, e.g. for
        // cycle of 100, the header is forced on frame 102, 202, ...;
        // note that api_ctx.pkt_num returned is the actual index + 1
        int intra_refresh_hdr_sz = 0;
        if (enc->p_spsPpsHdr && enc->spsPpsHdrLen &&
            (p_enc_params->enc_input_params.forced_header_enable ==
                NI_LOGAN_ENC_REPEAT_HEADERS_ALL_KEY_FRAMES)
            && (1 == p_enc_params->enc_input_params.intra_mb_refresh_mode
                || 2 == p_enc_params->enc_input_params.intra_mb_refresh_mode
                || 3 == p_enc_params->enc_input_params.intra_mb_refresh_mode)) {
          if (p_enc_params->intra_refresh_reset) {
            if (p_ctx->pkt_num - p_ctx->force_frame_pkt_num ==
                p_enc_params->ui32minIntraRefreshCycle) {
              intra_refresh_hdr_sz = enc->spsPpsHdrLen;
              p_ctx->force_frame_pkt_num = p_ctx->pkt_num;
            } else {
              for (i = 0; i < NI_LOGAN_MAX_FORCE_FRAME_TABLE_SIZE; i++) {
                if (xpkt->pts == p_ctx->force_frame_pts_table[i]) {
                  intra_refresh_hdr_sz = enc->spsPpsHdrLen;
                  p_ctx->force_frame_pkt_num = p_ctx->pkt_num;
                  break;
                }
              }
            }
          } else if (p_enc_params->ui32minIntraRefreshCycle > 0
              && p_ctx->pkt_num > 3
              && 0 ==
              ((p_ctx->pkt_num - 3) % p_enc_params->ui32minIntraRefreshCycle)) {
            intra_refresh_hdr_sz = enc->spsPpsHdrLen;
            GST_DEBUG ("%s pkt %" PRId64 " force header on "
                "intraRefreshMinPeriod %u\n", __FUNCTION__,
                p_ctx->pkt_num - 1, p_enc_params->ui32minIntraRefreshCycle);
          }
        }

        uint8_t *p_dst = p_data;
        if (intra_refresh_hdr_sz) {
          memcpy (p_dst, enc->p_spsPpsHdr, intra_refresh_hdr_sz);
          p_dst += intra_refresh_hdr_sz;
          *size += intra_refresh_hdr_sz;
        }
        // insert pic_timing if required
        if (pic_timing_sei_len || custom_sei_cnt) {
          // for non-IDR, skip AUD and insert
          // for IDR, skip AUD VPS SPS PPS buf_period and insert
          memcpy (p_dst, p_src, copy_len);
          p_dst += copy_len;
          *size += copy_len;

          // copy custom sei before slice
          sei_idx = 0;
          while (sei_idx < custom_sei_cnt) {
            ni_custom_sei = &ni_logan_all_custom_sei->ni_custom_sei[sei_idx];
            if (ni_custom_sei->custom_sei_loc ==
                NI_LOGAN_CUSTOM_SEI_LOC_AFTER_VCL) {
              break;
            }
            memcpy (p_dst, ni_custom_sei->custom_sei_data,
                ni_custom_sei->custom_sei_size);
            p_dst += ni_custom_sei->custom_sei_size;
            *size += ni_custom_sei->custom_sei_size;
            sei_idx++;
          }

          // copy pic_timing
          if (pic_timing_sei_len) {
            memcpy (p_dst, pic_timing_buf, pic_timing_sei_len);
            p_dst += pic_timing_sei_len;
            *size += pic_timing_sei_len;
          }
          // copy the video data
          memcpy (p_dst, p_src + copy_len,
              xpkt->data_len - meta_size - copy_len);
          p_dst += (xpkt->data_len - meta_size - copy_len);
          *size += (xpkt->data_len - meta_size - copy_len);

          // copy custom sei after slice
          while (sei_idx < custom_sei_cnt) {
            ni_custom_sei = &ni_logan_all_custom_sei->ni_custom_sei[sei_idx];
            memcpy (p_dst, ni_custom_sei->custom_sei_data,
                ni_custom_sei->custom_sei_size);
            p_dst += ni_custom_sei->custom_sei_size;
            *size += ni_custom_sei->custom_sei_size;
            sei_idx++;
          }
        } else {
          memcpy (p_dst, (uint8_t *) xpkt->p_data + meta_size,
              xpkt->data_len - meta_size);
          *size += xpkt->data_len - meta_size;
        }
        // free buffer
        if (custom_sei_cnt) {
          free (p_ctx->pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ]);
          p_ctx->pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ] = NULL;
        }
      }

      if (!ret) {

        /* to ensure pts>dts for all frames, we assign a guess pts for the first 'dtsOffset' frames and then the pts from input stream
         * is extracted from input pts FIFO.
         * if GOP = IBBBP and PTSs = 0 1 2 3 4 5 .. then out DTSs = -3 -2 -1 0 1 ... and -3 -2 -1 are the guessed values
         * if GOP = IBPBP and PTSs = 0 1 2 3 4 5 .. then out DTSs = -1 0 1 2 3 ... and -1 is the guessed value
         * the number of guessed values is equal to dtsOffset
         */
        if (enc->total_frames_received < enc->dts_offset) {
          // guess dts
          xpkt->dts = enc->first_frame_pts +
              ((enc->gop_offset_count -
                  enc->dts_offset) * enc->ticks_per_frame);
          enc->gop_offset_count++;
          GST_DEBUG ("Packet dts (guessed): %lld\n", xpkt->dts);
        } else {
          // get dts from pts FIFO
          xpkt->dts =
              p_ctx->enc_pts_list[p_ctx->enc_pts_r_idx % NI_LOGAN_FIFO_SZ];
          p_ctx->enc_pts_r_idx++;
          GST_DEBUG ("Packet dts: %lld\n", xpkt->dts);
        }
        if (enc->total_frames_received >= 1) {
          if (xpkt->dts < enc->latest_dts) {
            GST_DEBUG ("dts: %lld < latest_dts: %" PRIi64 ".\n", xpkt->dts,
                enc->latest_dts);
          }
        }
        if (xpkt->dts > xpkt->pts) {
          GST_DEBUG ("dts: %lld, pts: %lld. Forcing dts = pts \n", xpkt->dts,
              xpkt->pts);
          xpkt->dts = xpkt->pts;
          GST_DEBUG ("Force dts to: %lld\n", xpkt->dts);
        }

        enc->total_frames_received++;
        enc->latest_dts = xpkt->dts;
        GST_DEBUG ("XCoder recv pkt %" PRIu64
            " pts %lld dts %lld size %d frame_type %u avg qp %u\n",
            p_ctx->pkt_num - 1, xpkt->pts, xpkt->dts, *size, xpkt->frame_type,
            xpkt->avg_frame_qp);
      }
      enc->encoder_eof = xpkt->end_of_stream;
      if (enc->encoder_eof &&
          LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
          p_ctx->session_run_state) {
        // after sequence change completes, reset codec state
        GST_DEBUG ("xcoder_receive_packet 2: sequence change "
            "completed, return 0 and will reopen codec !\n");
        ret = xcoder_encode_reinit (enc);
        GST_DEBUG ("xcoder_receive_packet: xcoder_encode_reinit ret %d\n", ret);
        if (ret >= 0) {
          xcoder_send_frame (enc, NULL);
          p_ctx->session_run_state = LOGAN_SESSION_RUN_STATE_NORMAL;
        }
      }
      break;
    }
  }

  GST_DEBUG ("xcoder_receive_packet: return %d\n", ret);
  return ret;
}

static void
gst_niloganenc_free_pkt (gpointer pkt)
{
  g_free (pkt);
}

static GstFlowReturn
gst_niloganenc_receive_packet (GstNiloganEnc * enc, gboolean * got_packet,
    gboolean send)
{
  GstBuffer *outbuf;
  ni_logan_session_data_io_t *p_pkt =
      gst_nilogan_context_get_data_pkt (enc->context);
  GstVideoCodecFrame *frame;
  gint res;
  uint8_t *p_data = NULL;
  int got_pkt_size = 0;
  GstFlowReturn ret = GST_FLOW_OK;

  *got_packet = FALSE;

  ni_logan_packet_t *xpkt = &(p_pkt->data.packet);

  p_data = g_malloc0 (NI_LOGAN_MAX_TX_SZ);

  res = xcoder_receive_packet (enc, xpkt, p_data, &got_pkt_size);

  if (res == NI_EAGAIN) {       // EAGAIN
    //ni_logan_packet_buffer_free (xpkt);
    g_free (p_data);
    goto done;
  } else if (res == NI_EOF) {   //EOF
    g_free (p_data);
    ret = GST_FLOW_EOS;
    goto done;
  } else if (res < 0) {
    ret = GST_FLOW_ERROR;
    g_free (p_data);
    goto done;
  }

  *got_packet = TRUE;

  GList *list = gst_video_encoder_get_frames (GST_VIDEO_ENCODER (enc));

  if (!list) {
    GST_ERROR_OBJECT (enc, "failed to get list of frame");
    return GST_FLOW_ERROR;
  }

  GstClockTime searchPts = xpkt->pts;
  frame = gst_niloganenc_find_best_frame (enc, list, searchPts);
  if (!frame) {
    frame = gst_video_encoder_get_oldest_frame (GST_VIDEO_ENCODER (enc));
  }

  if (send) {
    outbuf = gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY, p_data,
        got_pkt_size, 0, got_pkt_size, p_data, gst_niloganenc_free_pkt);
    frame->output_buffer = outbuf;
    frame->pts = xpkt->pts;
    frame->dts = xpkt->dts;

    enc->encoder_eof = xpkt->end_of_stream;

    GST_DEBUG
        ("xcoder_pts=%lld, xcoder_dts=%lld,output_Pts=%ld,output_Dts=%ld,num=%d",
        xpkt->pts, xpkt->dts, frame->pts, frame->dts,
        frame->system_frame_number);

    if (xpkt->frame_type == 0)
      GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT (frame);
    else
      GST_VIDEO_CODEC_FRAME_UNSET_SYNC_POINT (frame);
  }

  ret = gst_video_encoder_finish_frame (GST_VIDEO_ENCODER (enc), frame);

  GST_DEBUG ("Try to finish frame,size=%d,send=%d,ret=%d,num=%d,ref=%d\n",
      got_pkt_size, send, ret, frame->system_frame_number, frame->ref_count);

  g_list_free_full (list, (GDestroyNotify) gst_video_codec_frame_unref);

  //gst_video_codec_frame_unref(frame);

  //gst_niloganenc_dequeue_frame (enc, frame);

done:
  GST_DEBUG ("Simon ret=%d,res=%d,got_packet=%d", ret, res, *got_packet);
  return ret;
}

static GstFlowReturn
gst_niloganenc_handle_frame (GstVideoEncoder * encoder,
    GstVideoCodecFrame * frame)
{
  GST_DEBUG ("encoder handle frame,PTS=%lu,DTS=%lu,num=%d\n", frame->pts,
      frame->dts, frame->system_frame_number);
  GstNiloganEnc *thiz = GST_NILOGANENC (encoder);
  gboolean got_packet;
  GstFlowReturn ret;

  thiz->ticks_per_frame = frame->duration;

  ret = xcoder_send_frame (thiz, frame);

  if (ret != GST_FLOW_OK)
    goto encode_send_fail;

  gst_video_codec_frame_unref (frame);

  do {
    ret = gst_niloganenc_receive_packet (thiz, &got_packet, TRUE);
    if (ret != GST_FLOW_OK)
      break;
  } while (got_packet);

done:
  return ret;

encode_send_fail:
  {
    GST_DEBUG ("Encode send fail\n");
    gst_video_codec_frame_unref (frame);
    ret = GST_FLOW_ERROR;
    goto done;
  }
}

static GstFlowReturn
gst_niloganenc_flush_buffers (GstNiloganEnc * nienc, gboolean send)
{
  GST_DEBUG ("Flush buffers,send=%d\n", send);
  GstFlowReturn ret = GST_FLOW_OK;
  gboolean got_packet;

  GST_DEBUG_OBJECT (nienc, "flushing buffers with sending %d", send);

  /* no need to empty codec if there is none */
  if (!nienc->started)
    goto done;

  ret = xcoder_send_frame (nienc, NULL);

  if (ret != GST_FLOW_OK)
    goto done;

  do {
    ret = gst_niloganenc_receive_packet (nienc, &got_packet, send);
    if (ret != GST_FLOW_OK)
      break;
  } while (got_packet);

done:
  return ret;
}

static gboolean
gst_niloganenc_stop (GstVideoEncoder * encoder)
{
  GstNiloganEnc *nienc = (GstNiloganEnc *) encoder;

  gst_niloganenc_flush_buffers (nienc, FALSE);
  gst_niloganenc_close (nienc);
  nienc->started = 0;

  if (nienc->input_state) {
    gst_video_codec_state_unref (nienc->input_state);
    nienc->input_state = NULL;
  }

  if (nienc->xcoder_opts) {
    g_free (nienc->xcoder_opts);
    nienc->xcoder_opts = NULL;
  }

  if (nienc->dev_xcoder) {
    g_free (nienc->dev_xcoder);
    nienc->dev_xcoder = NULL;
  }

  if (nienc->xcoder_gop) {
    g_free (nienc->xcoder_gop);
    nienc->xcoder_gop = NULL;
  }

  if (nienc->dev_xcoder_name) {
    g_free (nienc->dev_xcoder_name);
    nienc->dev_xcoder_name = NULL;
  }

  return TRUE;
}

static GstFlowReturn
gst_niloganenc_finish (GstVideoEncoder * encoder)
{
  GstNiloganEnc *nienc = (GstNiloganEnc *) encoder;

  return gst_niloganenc_flush_buffers (nienc, TRUE);
}

static void
gst_niloganenc_finalize (GObject * object)
{
  /* clean up remaining allocated data */
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_niloganenc_sink_query (GstVideoEncoder * enc, GstQuery * query)
{
  GstPad *pad = GST_VIDEO_ENCODER_SINK_PAD (enc);
  gboolean ret = FALSE;

  GST_DEBUG ("Received %s query on sinkpad, %" GST_PTR_FORMAT,
      GST_QUERY_TYPE_NAME (query), query);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_ACCEPT_CAPS:{
      GstCaps *acceptable, *caps;

      acceptable = gst_pad_get_pad_template_caps (pad);

      gst_query_parse_accept_caps (query, &caps);

      gst_query_set_accept_caps_result (query,
          gst_caps_is_subset (caps, acceptable));
      gst_caps_unref (acceptable);
      ret = TRUE;
    }
      break;
    default:
      ret = GST_VIDEO_ENCODER_CLASS (parent_class)->sink_query (enc, query);
      break;
  }

  return ret;
}

// class_init
static void
gst_niloganenc_class_init (GstNiloganEncClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstVideoEncoderClass *gstencoder_class;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  gstencoder_class = GST_VIDEO_ENCODER_CLASS (klass);

  element_class->set_context = gst_niloganenc_set_context;

  gstencoder_class->set_format = GST_DEBUG_FUNCPTR (gst_niloganenc_set_format);
  gstencoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_niloganenc_handle_frame);
  gstencoder_class->start = GST_DEBUG_FUNCPTR (gst_niloganenc_start);
  gstencoder_class->stop = GST_DEBUG_FUNCPTR (gst_niloganenc_stop);
  gstencoder_class->flush = GST_DEBUG_FUNCPTR (gst_niloganenc_flush);
  gstencoder_class->finish = GST_DEBUG_FUNCPTR (gst_niloganenc_finish);
  gstencoder_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_niloganenc_propose_allocation);
  gstencoder_class->getcaps = GST_DEBUG_FUNCPTR (gst_niloganenc_sink_getcaps);
  gstencoder_class->sink_query = GST_DEBUG_FUNCPTR (gst_niloganenc_sink_query);
  gobject_class->finalize = gst_niloganenc_finalize;
}

static gboolean
gst_niloganenc_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstVideoEncoder *videoenc = NULL;
  GstVideoEncoderClass *videoencclass = NULL;
  GstNiloganEnc *nienc = (GstNiloganEnc *) parent;
  gboolean ret = FALSE;

  videoenc = GST_VIDEO_ENCODER (parent);
  videoencclass = GST_VIDEO_ENCODER_GET_CLASS (videoenc);

  if (GST_EVENT_CAPS == GST_EVENT_TYPE (event)) {
    GstCaps *caps;
    GstStructure *structure;
    gint hw_fmt = 0;

    gst_event_parse_caps (event, &caps);
    structure = gst_caps_get_structure (caps, 0);
    gst_structure_get_int (structure, "hw_pix_fmt", &hw_fmt);
    if (hw_fmt == PIX_FMT_NI_LOGAN) {
      nienc->hardware_mode = true;
    }
    // TODO: Disable hw mode for now
    if (nienc->hardware_mode) {
      return FALSE;
    }
  }


  GST_DEBUG_OBJECT (videoenc, "received event %d, %s", GST_EVENT_TYPE (event),
      GST_EVENT_TYPE_NAME (event));

  if (videoencclass->sink_event)
    ret = videoencclass->sink_event (videoenc, event);

  return ret;
}

static void
gst_niloganenc_init (GstNiloganEnc * nienc)
{
  GstVideoEncoder *videoenc = NULL;

  videoenc = GST_VIDEO_ENCODER (nienc);

  nienc->keep_alive_timeout = PROP_ENC_TIMEOUT;
  nienc->dev_enc_idx = PROP_ENC_INDEX;
  nienc->hardware_mode = false;
  nienc->dev_xcoder = g_strdup (PROP_ENC_XCODER);

  gst_pad_set_event_function (videoenc->sinkpad, gst_niloganenc_sink_event);
  GST_PAD_SET_ACCEPT_TEMPLATE (GST_VIDEO_ENCODER_SINK_PAD (nienc));
}

gboolean
gst_niloganenc_set_common_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiloganEnc *nienc = GST_NILOGANENC (object);
  GstState state;
  gboolean ret = TRUE;

  GST_OBJECT_LOCK (nienc);
  state = GST_STATE (nienc);
  if ((state != GST_STATE_READY && state != GST_STATE_NULL) &&
      !(pspec->flags & GST_PARAM_MUTABLE_PLAYING)) {
    ret = FALSE;
    goto wrong_state;
  }

  switch (prop_id) {
    case GST_NILOGAN_ENC_PROP_NAME:
      g_free (nienc->dev_xcoder_name);
      nienc->dev_xcoder_name = g_strdup (g_value_get_string (value));
      break;
    case GST_NILOGAN_ENC_PROP_XCODER:
      g_free (nienc->dev_xcoder);
      nienc->dev_xcoder = g_strdup (g_value_get_string (value));
      break;
    case GST_NILOGAN_ENC_PROP_CARD_NUM:
      nienc->dev_enc_idx = g_value_get_int (value);
      break;
    case GST_NILOGAN_ENC_PROP_TIMEOUT:
      nienc->keep_alive_timeout = g_value_get_uint (value);
      break;
    case GST_NILOGAN_ENC_PROP_HIGH_PRIORITY:
      nienc->set_high_priority = g_value_get_int (value);
      break;
    case GST_NILOGAN_ENC_PROP_XCODER_PARAM:
      g_free (nienc->xcoder_opts);
      nienc->xcoder_opts = g_strdup (g_value_get_string (value));
      break;
    case GST_NILOGAN_ENC_PROP_XCODER_GOP:
      g_free (nienc->xcoder_gop);
      nienc->xcoder_gop = g_strdup (g_value_get_string (value));
      break;
    default:
      ret = FALSE;
      return ret;
  }
  GST_OBJECT_UNLOCK (nienc);
  return ret;

wrong_state:
  {
    GST_WARNING_OBJECT (nienc, "setting property in wrong state");
    GST_OBJECT_UNLOCK (nienc);
    return ret;
  }
}

gboolean
gst_niloganenc_get_common_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiloganEnc *nienc = GST_NILOGANENC (object);
  gboolean ret = TRUE;

  GST_OBJECT_LOCK (nienc);
  switch (prop_id) {
    case GST_NILOGAN_ENC_PROP_CARD_NUM:
      g_value_set_int (value, nienc->dev_enc_idx);
      break;
    case GST_NILOGAN_ENC_PROP_XCODER:
      g_value_set_string (value, nienc->dev_xcoder);
      break;
    case GST_NILOGAN_ENC_PROP_NAME:
      g_value_set_string (value, nienc->dev_xcoder_name);
      break;
    case GST_NILOGAN_ENC_PROP_HIGH_PRIORITY:
      g_value_set_int (value, nienc->set_high_priority);
      break;
    case GST_NILOGAN_ENC_PROP_TIMEOUT:
      g_value_set_uint (value, nienc->keep_alive_timeout);
      break;
    case GST_NILOGAN_ENC_PROP_XCODER_PARAM:
      g_value_set_string (value, nienc->xcoder_opts);
      break;
    case GST_NILOGAN_ENC_PROP_XCODER_GOP:
      g_value_set_string (value, nienc->xcoder_gop);
      break;
    default:
      ret = FALSE;
      break;
  }
  GST_OBJECT_UNLOCK (nienc);
  return ret;
}

void
gst_niloganenc_install_common_properties (GstNiloganEncClass * enc_class)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (enc_class);
  GParamSpec *obj_properties[GST_NILOGAN_ENC_PROP_MAX] = { NULL, };

  obj_properties[GST_NILOGAN_ENC_PROP_CARD_NUM] =
      g_param_spec_int ("enc", "Enc",
      "Select which encoder to use by index. First is 0, second is 1, and so on",
      -1, INT_MAX, PROP_ENC_INDEX, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NILOGAN_ENC_PROP_NAME] =
      g_param_spec_string ("device-name", "Device-Name",
      "Select which encoder to use by NVMe block device name, e.g. /dev/nvme0n1",
      PROP_ENC_NAME, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);


  obj_properties[GST_NILOGAN_ENC_PROP_HIGH_PRIORITY] =
      g_param_spec_int ("set-high-priority", "HighPriority",
      "Specify a custom session set high priority in 0 or 1 (from 0 to 1) (default 0)",
      0, 1, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NILOGAN_ENC_PROP_XCODER_PARAM] =
      g_param_spec_string ("xcoder-params", "XCODER-PARAMS",
      "Set the XCoder configuration using a :-separated list of key=value parameters",
      NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NILOGAN_ENC_PROP_XCODER_GOP] =
      g_param_spec_string ("xcoder-gop", "XCODER-GOP",
      "Set the XCoder custom gop using a :-separated list of key=value parameters",
      NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NILOGAN_ENC_PROP_TIMEOUT] =
      g_param_spec_uint ("keep-alive-timeout", "TIMEOUT",
      "Specify a custom session keep alive timeout in seconds.", 0, INT_MAX,
      PROP_ENC_TIMEOUT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NILOGAN_ENC_PROP_XCODER] =
      g_param_spec_string ("xcoder", "Xcoder",
      "Select which XCoder card to use",
      PROP_ENC_XCODER, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, GST_NILOGAN_ENC_PROP_MAX,
      obj_properties);
}
