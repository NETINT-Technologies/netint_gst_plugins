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
 *  \file   gstniquadraenc.c
 *
 *  \brief  Implement of NetInt Quadra common encoder.
 ******************************************************************************/

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <glib/gstdio.h>

#include <gst/gst.h>
#include <gst/video/video.h>
#include <ni_util.h>
#include <ni_av_codec.h>

#include "gstniquadraenc.h"
#include "gstniquadramemory.h"
#include "gstnimetadata.h"

GST_DEBUG_CATEGORY_STATIC (gst_niquadraenc_debug);
#define GST_CAT_DEFAULT gst_niquadraenc_debug

#if ENABLE_NI_GST_FRAMEWORK_PATCH
#define GST_NI_QUADRA_FORMAT_IS_TILED(format) \
  ((format) == GST_VIDEO_FORMAT_NI_QUAD_8_4L4 || \
      (format) == GST_VIDEO_FORMAT_NI_QUAD_10_4L4)
#else
#define GST_NI_QUADRA_FORMAT_IS_TILED(format) FALSE
#endif

#define GST_NIQUADRA_CAPS_MAKE(format) \
  GST_VIDEO_CAPS_MAKE (format) ", " \
  "interlace-mode = (string) progressive"
#define GST_NIQUADRA_CAPS_MAKE_WITH_DMABUF_FEATURE(dmaformat) ""

#define GST_NIQUADRA_CAPS_STR(format,dmaformat) \
  GST_NIQUADRA_CAPS_MAKE (format) "; " \
  GST_NIQUADRA_CAPS_MAKE_WITH_DMABUF_FEATURE (dmaformat)

#define PROP_ENC_INDEX -1
#define PROP_ENC_IO_SIZE -1

#define gst_niquadraenc_parent_class parent_class
G_DEFINE_TYPE (GstNiquadraEnc, gst_niquadraenc, GST_TYPE_VIDEO_ENCODER);

static inline void
ff_xcoder_strncpy (char *dst, const char *src, int max)
{
  if (dst && src && max) {
    *dst = '\0';
    strncpy (dst, src, max);
    *(dst + max - 1) = '\0';
  }
}

static void
gst_niquadraenc_free_input_frame (GstNiquadraEnc * thiz, uint16_t frameIndex)
{
  if (!thiz->hardware_mode)
    return;

  GList *frames = gst_video_encoder_get_frames (GST_VIDEO_ENCODER (thiz));
  GList *iter;

  for (iter = frames; iter; iter = g_list_next (iter)) {
    GstVideoCodecFrame *frame = (GstVideoCodecFrame *) iter->data;

    if (!frame->input_buffer) {
      continue;
    }

    if (gst_buffer_n_memory (frame->input_buffer) < 1) {
      continue;
    }

    GstMemory *nimem = gst_buffer_peek_memory (frame->input_buffer, 0);
    niFrameSurface1_t *surface = gst_surface_from_ni_hw_memory (nimem);
    if (surface->ui16FrameIdx == frameIndex) {
      GstBuffer *old_buffer = frame->input_buffer;
      frame->input_buffer = gst_buffer_make_writable (frame->input_buffer);

      // when old_buffer's ref count is 1, below condition will meet
      if (old_buffer == frame->input_buffer) {
        gst_buffer_remove_memory (frame->input_buffer, 0);
      }

      break;
    }
  }

  g_list_free_full (frames, (GDestroyNotify) gst_video_codec_frame_unref);
}

const uint8_t *
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

static void gst_niquadraenc_class_init (GstNiquadraEncClass * klass);

static GstVideoCodecFrame *
gst_niquadraenc_find_best_frame (GstNiquadraEnc * enc, guint32 frame_num)
{
  GList *frames, *iter;
  GstVideoCodecFrame *ret = NULL;

  frames = gst_video_encoder_get_frames (GST_VIDEO_ENCODER (enc));
  for (iter = frames; iter; iter = g_list_next (iter)) {
    GstVideoCodecFrame *frame = (GstVideoCodecFrame *) iter->data;
    if (frame->system_frame_number == frame_num) {
      ret = gst_video_codec_frame_ref (frame);
      break;
    }
  }

  if (frames) {
    g_list_free_full (frames, (GDestroyNotify) gst_video_codec_frame_unref);
  }

  return ret;
}

static void
gst_niquadraenc_set_context (GstElement * element, GstContext * context)
{
  GST_ELEMENT_CLASS (parent_class)->set_context (element, context);
}

static NiPluginError
xcoder_encoder_header_check_set (GstNiquadraEnc * nienc)
{
  ni_session_context_t *p_ctx =
      gst_niquadra_context_get_session_context (nienc->context);
  ni_xcoder_params_t *p_enc_params = p_ctx->p_session_config;
  int color_primaries, color_trc, color_space;
  color_primaries = map_gst_color_primaries (nienc->colorimetry.primaries);
  color_trc = map_gst_color_trc (nienc->colorimetry.transfer);
  color_space = map_gst_color_space (nienc->colorimetry.matrix);

  p_enc_params->cfg_enc_params.videoFullRange = 0;

  if (5 == p_enc_params->dolby_vision_profile) {
    switch (nienc->codec_format) {
      case NI_CODEC_FORMAT_H265:
        color_primaries = 2;
        color_trc = 2;
        color_space = 2;
        p_enc_params->cfg_enc_params.hrdEnable =
            p_enc_params->cfg_enc_params.EnableAUD = 1;
        p_enc_params->cfg_enc_params.forced_header_enable = 1;
        p_enc_params->cfg_enc_params.videoFullRange = 1;
        break;
      case NI_CODEC_FORMAT_H264:
        GST_ERROR_OBJECT (nienc,
            "dolbyVisionProfile is not supported on h264 encoder.\n");
        return NI_PLUGIN_UNSUPPORTED;
      case NI_CODEC_FORMAT_VP9:
        GST_ERROR_OBJECT (nienc,
            "dolbyVisionProfile is not supported on VP9 encoder.\n");
        return NI_PLUGIN_UNSUPPORTED;
      case NI_CODEC_FORMAT_JPEG:
        GST_ERROR_OBJECT (nienc,
            "dolbyVisionProfile is not supported on JPEG encoder.\n");
        return NI_PLUGIN_UNSUPPORTED;
      case NI_CODEC_FORMAT_AV1:
        GST_ERROR_OBJECT (nienc,
            "dolbyVisionProfile is not supported on av1 encoder.\n");
        return NI_PLUGIN_UNSUPPORTED;
      default:
        return NI_PLUGIN_UNSUPPORTED;
    }
  }

  if (nienc->codec_format != NI_CODEC_FORMAT_JPEG &&
      ((5 == p_enc_params->dolby_vision_profile &&
              nienc->codec_format == NI_CODEC_FORMAT_H265) ||
          color_primaries != GST_VIDEO_COLOR_PRIMARIES_UNKNOWN ||
          color_trc != GST_VIDEO_TRANSFER_UNKNOWN ||
          color_space != GST_VIDEO_COLOR_MATRIX_UNKNOWN)) {
    p_enc_params->cfg_enc_params.colorDescPresent = 1;
    p_enc_params->cfg_enc_params.colorPrimaries = color_primaries;
    p_enc_params->cfg_enc_params.colorTrc = color_trc;
    p_enc_params->cfg_enc_params.colorSpace = color_space;

    GST_DEBUG_OBJECT (nienc,
        "XCoder HDR color info color_primaries: %d color_trc: %d  color_space %d\n",
        color_primaries, color_trc, color_space);
  }

  if (nienc->codec_format == NI_CODEC_FORMAT_JPEG) {
    p_enc_params->cfg_enc_params.videoFullRange = 1;
  }

  return NI_PLUGIN_OK;
}

static gint
gst_niquadraenc_parse_xcoder_params (GstNiquadraEnc * nienc,
    const gchar * xcoder_opts, ni_xcoder_params_t * p_param,
    ni_session_context_t * p_ctx)
{
  gchar **entries = NULL;
  gchar **entry = NULL;
  gint ret = NI_RETCODE_SUCCESS;

  if (!xcoder_opts || !*xcoder_opts) {
    return NI_RETCODE_SUCCESS;
  }

  entries = g_strsplit (xcoder_opts, ":", -1);
  for (entry = entries; entry && *entry; entry++) {
    gchar *pair = NULL;
    gchar *separator = NULL;
    gchar *key = NULL;
    gchar *value = NULL;

    if (!**entry) {
      continue;
    }

    pair = g_strdup (*entry);
    separator = strchr (pair, '=');
    if (!separator) {
      GST_ERROR_OBJECT (nienc,
          "Error: xcoder-params p_config key/value not retrieved: %s\n", pair);
      g_free (pair);
      ret = -1;
      break;
    }

    *separator = '\0';
    key = pair;
    value = separator + 1;

    ret = ni_encoder_params_set_value (p_param, key, value);
    if (ret != NI_RETCODE_SUCCESS) {
      switch (ret) {
        case NI_RETCODE_PARAM_INVALID_NAME:
          GST_ERROR_OBJECT (nienc, "Unknown option: %s.\n", key);
          break;
        case NI_RETCODE_PARAM_ERROR_TOO_BIG:
          GST_ERROR_OBJECT (nienc, "Invalid %s: too big\n", key);
          break;
        case NI_RETCODE_PARAM_ERROR_TOO_SMALL:
          GST_ERROR_OBJECT (nienc, "Invalid %s: too small\n", key);
          break;
        case NI_RETCODE_PARAM_ERROR_OOR:
          GST_ERROR_OBJECT (nienc, "Invalid %s: out of range\n", key);
          break;
        case NI_RETCODE_PARAM_ERROR_ZERO:
          GST_ERROR_OBJECT (nienc, "Error setting option %s to value 0\n", key);
          break;
        case NI_RETCODE_PARAM_INVALID_VALUE:
          GST_ERROR_OBJECT (nienc, "Invalid value for %s: %s.\n", key, value);
          break;
        case NI_RETCODE_PARAM_WARNING_DEPRECATED:
          GST_WARNING_OBJECT (nienc, "Parameter %s is deprecated\n", key);
          ret = NI_RETCODE_SUCCESS;
          break;
        default:
          GST_ERROR_OBJECT (nienc, "Invalid %s: ret %d\n", key, ret);
          break;
      }
    }

    g_free (pair);

    if (ret != NI_RETCODE_SUCCESS) {
      break;
    }
  }
  g_strfreev (entries);

  if (ret == NI_RETCODE_SUCCESS) {
    p_ctx->keep_alive_timeout = p_param->cfg_enc_params.keep_alive_timeout;
    /* Reuse decoder_low_delay for low delay encoding wait interval. */
    p_ctx->decoder_low_delay = p_param->low_delay_mode;
  }

  return ret;
}

static gboolean
gst_niquadraenc_open_xcoder (GstVideoEncoder * encoder,
    GstVideoCodecState * state)
{
  GstNiquadraEnc *nienc = (GstNiquadraEnc *) encoder;
  GstNiquadraEncClass *oclass = (GstNiquadraEncClass *)
      G_OBJECT_GET_CLASS (nienc);
  ni_session_context_t *p_ctx =
      gst_niquadra_context_get_session_context (nienc->context);
  ni_xcoder_params_t *p_param =
      gst_niquadra_context_get_xcoder_param (nienc->context);
  ni_session_data_io_t *p_pkt =
      gst_niquadra_context_get_data_pkt (nienc->context);

  GST_DEBUG_OBJECT (nienc, "XCoder encode init\n");
  ni_retcode_t res = NI_RETCODE_SUCCESS;

  ni_xcoder_params_t *pparams = NULL;
  int is_bigendian = NI_FRAME_LITTLE_ENDIAN;
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

  if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING != p_ctx->session_run_state) {
    GST_DEBUG_OBJECT (encoder, "Session state: %d allocate frame fifo.\n",
        p_ctx->session_run_state);
  } else {
    GST_DEBUG_OBJECT (encoder, "Session seq change, fifo size:\n");
  }
  nienc->eos_fme_received = 0;

  ni_pix_fmt_t ni_format = NI_PIX_FMT_NONE;
  switch (state->info.finfo->format) {
    case GST_VIDEO_FORMAT_I420:
      ni_format = NI_PIX_FMT_YUV420P;
      break;
    case GST_VIDEO_FORMAT_I420_10LE:
      ni_format = NI_PIX_FMT_YUV420P10LE;
      break;
    case GST_VIDEO_FORMAT_NV12:
      ni_format = NI_PIX_FMT_NV12;
      break;
    case GST_VIDEO_FORMAT_P010_10LE:
      ni_format = NI_PIX_FMT_P010LE;
      break;
    case GST_VIDEO_FORMAT_P010_10BE:
      ni_format = NI_PIX_FMT_P010LE;
      is_bigendian = NI_FRAME_BIG_ENDIAN;
      break;
    case GST_VIDEO_FORMAT_I420_10BE:
      ni_format = NI_PIX_FMT_YUV420P10LE;
      is_bigendian = NI_FRAME_BIG_ENDIAN;
      break;
    case GST_VIDEO_FORMAT_RGBA:
      ni_format = NI_PIX_FMT_RGBA;
      break;
    case GST_VIDEO_FORMAT_ARGB:
      ni_format = NI_PIX_FMT_ARGB;
      break;
    case GST_VIDEO_FORMAT_ABGR:
      ni_format = NI_PIX_FMT_ABGR;
      break;
    case GST_VIDEO_FORMAT_BGRA:
      ni_format = NI_PIX_FMT_BGRA;
      break;
#if ENABLE_NI_GST_FRAMEWORK_PATCH
    case GST_VIDEO_FORMAT_NI_QUAD_8_4L4:
      ni_format = NI_PIX_FMT_8_TILED4X4;
      break;
    case GST_VIDEO_FORMAT_NI_QUAD_10_4L4:
      ni_format = NI_PIX_FMT_10_TILED4X4;
      break;
#endif
    default:
      GST_ERROR_OBJECT (nienc,
          "Ni enc don't support [%d]", state->info.finfo->format);
      return FALSE;
  }
  p_ctx->codec_format = nienc->codec_format;
  GST_DEBUG_OBJECT (nienc,
      "fps:%d/%d,res=%dx%d,format=%d,p=%p",
      state->info.fps_n, state->info.fps_d, state->info.width,
      state->info.height, ni_format, p_param);
  res = ni_encoder_init_default_params (p_param, state->info.fps_n,
      state->info.fps_d, 200000, state->info.width, state->info.height,
      p_ctx->codec_format);
  switch (res) {
    case NI_RETCODE_PARAM_ERROR_WIDTH_TOO_BIG:
      if (p_ctx->codec_format == NI_CODEC_FORMAT_AV1)
        GST_ERROR_OBJECT (nienc,
            "Invalid Picture Width: exceeds %d\n", NI_PARAM_AV1_MAX_WIDTH);
      else
        GST_ERROR_OBJECT (nienc, "Invalid Picture Width: too big\n");
      return FALSE;
    case NI_RETCODE_PARAM_ERROR_WIDTH_TOO_SMALL:
      GST_ERROR_OBJECT (nienc, "Invalid Picture Width: too small\n");
      return FALSE;
    case NI_RETCODE_PARAM_ERROR_HEIGHT_TOO_BIG:
      if (p_ctx->codec_format == NI_CODEC_FORMAT_AV1)
        GST_ERROR_OBJECT (nienc,
            "Invalid Picture Height: exceeds %d\n", NI_PARAM_AV1_MAX_HEIGHT);
      else
        GST_ERROR_OBJECT (nienc, "Invalid Picture Height: too big\n");
      return FALSE;
    case NI_RETCODE_PARAM_ERROR_HEIGHT_TOO_SMALL:
      GST_ERROR_OBJECT (nienc, "Invalid Picture Height: too small\n");
      return FALSE;
    case NI_RETCODE_PARAM_ERROR_AREA_TOO_BIG:
      if (p_ctx->codec_format == NI_CODEC_FORMAT_AV1)
        GST_ERROR_OBJECT (nienc,
            "Invalid Picture Width x Height: exceeds %d\n",
            NI_PARAM_AV1_MAX_AREA);
      else
        GST_ERROR_OBJECT (nienc,
            "Invalid Picture Width x Height: exceeds %d\n",
            NI_MAX_RESOLUTION_AREA);
      return FALSE;
    case NI_RETCODE_PARAM_ERROR_PIC_WIDTH:
      GST_ERROR_OBJECT (nienc, "Invalid Picture Width\n");
      return FALSE;
    case NI_RETCODE_PARAM_ERROR_PIC_HEIGHT:
      GST_ERROR_OBJECT (nienc, "Invalid Picture Height\n");
      return FALSE;
    default:
      if (res < 0) {
        int i;
        GST_ERROR_OBJECT (nienc, "Error setting preset or log.\n");
        GST_LOG ("Possible presets:");
        for (i = 0; g_xcoder_preset_names[i]; i++)
          GST_LOG (" %s", g_xcoder_preset_names[i]);
        GST_LOG ("\n");

        GST_LOG ("Possible log:");
        for (i = 0; g_xcoder_log_names[i]; i++)
          GST_LOG (" %s", g_xcoder_log_names[i]);
        GST_LOG ("\n");

        return FALSE;
      }
      break;
  }

  if (nienc->hardware_mode) {
    if (nienc->width >= NI_MIN_WIDTH && nienc->height >= NI_MIN_HEIGHT) {
      p_param->hwframes = 1;
    }
    if (nienc->codec_format == NI_CODEC_FORMAT_JPEG) {
      if (nienc->colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255
          || (nienc->colorimetry.range == GST_VIDEO_COLOR_RANGE_UNKNOWN
              && (nienc->pix_fmt == GST_VIDEO_FORMAT_I420
                  || nienc->pix_fmt == GST_VIDEO_FORMAT_I420_10LE
                  || nienc->pix_fmt == GST_VIDEO_FORMAT_NV12
                  || nienc->pix_fmt == GST_VIDEO_FORMAT_P010_10LE))) {
        GST_DEBUG_OBJECT (nienc,
            "Pixfmt %s supported in JPEG encoder when color_range is GST_VIDEO_COLOR_RANGE_16_235\n",
            gst_video_format_to_string (nienc->pix_fmt));
      } else {
        GST_WARNING_OBJECT (nienc,
            "Pixfmt %s not supported in JPEG encoder when color_range is %d\n",
            gst_video_format_to_string (nienc->pix_fmt),
            nienc->colorimetry.range);
        return FALSE;
      }
    }
  }

  if (nienc->hardware_mode && nienc->width >= NI_MIN_WIDTH
      && nienc->height >= NI_MIN_HEIGHT) {
    p_param->hwframes = 1;
  }

  /* set pixel_format */
  switch (nienc->pix_fmt) {
    case GST_VIDEO_FORMAT_I420:
      p_ctx->pixel_format = NI_PIX_FMT_YUV420P;
      break;
    case GST_VIDEO_FORMAT_I420_10LE:
      p_ctx->pixel_format = NI_PIX_FMT_YUV420P10LE;
      break;
    case GST_VIDEO_FORMAT_NV12:
      p_ctx->pixel_format = NI_PIX_FMT_NV12;
      break;
    case GST_VIDEO_FORMAT_P010_10LE:
      p_ctx->pixel_format = NI_PIX_FMT_P010LE;
      break;
    case GST_VIDEO_FORMAT_RGBA:
      p_ctx->pixel_format = NI_PIX_FMT_RGBA;
      break;
    case GST_VIDEO_FORMAT_ARGB:
      p_ctx->pixel_format = NI_PIX_FMT_ARGB;
      break;
    case GST_VIDEO_FORMAT_ABGR:
      p_ctx->pixel_format = NI_PIX_FMT_ABGR;
      break;
    case GST_VIDEO_FORMAT_BGRA:
      p_ctx->pixel_format = NI_PIX_FMT_BGRA;
      break;
#if ENABLE_NI_GST_FRAMEWORK_PATCH
    case GST_VIDEO_FORMAT_NI_QUAD_8_4L4:
      p_ctx->pixel_format = NI_PIX_FMT_8_TILED4X4;
      break;
    case GST_VIDEO_FORMAT_NI_QUAD_10_4L4:
      p_ctx->pixel_format = NI_PIX_FMT_10_TILED4X4;
      break;
#endif
    default:
      break;
  }

  if (nienc->xcoder_opts) {
    int ret = gst_niquadraenc_parse_xcoder_params (nienc, nienc->xcoder_opts,
        p_param, p_ctx);
    if (ret != NI_RETCODE_SUCCESS) {
      GST_ERROR_OBJECT (nienc, "Error: encoder p_config parsing error\n");
      return FALSE;
    }
  }

  if (p_param->enable_vfr) {
    p_param->cfg_enc_params.frame_rate = 30;
    p_ctx->prev_fps = 30;
    p_ctx->last_change_framenum = 0;
    p_ctx->fps_change_detect_count = 0;
  }

  if (nienc->xcoder_gop) {
    char opts[2048];
    memset (opts, '\0', sizeof (opts));
    strcpy (opts, nienc->xcoder_gop);
    if (gst_ni_retrieve_gop_params (opts, p_param) < 0) {
      GST_ERROR_OBJECT (nienc, "Error: encoder p_config parsing error\n");
      return FALSE;
    }
  }
  if (nienc->nvme_io_size > 0 && nienc->nvme_io_size % 4096 != 0) {
    GST_ERROR_OBJECT (nienc, "Error XCoder iosize is not 4KB aligned!\n");
    return FALSE;
  }
  p_ctx->p_session_config = p_param;
  pparams = (ni_xcoder_params_t *) p_ctx->p_session_config;

  switch (pparams->cfg_enc_params.gop_preset_index) {
      /* dtsOffset is the max number of non-reference frames in a GOP
       * (derived from x264/5 algo) In case of IBBBP the first dts of the I
       * frame should be input_pts-(3*ticks_per_frame) In case of IBP the
       * first dts of the I frame should be input_pts-(1*ticks_per_frame)
       * thus we ensure pts>dts in all cases
       * */
    case 1:
    case 9:
    case 10:
      nienc->dtsOffset = 0;
      break;
      /* ts requires dts/pts of I fraem not same when there are B frames in
       * streams */
    case 3:
    case 4:
    case 7:
      nienc->dtsOffset = 1;
      break;
    case 5:
      nienc->dtsOffset = 2;
      break;
    case -1:                   // adaptive GOP
#if ((LIBXCODER_API_VERSION_MAJOR > 2) || \
    (LIBXCODER_API_VERSION_MAJOR == 2 && LIBXCODER_API_VERSION_MINOR >= 80))
    case -2:                   // not initialized stage
#endif
    case 8:
      nienc->dtsOffset = 3;
      break;
    default:
      nienc->dtsOffset = 7;
      break;
  }

  if (pparams->cfg_enc_params.custom_gop_params.custom_gop_size) {
    int dts_offset = 0;
    nienc->dtsOffset = 0;
    bool has_b_frame = false;
    for (int idx = 0;
        idx < pparams->cfg_enc_params.custom_gop_params.custom_gop_size;
        idx++) {
      if (pparams->cfg_enc_params.custom_gop_params.pic_param[idx].poc_offset <
          idx + 1) {
        dts_offset =
            (idx + 1) -
            pparams->cfg_enc_params.custom_gop_params.pic_param[idx].poc_offset;
        if (nienc->dtsOffset < dts_offset) {
          nienc->dtsOffset = dts_offset;
        }
      }

      if (!has_b_frame &&
          (pparams->cfg_enc_params.custom_gop_params.pic_param[idx].pic_type ==
              PIC_TYPE_B)) {
        has_b_frame = true;
      }
    }

    if (has_b_frame && !nienc->dtsOffset) {
      nienc->dtsOffset = 1;
    }
  }

  nienc->total_frames_received = 0;
  nienc->gop_offset_count = 0;

  GST_DEBUG_OBJECT (nienc,
      "dts offset: %ld, gop_offset_count: %d\n",
      nienc->dtsOffset, nienc->gop_offset_count);

  //overwrite the nvme io size here with a custom value if it was provided
  if (nienc->nvme_io_size > 0 && nienc->nvme_io_size % 4096 != 0) {
    p_ctx->max_nvme_io_size = nienc->nvme_io_size;
  }

  nienc->encoder_eof = 0;
  nienc->bit_rate = p_param->bitrate;
  p_ctx->src_bit_depth = 8;
  p_ctx->src_endian = NI_FRAME_LITTLE_ENDIAN;
  p_ctx->roi_len = 0;
  p_ctx->roi_avg_qp = 0;
  p_ctx->bit_depth_factor = 1;
  if (p_ctx->keep_alive_timeout != NI_DEFAULT_KEEP_ALIVE_TIMEOUT) {
    GST_DEBUG_OBJECT (nienc,
        "Default:%d, custom=%d",
        NI_DEFAULT_KEEP_ALIVE_TIMEOUT, nienc->keep_alive_timeout);
    p_ctx->keep_alive_timeout = nienc->keep_alive_timeout;
  }

  if (ni_format == NI_PIX_FMT_YUV420P10LE || ni_format == NI_PIX_FMT_P010LE ||
      ni_format == NI_PIX_FMT_10_TILED4X4) {
    p_ctx->bit_depth_factor = 2;
    p_ctx->src_bit_depth = 10;
    p_ctx->src_endian = is_bigendian;
  }

  switch (ni_format) {
    case NI_PIX_FMT_NV12:
    case NI_PIX_FMT_P010LE:
      pparams->cfg_enc_params.planar = NI_PIXEL_PLANAR_FORMAT_SEMIPLANAR;
      break;
    case NI_PIX_FMT_8_TILED4X4:
    case NI_PIX_FMT_10_TILED4X4:
      pparams->cfg_enc_params.planar = NI_PIXEL_PLANAR_FORMAT_TILED4X4;
      break;
    default:
      pparams->cfg_enc_params.planar = NI_PIXEL_PLANAR_FORMAT_PLANAR;
      break;
  }

  if (xcoder_encoder_header_check_set (nienc) != NI_PLUGIN_OK) {
    return FALSE;
  }
  // Use the value passed in from gstreamer if aspect ratio from xcoder-params have default values
  if ((p_param->cfg_enc_params.aspectRatioWidth == 0)
      && (p_param->cfg_enc_params.aspectRatioHeight == 1)) {
    p_param->cfg_enc_params.aspectRatioWidth =
        GST_VIDEO_INFO_PAR_N (&state->info);
    p_param->cfg_enc_params.aspectRatioHeight =
        GST_VIDEO_INFO_PAR_D (&state->info);
  }

  memset (p_pkt, 0, sizeof (ni_session_data_io_t));

  // init HDR SEI stuff
  p_ctx->sei_hdr_content_light_level_info_len = p_ctx->light_level_data_len
      = p_ctx->sei_hdr_mastering_display_color_vol_len =
      p_ctx->mdcv_max_min_lum_data_len = 0;
  p_ctx->p_master_display_meta_data = NULL;

  p_ctx->ori_width = state->info.width;
  p_ctx->ori_height = state->info.height;
  p_ctx->ori_bit_depth_factor = p_ctx->bit_depth_factor;
  p_ctx->ori_pix_fmt = 0;

  return TRUE;
}

void
gst_niquadraenc_videoinfo_to_context (GstVideoInfo * info,
    GstNiquadraEnc * nienc)
{
  gint i, bpp = 0;

  nienc->width = GST_VIDEO_INFO_WIDTH (info);
  nienc->height = GST_VIDEO_INFO_HEIGHT (info);
  for (i = 0; i < GST_VIDEO_INFO_N_COMPONENTS (info); i++)
    bpp += GST_VIDEO_INFO_COMP_DEPTH (info, i);
  nienc->bits_per_coded_sample = bpp;

  nienc->ticks_per_frame = 1;
  if (GST_VIDEO_INFO_FPS_N (info) == 0) {
    GST_DEBUG_OBJECT (nienc, "Using 25/1 framerate");
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
gst_niquadraenc_caps_new (GstNiquadraEnc * nienc, const char *mimetype,
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
    GST_DEBUG_OBJECT (nienc, "invalid framerate: %d/0, -> %d/1", num, num);
    denom = 1;
  }
  if (gst_util_fraction_compare (num, denom, 1000, 1) > 0) {
    GST_DEBUG_OBJECT (nienc, "excessive framerate: %d/%d, -> 0/1", num, denom);
    num = 0;
    denom = 1;
  }
  GST_DEBUG_OBJECT (nienc, "setting framerate: %d/%d", num, denom);
  gst_caps_set_simple (caps, "framerate", GST_TYPE_FRACTION, num, denom, NULL);
  if (!caps) {
    GST_DEBUG_OBJECT (nienc, "Creating default caps");
    caps = gst_caps_new_empty_simple (mimetype);
  }

  va_start (var_args, fieldname);
  gst_caps_set_simple_valist (caps, fieldname, var_args);
  va_end (var_args);
  return caps;
}

static void
gst_niquadraenc_close (GstVideoEncoder * encoder)
{
  ni_retcode_t ret = NI_RETCODE_SUCCESS;
  GstNiquadraEnc *nienc = (GstNiquadraEnc *) encoder;
  ni_session_context_t *p_ctx =
      gst_niquadra_context_get_session_context (nienc->context);
  ni_session_data_io_t *p_pkt =
      gst_niquadra_context_get_data_pkt (nienc->context);
  ni_session_data_io_t *p_frame =
      gst_niquadra_context_get_data_frame (nienc->context);

  if (nienc->opened) {
    ret =
        ni_device_session_close (p_ctx, nienc->encoder_eof,
        NI_DEVICE_TYPE_ENCODER);
    if (NI_RETCODE_SUCCESS != ret) {
      GST_ERROR_OBJECT (nienc,
          "Failed to close Encoder Session(status=%d)\n", ret);
    }
    nienc->opened = FALSE;
  }

  GST_DEBUG_OBJECT (nienc,
      "XCoder encode close: session_run_state %d\n", p_ctx->session_run_state);
  if (p_frame->data.frame.buffer_size
      || p_frame->data.frame.metadata_buffer_size
      || p_frame->data.frame.start_buffer_size) {
    ni_frame_buffer_free (&(p_frame->data.frame));
  }
  ni_packet_buffer_free (&(p_pkt->data.packet));

  if (nienc->codec_format == NI_CODEC_FORMAT_AV1) {
    ni_packet_buffer_free_av1 (&(p_pkt->data.packet));
  }

  if (nienc->p_spsPpsHdr) {
    free (nienc->p_spsPpsHdr);
    nienc->p_spsPpsHdr = NULL;
  }

  if (p_ctx->session_run_state != SESSION_RUN_STATE_SEQ_CHANGE_DRAINING) {
    gst_object_unref (nienc->context);
    nienc->context = NULL;
  }

  nienc->started = 0;
}

/*
 * For some elements (eg. fpsdisplay) the downstream element would try to lookup the caps from 
 * upstream elements, by default function would not accept the caps, we need to implement getcaps
 * function and select the allowed caps.
 * */
static GstCaps *
gst_niquadraenc_sink_getcaps (GstVideoEncoder * enc, GstCaps * filter)
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
    GstStructure *templ_s = gst_caps_get_structure (supported_incaps, i);
    GstCapsFeatures *templ_f = gst_caps_get_features (supported_incaps, i);
    GQuark q_name = gst_structure_get_name_id (templ_s);

    for (j = 0; j < gst_caps_get_size (allowed); j++) {
      const GstStructure *allowed_s = gst_caps_get_structure (allowed, j);
      const GValue *val;
      GstStructure *s;

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

      gst_caps_append_structure_full (filter_caps, s,
          gst_caps_features_copy (templ_f));
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
gst_niquadraenc_get_caps (GstVideoEncoder * encoder)
{
  GstNiquadraEnc *nienc = (GstNiquadraEnc *) encoder;
  GstCaps *caps = NULL;
  switch (nienc->codec_format) {
    case NI_CODEC_FORMAT_H264:
      caps = gst_niquadraenc_caps_new (nienc, "video/x-h264",
          "alignment", G_TYPE_STRING, "au", NULL);
      gst_caps_set_simple (caps, "stream-format", G_TYPE_STRING,
          "byte-stream", NULL);
      break;
    case NI_CODEC_FORMAT_H265:
      caps = gst_niquadraenc_caps_new (nienc, "video/x-h265",
          "alignment", G_TYPE_STRING, "au", NULL);
      gst_caps_set_simple (caps, "stream-format", G_TYPE_STRING,
          "byte-stream", NULL);
      break;
    case NI_CODEC_FORMAT_AV1:
      caps = gst_niquadraenc_caps_new (nienc, "video/x-av1", NULL);
      break;
    case NI_CODEC_FORMAT_JPEG:
      caps = gst_niquadraenc_caps_new (nienc, "image/jpeg", NULL);
      break;
    default:
      GST_WARNING_OBJECT (nienc, "Unsupported codec format: %d",
          nienc->codec_format);
      caps = NULL;
      break;
  }

  return caps;
}

static gboolean
gst_niquadraenc_set_format (GstVideoEncoder * encoder,
    GstVideoCodecState * state)
{
  GstCaps *other_caps;
  GstCaps *allowed_caps;
  GstCaps *icaps;
  GstVideoCodecState *output_format;
  GstNiquadraEnc *nienc = (GstNiquadraEnc *) encoder;

  // NI encoder don't support interlaced encoding now.
  if (GST_VIDEO_INFO_IS_INTERLACED (&state->info))
    return FALSE;

  GST_OBJECT_LOCK (encoder);

  if (nienc->started) {
    GST_DEBUG_OBJECT (nienc,
        "Format may changed when encoding process,reset here");
    if (nienc->input_state)
      gst_video_codec_state_unref (nienc->input_state);
    nienc->input_state = gst_video_codec_state_ref (state);
    GST_OBJECT_UNLOCK (encoder);
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
  gst_niquadraenc_videoinfo_to_context (&state->info, nienc);

  /* sanitize time base */
  if (nienc->time_base_num <= 0 || nienc->time_base_den <= 0)
    goto insane_timebase;

  GstClockTime min_pts = GST_SECOND * 60 * 60 * 1000;
  gst_video_encoder_set_min_pts (encoder, min_pts);
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
  if (!gst_niquadraenc_open_xcoder (encoder, state)) {
    gst_caps_unref (allowed_caps);
    goto OPEN_ERR;
  }

  other_caps = gst_niquadraenc_get_caps (encoder);

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

  GST_OBJECT_UNLOCK (encoder);

  return TRUE;

insane_timebase:
  {
    GST_ERROR_OBJECT (encoder, "Rejecting time base %d/%d",
        nienc->time_base_den, nienc->time_base_num);
    GST_OBJECT_UNLOCK (encoder);
    return FALSE;
  }
unsupported_codec:
  {
    GST_DEBUG_OBJECT (nienc, "Unsupported codec - no caps found");
    GST_OBJECT_UNLOCK (encoder);
    return FALSE;
  };
OPEN_ERR:
  {
    GST_ERROR_OBJECT (nienc, "Open codec error");
    GST_OBJECT_UNLOCK (encoder);
    return FALSE;
  };
}

static gboolean
gst_niquadraenc_start (GstVideoEncoder * encoder)
{
  GstNiquadraEnc *nienc = (GstNiquadraEnc *) encoder;

  nienc->pending_frames = g_queue_new ();
  nienc->context = gst_niquadra_context_new ();
  gst_video_encoder_set_min_pts (encoder, GST_SECOND * 60 * 60 * 1000);

  return TRUE;
}

static gboolean
gst_niquadraenc_flush (GstVideoEncoder * encoder)
{
  return TRUE;
}

static gboolean
gst_niquadraenc_propose_allocation (GstVideoEncoder * encoder, GstQuery * query)
{
  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);

  return GST_VIDEO_ENCODER_CLASS (parent_class)->propose_allocation (encoder,
      query);
}

static inline void
gst_xcoder_strncpy (char *dst, const char *src, int max)
{
  if (dst && src && max) {
    snprintf (dst, max, "%s", src);
  }
}

static NiPluginError
xcoder_encode_sequence_change (GstNiquadraEnc * enc, int width, int height,
    int bit_depth_factor)
{
  ni_session_context_t *p_ctx =
      gst_niquadra_context_get_session_context (enc->context);
  ni_session_data_io_t *p_pkt =
      gst_niquadra_context_get_data_pkt (enc->context);
  ni_retcode_t ret = NI_RETCODE_SUCCESS;
  ni_xcoder_params_t *pparams = (ni_xcoder_params_t *) p_ctx->p_session_config;

  GST_DEBUG_OBJECT (enc,
      "XCoder encode sequence change: session_run_state %d",
      p_ctx->session_run_state);

  ret = ni_device_session_sequence_change (p_ctx, width, height,
      bit_depth_factor, NI_DEVICE_TYPE_ENCODER);
  if (NI_RETCODE_SUCCESS != ret) {
    GST_ERROR_OBJECT (enc,
        "Failed to send Sequence Change to Encoder Session (status = %d)", ret);
    return NI_PLUGIN_FAILURE;
  }

  enc->pix_fmt = enc->input_state->info.finfo->format;

  // update session context
  p_ctx->bit_depth_factor = bit_depth_factor;
  p_ctx->src_bit_depth = (bit_depth_factor == 1) ? 8 : 10;
  p_ctx->src_endian = NI_FRAME_LITTLE_ENDIAN;
  p_ctx->ready_to_close = 0;
  p_ctx->frame_num = 0;
  p_ctx->pkt_num = 0;
  p_pkt->data.packet.end_of_stream = 0;

  switch (enc->pix_fmt) {
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_P010_10LE:
      pparams->cfg_enc_params.planar = NI_PIXEL_PLANAR_FORMAT_SEMIPLANAR;
      break;
    default:
      pparams->cfg_enc_params.planar = NI_PIXEL_PLANAR_FORMAT_PLANAR;
      break;
  }

  return NI_PLUGIN_OK;
}

static gboolean
xcoder_encode_reset (GstNiquadraEnc * enc)
{
  GST_DEBUG_OBJECT (enc, "XCoder encode reset");
  GstVideoEncoder *encoder = (GstVideoEncoder *) enc;
  gst_niquadraenc_close (encoder);
  return gst_niquadraenc_open_xcoder (encoder, enc->input_state);
}

static void
get_luma_size (int width, int height, int bit_depth,
    int *luma_size, int *luma_size_4n)
{
  int width_4n, height_4n;

  width_4n = ((width + 15) / 16) * 4;
  width = NI_ALIGN (width, 64);
  height = NI_ALIGN (height, 64);
  height_4n = height / 4;

  *luma_size = NI_ALIGN (width * 4 * bit_depth / 8, 64) * height / 4;
  *luma_size_4n = NI_ALIGN (width_4n * 4 * bit_depth / 8, 64) * height_4n / 4;
}

static NiPluginError
xcoder_encode_reinit (GstNiquadraEnc * enc)
{
  NiPluginError ret = NI_PLUGIN_OK;
  ni_session_context_t *p_ctx =
      gst_niquadra_context_get_session_context (enc->context);
  ni_xcoder_params_t *p_enc_params =
      gst_niquadra_context_get_xcoder_param (enc->context);
  bool ishwframe;
  ni_device_handle_t device_handle = p_ctx->device_handle;
  ni_device_handle_t blk_io_handle = p_ctx->blk_io_handle;
  int hw_id = p_ctx->hw_id;
  char tmp_blk_dev_name[NI_MAX_DEVICE_NAME_LEN];
  int bit_depth = 1;
  int pix_fmt = NI_PIX_FMT_YUV420P;
  int stride, luma_size, luma_size_4n;
  int ori_stride, ori_luma_size, ori_luma_size_4n;
  bool bIsSmallPicture = false;

  gst_xcoder_strncpy (tmp_blk_dev_name, p_ctx->blk_dev_name,
      NI_MAX_DEVICE_NAME_LEN);

  GstVideoCodecFrame *frame = g_queue_peek_head (enc->pending_frames);

  GST_DEBUG_OBJECT (enc,
      "xcoder_receive_packet resolution changing %dx%d -> %dx%d pix fmt=%d",
      enc->width, enc->height,
      enc->input_state->info.width, enc->input_state->info.height,
      enc->input_state->info.finfo->format);

  enc->width = enc->input_state->info.width;
  enc->height = enc->input_state->info.height;
  enc->pix_fmt = enc->input_state->info.finfo->format;

  ishwframe = enc->hardware_mode;
  if (ishwframe) {
    GstMemory *nimem = gst_buffer_peek_memory (frame->input_buffer, 0);
    niFrameSurface1_t *surface = gst_surface_from_ni_hw_memory (nimem);
    bit_depth = (uint8_t) surface->bit_depth;
    switch (enc->pix_fmt) {
      case GST_VIDEO_FORMAT_NV12:
        pix_fmt = NI_PIX_FMT_NV12;
        break;
      case GST_VIDEO_FORMAT_I420:
        pix_fmt = NI_PIX_FMT_YUV420P;
        break;
      case GST_VIDEO_FORMAT_P010_10LE:
      case GST_VIDEO_FORMAT_P010_10BE:
        pix_fmt = NI_PIX_FMT_P010LE;
        break;
      case GST_VIDEO_FORMAT_I420_10LE:
        pix_fmt = NI_PIX_FMT_YUV420P10LE;
        break;
#if ENABLE_NI_GST_FRAMEWORK_PATCH
      case GST_VIDEO_FORMAT_NI_QUAD_8_4L4:
        if (bit_depth == 2) {
          enc->pix_fmt = GST_VIDEO_FORMAT_NI_QUAD_10_4L4;
          p_ctx->pixel_format = NI_PIX_FMT_10_TILED4X4;
        } else
          p_ctx->pixel_format = NI_PIX_FMT_8_TILED4X4;
        break;
      case GST_VIDEO_FORMAT_NI_QUAD_10_4L4:
        if (bit_depth == 1) {
          enc->pix_fmt = GST_VIDEO_FORMAT_NI_QUAD_8_4L4;
          p_ctx->pixel_format = NI_PIX_FMT_8_TILED4X4;
        } else
          p_ctx->pixel_format = NI_PIX_FMT_10_TILED4X4;
        break;
#endif
      default:
        break;
    }
  } else {
    switch (enc->pix_fmt) {
      case GST_VIDEO_FORMAT_NV12:
      case GST_VIDEO_FORMAT_I420:
        bit_depth = 1;
        break;
      case GST_VIDEO_FORMAT_P010_10LE:
      case GST_VIDEO_FORMAT_P010_10BE:
      case GST_VIDEO_FORMAT_I420_10LE:
        bit_depth = 2;
        break;
      default:
        break;
    }
  }

  enc->eos_fme_received = 0;
  enc->encoder_eof = 0;
  enc->encoder_flushing = 0;
  enc->firstPktArrived = 0;
  enc->spsPpsArrived = 0;
  enc->spsPpsHdrLen = 0;
  if (enc->p_spsPpsHdr) {
    free (enc->p_spsPpsHdr);
    enc->p_spsPpsHdr = NULL;
  }
  // check if resolution is zero copy compatible and set linesize according to new resolution
  if (ni_encoder_frame_zerocopy_check (p_ctx,
          p_enc_params, enc->input_state->info.width,
          enc->input_state->info.height,
          (const int *) enc->input_state->info.stride,
          true) == NI_RETCODE_SUCCESS) {
    stride = p_enc_params->luma_linesize;       // new sequence is zero copy compatible
  } else {
    stride = NI_ALIGN (enc->input_state->info.width * bit_depth, 128);
  }

  if (p_ctx->ori_luma_linesize && p_ctx->ori_chroma_linesize) {
    ori_stride = p_ctx->ori_luma_linesize;      // previous sequence was zero copy compatible
  } else {
    ori_stride = NI_ALIGN (p_ctx->ori_width * bit_depth, 128);
  }

  get_luma_size (enc->input_state->info.width, enc->input_state->info.height,
      bit_depth == 2 ? 10 : 8, &luma_size, &luma_size_4n);
  get_luma_size (p_ctx->ori_width, p_ctx->ori_height,
      p_ctx->ori_bit_depth_factor == 2 ? 10 : 8, &ori_luma_size,
      &ori_luma_size_4n);

  if (pix_fmt == NI_PIX_FMT_ARGB || pix_fmt == NI_PIX_FMT_ABGR ||
      pix_fmt == NI_PIX_FMT_RGBA || pix_fmt == NI_PIX_FMT_BGRA) {
    stride = enc->input_state->info.width;
    ori_stride = p_ctx->ori_width;
    luma_size = luma_size_4n =
        enc->input_state->info.width * enc->input_state->info.height;
    ori_luma_size = ori_luma_size_4n = p_ctx->ori_width * p_ctx->ori_height;
  }

  if (p_enc_params->cfg_enc_params.lookAheadDepth ||
      p_enc_params->cfg_enc_params.crf >= 0 ||
      p_enc_params->cfg_enc_params.crfFloat >= 0) {
    GST_DEBUG_OBJECT (enc,
        "xcoder_encode_reinit 2-pass lookaheadDepth %d and/or CRF %d and/or CRFFloat %f",
        p_enc_params->cfg_enc_params.lookAheadDepth,
        p_enc_params->cfg_enc_params.crf,
        p_enc_params->cfg_enc_params.crfFloat);
    if ((enc->input_state->info.width < NI_2PASS_ENCODE_MIN_WIDTH) ||
        (enc->input_state->info.height < NI_2PASS_ENCODE_MIN_HEIGHT)) {
      bIsSmallPicture = true;
    }
  } else {
    if ((enc->input_state->info.width < NI_MIN_WIDTH) ||
        (enc->input_state->info.height < NI_MIN_HEIGHT)) {
      bIsSmallPicture = true;
    }
  }

  if (p_enc_params->cfg_enc_params.multicoreJointMode) {
    GST_DEBUG_OBJECT (enc, "xcoder_encode_reinit multicore joint mode");
    if ((enc->input_state->info.width < 256) ||
        (enc->input_state->info.height < 256)) {
      bIsSmallPicture = true;
    }
  }

  if (p_enc_params->cfg_enc_params.crop_width
      || p_enc_params->cfg_enc_params.crop_height) {
    GST_DEBUG_OBJECT (enc,
        "xcoder_encode_reinit needs to close and re-open due to crop width x height");
    bIsSmallPicture = true;
  }
  // fast sequence change without close / open only if new resolution < original resolution
  if ((ori_stride * p_ctx->ori_height <= stride * enc->input_state->info.height)
      || (ori_luma_size < luma_size)
      || (ori_luma_size_4n < luma_size_4n)
      || (p_ctx->ori_pix_fmt != pix_fmt)
      || (bIsSmallPicture || enc->codec_format == NI_CODEC_FORMAT_JPEG)
      || (p_enc_params->cfg_enc_params.disable_adaptive_buffers)) {
    gst_niquadraenc_close (&enc->element);
    gst_niquadraenc_open_xcoder (&enc->element, enc->input_state);
  } else {
    if (enc->codec_format == NI_CODEC_FORMAT_AV1) {
      if (enc->input_state->info.width % NI_PARAM_AV1_ALIGN_WIDTH_HEIGHT)
        GST_DEBUG_OBJECT (enc,
            "resolution change: AV1 Picture Width not aligned to %d - picture will be cropped",
            NI_PARAM_AV1_ALIGN_WIDTH_HEIGHT);

      if (enc->input_state->info.height % NI_PARAM_AV1_ALIGN_WIDTH_HEIGHT)
        GST_DEBUG_OBJECT (enc,
            "resolution change: AV1 Picture Height not aligned to %d - picture will be cropped",
            NI_PARAM_AV1_ALIGN_WIDTH_HEIGHT);
    }
    ret = xcoder_encode_sequence_change (enc, enc->input_state->info.width,
        enc->input_state->info.height, bit_depth);
  }

  // keep device handle(s) open during sequence change to fix mem bin buffer not recycled
  p_ctx->device_handle = device_handle;
  p_ctx->blk_io_handle = blk_io_handle;
  p_ctx->hw_id = hw_id;
  gst_xcoder_strncpy (p_ctx->blk_dev_name, tmp_blk_dev_name,
      NI_MAX_DEVICE_NAME_LEN);
  p_ctx->session_run_state = SESSION_RUN_STATE_SEQ_CHANGE_OPENING;      // this state is referenced when sending first frame after sequence change

  return ret;
}

/* set meta data such as HDR closed caption and roi */
static void
gst_ni_add_metadata (GstNiquadraEnc * enc, GstVideoCodecFrame * src_frame,
    ni_frame_t * dec_frame, ni_aux_data_t * aux_data)
{
  ni_session_context_t *p_ctx =
      gst_niquadra_context_get_session_context (enc->context);
  ni_xcoder_params_t *p_enc_params =
      gst_niquadra_context_get_xcoder_param (enc->context);
  ni_session_data_io_t *p_frame =
      gst_niquadra_context_get_data_frame (enc->context);
  ni_xcoder_params_t *p_param;
  p_param = (ni_xcoder_params_t *) p_ctx->p_session_config;

  if (p_enc_params->enable_vfr) {
    int cur_fps = 0, pre_fps = 0;

    pre_fps = p_ctx->prev_fps;

    if (dec_frame->pts > p_ctx->prev_pts) {
      p_ctx->passed_time_in_timebase_unit += dec_frame->pts - p_ctx->prev_pts;
      p_ctx->count_frame_num_in_sec++;
      if (p_ctx->passed_time_in_timebase_unit >= (enc->fps_num / enc->fps_den)) {
        cur_fps = p_ctx->count_frame_num_in_sec;
        if ((p_ctx->frame_num != 0) && (pre_fps != cur_fps) &&
            ((p_ctx->frame_num < p_param->cfg_enc_params.frame_rate) ||
                (p_ctx->frame_num - p_ctx->last_change_framenum >=
                    p_param->cfg_enc_params.frame_rate))) {
          aux_data =
              ni_frame_new_aux_data (dec_frame, NI_FRAME_AUX_DATA_FRAMERATE,
              sizeof (ni_framerate_t));
          if (aux_data) {
            ni_framerate_t *framerate = (ni_framerate_t *) aux_data->data;
            framerate->framerate_num = cur_fps;
            framerate->framerate_denom = 1;
          }

          p_ctx->last_change_framenum = p_ctx->frame_num;
          p_ctx->prev_fps = cur_fps;
        }
        p_ctx->count_frame_num_in_sec = 0;
        p_ctx->passed_time_in_timebase_unit = 0;
      }
      p_ctx->prev_pts = dec_frame->pts;
    } else if (dec_frame->pts < p_ctx->prev_pts) {
      p_ctx->prev_pts = dec_frame->pts;
    }
  }
  if (p_param->force_pic_qp_demo_mode) {
    if (p_ctx->frame_num >= 300) {
      p_frame->data.frame.force_pic_qp = p_param->cfg_enc_params.rc.intra_qp;
    } else if (p_ctx->frame_num >= 200) {
      p_frame->data.frame.force_pic_qp = p_param->force_pic_qp_demo_mode;
    }
  }
  uint8_t num_roi = gst_buffer_get_n_meta (src_frame->input_buffer,
      GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE);
  if (!p_param->roi_demo_mode && p_param->cfg_enc_params.roi_enable
      && num_roi > 0) {
    int roi_data_size = num_roi * sizeof (ni_region_of_interest_t);
    GstStructure *s;
    aux_data =
        ni_frame_new_aux_data (dec_frame, NI_FRAME_AUX_DATA_REGIONS_OF_INTEREST,
        roi_data_size);
    uint8_t *roi_data = g_malloc0 (roi_data_size + 1);
    int valid_roi = 0;
    for (int i = 0; i < num_roi; i++) {
      GstVideoRegionOfInterestMeta *roi;
      gpointer state = NULL;
      roi = (GstVideoRegionOfInterestMeta *)
          gst_buffer_iterate_meta_filtered (src_frame->input_buffer, &state,
          GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE);
      if (!roi)
        continue;
      ni_region_of_interest_t ni_roi_t;
      ni_roi_t.self_size = sizeof (ni_region_of_interest_t);
      ni_roi_t.top = roi->y;
      ni_roi_t.bottom = roi->y + roi->h;
      ni_roi_t.left = roi->x;
      ni_roi_t.right = roi->x + roi->w;

      s = gst_video_region_of_interest_meta_get_param (roi, "roi/niquadra");

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
  if (p_param->roi_demo_mode && p_param->cfg_enc_params.roi_enable) {
    if (p_ctx->frame_num > 90 && p_ctx->frame_num < 300) {
      p_frame->data.frame.roi_len = p_ctx->roi_len;
    } else {
      p_frame->data.frame.roi_len = 0;
    }
    // when ROI enabled, always have a data buffer for ROI
    // Note: this is handled separately from ROI through side/aux data
    p_frame->data.frame.extra_data_len += p_ctx->roi_len;
  }

  if (!p_param->cfg_enc_params.enable_all_sei_passthru) {
    if (!(p_param->cfg_enc_params.HDR10CLLEnable)) {
      GstVideoContentLightLevel cll;
      if (gst_video_content_light_level_from_caps (&cll,
              enc->input_state->caps)) {
        ni_content_light_level_t metadata;

        metadata.max_cll = cll.max_content_light_level;
        metadata.max_fall = cll.max_frame_average_light_level;
        aux_data =
            ni_frame_new_aux_data (dec_frame,
            NI_FRAME_AUX_DATA_CONTENT_LIGHT_LEVEL,
            sizeof (ni_content_light_level_t));
        if (aux_data) {
          memcpy (aux_data->data, &metadata, sizeof (ni_content_light_level_t));
        }
      }
    } else if ((NI_CODEC_FORMAT_H264 == enc->codec_format ||
            p_ctx->bit_depth_factor == 1) && p_ctx->light_level_data_len == 0) {
      aux_data = ni_frame_new_aux_data (dec_frame,
          NI_FRAME_AUX_DATA_CONTENT_LIGHT_LEVEL,
          sizeof (ni_content_light_level_t));
      if (aux_data) {
        ni_content_light_level_t *cll =
            (ni_content_light_level_t *) (aux_data->data);
        cll->max_cll = p_param->cfg_enc_params.HDR10MaxLight;
        cll->max_fall = p_param->cfg_enc_params.HDR10AveLight;
      }
    }

    if (!(p_param->cfg_enc_params.HDR10Enable)) {
      GstVideoMasteringDisplayInfo minfo;
      if (gst_video_mastering_display_info_from_caps (&minfo,
              enc->input_state->caps)) {
        const int chroma_den = 50000;
        const int luma_den = 10000;
        int i;
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
            ni_frame_new_aux_data (dec_frame,
            NI_FRAME_AUX_DATA_MASTERING_DISPLAY_METADATA,
            sizeof (ni_mastering_display_metadata_t));
        if (aux_data) {
          memcpy (aux_data->data, &metadata,
              sizeof (ni_mastering_display_metadata_t));
        }
      }
    } else if ((NI_CODEC_FORMAT_H264 == enc->codec_format ||
            p_ctx->bit_depth_factor == 1) &&
        p_ctx->sei_hdr_mastering_display_color_vol_len == 0) {
      aux_data = ni_frame_new_aux_data (dec_frame,
          NI_FRAME_AUX_DATA_MASTERING_DISPLAY_METADATA,
          sizeof (ni_mastering_display_metadata_t));
      if (aux_data) {
        ni_mastering_display_metadata_t *mst_dsp =
            (ni_mastering_display_metadata_t *) (aux_data->data);

        mst_dsp->display_primaries[0][0].den = MASTERING_DISP_CHROMA_DEN;
        mst_dsp->display_primaries[0][1].den = MASTERING_DISP_CHROMA_DEN;
        mst_dsp->display_primaries[1][0].den = MASTERING_DISP_CHROMA_DEN;
        mst_dsp->display_primaries[1][1].den = MASTERING_DISP_CHROMA_DEN;
        mst_dsp->display_primaries[2][0].den = MASTERING_DISP_CHROMA_DEN;
        mst_dsp->display_primaries[2][1].den = MASTERING_DISP_CHROMA_DEN;
        mst_dsp->white_point[0].den = MASTERING_DISP_CHROMA_DEN;
        mst_dsp->white_point[1].den = MASTERING_DISP_CHROMA_DEN;
        mst_dsp->min_luminance.den = MASTERING_DISP_LUMA_DEN;
        mst_dsp->max_luminance.den = MASTERING_DISP_LUMA_DEN;
        mst_dsp->display_primaries[0][0].num = p_param->cfg_enc_params.HDR10dx2;
        mst_dsp->display_primaries[0][1].num = p_param->cfg_enc_params.HDR10dy2;
        mst_dsp->display_primaries[1][0].num = p_param->cfg_enc_params.HDR10dx0;
        mst_dsp->display_primaries[1][1].num = p_param->cfg_enc_params.HDR10dy0;
        mst_dsp->display_primaries[2][0].num = p_param->cfg_enc_params.HDR10dx1;
        mst_dsp->display_primaries[2][1].num = p_param->cfg_enc_params.HDR10dy1;
        mst_dsp->white_point[0].num = p_param->cfg_enc_params.HDR10wx;
        mst_dsp->white_point[1].num = p_param->cfg_enc_params.HDR10wy;
        mst_dsp->min_luminance.num = p_param->cfg_enc_params.HDR10minluma;
        mst_dsp->max_luminance.num = p_param->cfg_enc_params.HDR10maxluma;
        mst_dsp->has_primaries = 1;
        mst_dsp->has_luminance = 1;
      }
    }
    //SEI (HDR10+)
    GstVideoHdrPlusMeta *hdr10_meta;
    gpointer iter_hdr10 = NULL;

    while ((hdr10_meta = (GstVideoHdrPlusMeta *)
            gst_buffer_iterate_meta_filtered (src_frame->input_buffer,
                &iter_hdr10, GST_VIDEO_HDR_PLUS_META_API_TYPE))) {
      aux_data =
          ni_frame_new_aux_data (dec_frame, NI_FRAME_AUX_DATA_HDR_PLUS,
          sizeof (ni_dynamic_hdr_plus_t));

      if (aux_data) {
        memcpy (aux_data->data, hdr10_meta->data, hdr10_meta->size);
      }
    }

    //SEI (UDU)
    GstVideoSEIUserDataUnregisteredMeta *udu_meta;
    gpointer iter_udu = NULL;

    while ((udu_meta = (GstVideoSEIUserDataUnregisteredMeta *)
            gst_buffer_iterate_meta_filtered (src_frame->input_buffer,
                &iter_udu,
                GST_VIDEO_SEI_USER_DATA_UNREGISTERED_META_API_TYPE))) {
      aux_data =
          ni_frame_new_aux_data (dec_frame, NI_FRAME_AUX_DATA_UDU_SEI,
          udu_meta->size);

      if (aux_data) {
        memcpy (aux_data->data, udu_meta->data, udu_meta->size);
      }
    }

    // SEI (close caption)
    GstVideoCaptionMeta *cc_meta =
        gst_buffer_get_video_caption_meta (src_frame->input_buffer);
    if (cc_meta) {
      if (cc_meta->caption_type != GST_VIDEO_CAPTION_TYPE_CEA708_RAW)
        GST_ERROR_OBJECT (enc, "close caption type mismatch");

      aux_data = ni_frame_new_aux_data (dec_frame, NI_FRAME_AUX_DATA_A53_CC,
          cc_meta->size);
      if (aux_data) {
        memcpy (aux_data->data, cc_meta->data, cc_meta->size);
      }
    }
  }
}

static NiPluginError
expand_ni_frame (ni_frame_t * dst,
    const ni_frame_t * src, const int dst_stride[],
    int raw_width, int raw_height, GstVideoFormat format)
{
  int i, j, h, nb_planes, tenBit;
  int vpad[3], hpad[3], src_height[3], src_width[3], src_stride[3];
  uint8_t *src_line, *dst_line, *sample, *dest, YUVsample;
  uint16_t lastidx;
  int bytes_per_pixel = 1;

  nb_planes = 0;

  switch (format) {
    case GST_VIDEO_FORMAT_I420:
      /* width of source frame for each plane in pixels */
      src_width[0] = NI_ALIGN (raw_width, 2);
      src_width[1] = NI_ALIGN (raw_width, 2) / 2;
      src_width[2] = NI_ALIGN (raw_width, 2) / 2;

      /* height of source frame for each plane in pixels */
      src_height[0] = NI_ALIGN (raw_height, 2);
      src_height[1] = NI_ALIGN (raw_height, 2) / 2;
      src_height[2] = NI_ALIGN (raw_height, 2) / 2;

      /* stride of source frame for each plane in bytes */
      src_stride[0] = NI_ALIGN (src_width[0], 128);
      src_stride[1] = NI_ALIGN (src_width[1], 128);
      src_stride[2] = NI_ALIGN (src_width[2], 128);

      tenBit = 0;
      nb_planes = 3;

      /* horizontal padding needed for each plane in bytes */
      hpad[0] = dst_stride[0] - src_width[0];
      hpad[1] = dst_stride[1] - src_width[1];
      hpad[2] = dst_stride[2] - src_width[2];

      /* vertical padding needed for each plane in pixels */
      vpad[0] = NI_MIN_HEIGHT - src_height[0];
      vpad[1] = NI_MIN_HEIGHT / 2 - src_height[1];
      vpad[2] = NI_MIN_HEIGHT / 2 - src_height[2];

      break;

    case GST_VIDEO_FORMAT_I420_10LE:
      /* width of source frame for each plane in pixels */
      src_width[0] = NI_ALIGN (raw_width, 2);
      src_width[1] = NI_ALIGN (raw_width, 2) / 2;
      src_width[2] = NI_ALIGN (raw_width, 2) / 2;

      /* height of source frame for each plane in pixels */
      src_height[0] = NI_ALIGN (raw_height, 2);
      src_height[1] = NI_ALIGN (raw_height, 2) / 2;
      src_height[2] = NI_ALIGN (raw_height, 2) / 2;

      /* stride of source frame for each plane in bytes */
      src_stride[0] = NI_ALIGN (src_width[0] * 2, 128);
      src_stride[1] = NI_ALIGN (src_width[1] * 2, 128);
      src_stride[2] = NI_ALIGN (src_width[2] * 2, 128);

      tenBit = 1;
      nb_planes = 3;

      /* horizontal padding needed for each plane in bytes */
      hpad[0] = dst_stride[0] - src_width[0] * 2;
      hpad[1] = dst_stride[1] - src_width[1] * 2;
      hpad[2] = dst_stride[2] - src_width[2] * 2;

      /* vertical padding needed for each plane in pixels */
      vpad[0] = NI_MIN_HEIGHT - src_height[0];
      vpad[1] = NI_MIN_HEIGHT / 2 - src_height[1];
      vpad[2] = NI_MIN_HEIGHT / 2 - src_height[2];

      break;

    case GST_VIDEO_FORMAT_NV12:
      /* width of source frame for each plane in pixels */
      src_width[0] = NI_ALIGN (raw_width, 2);
      src_width[1] = NI_ALIGN (raw_width, 2);
      src_width[2] = 0;

      /* height of source frame for each plane in pixels */
      src_height[0] = NI_ALIGN (raw_height, 2);
      src_height[1] = NI_ALIGN (raw_height, 2) / 2;
      src_height[2] = 0;

      /* stride of source frame for each plane in bytes */
      src_stride[0] = NI_ALIGN (src_width[0], 128);
      src_stride[1] = NI_ALIGN (src_width[1], 128);
      src_stride[2] = 0;

      tenBit = 0;
      nb_planes = 2;

      /* horizontal padding needed for each plane in bytes */
      hpad[0] = dst_stride[0] - src_width[0];
      hpad[1] = dst_stride[1] - src_width[1];
      hpad[2] = 0;

      /* vertical padding for each plane in pixels */
      vpad[0] = NI_MIN_HEIGHT - src_height[0];
      vpad[1] = NI_MIN_HEIGHT / 2 - src_height[1];
      vpad[2] = 0;

      break;

    case GST_VIDEO_FORMAT_P010_10LE:
      /* width of source frame for each plane in pixels */
      src_width[0] = NI_ALIGN (raw_width, 2);
      src_width[1] = NI_ALIGN (raw_width, 2);
      src_width[2] = 0;

      /* height of source frame for each plane in pixels */
      src_height[0] = NI_ALIGN (raw_height, 2);
      src_height[1] = NI_ALIGN (raw_height, 2) / 2;
      src_height[2] = 0;

      /* stride of source frame for each plane in bytes */
      src_stride[0] = NI_ALIGN (src_width[0] * 2, 128);
      src_stride[1] = NI_ALIGN (src_width[1] * 2, 128);
      src_stride[2] = 0;

      tenBit = 1;

      /* horizontal padding needed for each plane in bytes */
      hpad[0] = dst_stride[0] - src_width[0] * 2;
      hpad[1] = dst_stride[1] - src_width[1] * 2;
      hpad[2] = 0;

      /* vertical padding for each plane in pixels */
      vpad[0] = NI_MIN_HEIGHT - src_height[0];
      vpad[1] = NI_MIN_HEIGHT / 2 - src_height[1];
      vpad[2] = 0;

      break;

    case GST_VIDEO_FORMAT_ARGB:
    case GST_VIDEO_FORMAT_ABGR:
    case GST_VIDEO_FORMAT_RGBA:
    case GST_VIDEO_FORMAT_BGRA:
      /* width of source frame for each plane in pixels */
      src_width[0] = NI_ALIGN (raw_width, 2);
      src_width[1] = 0;
      src_width[2] = 0;

      /* height of source frame for each plane in pixels */
      src_height[0] = NI_ALIGN (raw_height, 2);
      src_height[1] = 0;
      src_height[2] = 0;

      /* stride of source frame for each plane in bytes */
      src_stride[0] = NI_ALIGN (src_width[0] * 4, 64);
      src_stride[1] = 0;
      src_stride[2] = 0;

      tenBit = 0;
      bytes_per_pixel = 4;

      /* horizontal padding needed for each plane in bytes */
      hpad[0] = dst_stride[0] - src_width[0] * 4;
      hpad[1] = 0;
      hpad[2] = 0;

      /* vertical padding for each plane in pixels */
      vpad[0] = NI_MIN_HEIGHT - src_height[0];
      vpad[1] = 0;
      vpad[2] = 0;
      break;

    default:
      GST_ERROR ("Invalid pixel format %d\n", format);
      return NI_PLUGIN_FAILURE;
  }
  if (tenBit) {
    bytes_per_pixel = 2;
  }

  for (i = 0; i < nb_planes; i++) {
    dst_line = dst->p_data[i];
    src_line = src->p_data[i];

    for (h = 0; h < src_height[i]; h++) {
      memcpy (dst_line, src_line, src_width[i] * (bytes_per_pixel));

      /* Add horizontal padding */
      if (hpad[i]) {
        lastidx = src_width[i];

        if (bytes_per_pixel > 1) {
          sample = &src_line[(lastidx - 1) * bytes_per_pixel];
          dest = &dst_line[lastidx * bytes_per_pixel];

          /* bytes_per_pixel per sample */
          for (j = 0; j < hpad[i] / bytes_per_pixel; j++) {
            memcpy (dest, sample, bytes_per_pixel);
            dest += bytes_per_pixel;
          }
        } else {
          YUVsample = dst_line[lastidx - 1];
          memset (&dst_line[lastidx], YUVsample, hpad[i]);
        }
      }

      src_line += src_stride[i];
      dst_line += dst_stride[i];
    }

    /* Pad the height by duplicating the last line */
    src_line = dst_line - dst_stride[i];

    for (h = 0; h < vpad[i]; h++) {
      memcpy (dst_line, src_line, dst_stride[i]);
      dst_line += dst_stride[i];
    }
  }

  return NI_PLUGIN_OK;
}

static GstFlowReturn
xcoder_send_frame (GstNiquadraEnc * enc, GstVideoCodecFrame * frame,
    gboolean * send_frame)
{
  GstVideoInfo *info = &enc->input_state->info;
  ni_session_context_t *p_ctx =
      gst_niquadra_context_get_session_context (enc->context);
  ni_xcoder_params_t *p_enc_params =
      gst_niquadra_context_get_xcoder_param (enc->context);
  ni_session_data_io_t *p_frame =
      gst_niquadra_context_get_data_frame (enc->context);
  bool ishwframe = enc->hardware_mode;
  bool isnv12frame;
  bool alignment_2pass_wa;
  int format_in_use;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  int ret = 0;
  int sent;
  int i, j;
  int orig_avctx_width = enc->width;
  int orig_avctx_height = enc->height;
  ni_xcoder_params_t *p_param;
  int need_to_copy = 1;
  const GstVideoCodecFrame *first_frame = NULL;
  GstVideoCodecFrame *cur_frame = NULL;
  // employ a ni_frame_t as a data holder to convert/prepare for side data
  // of the passed in frame
  ni_frame_t dec_frame = { 0 };
  ni_session_context_t *nisession = NULL;
  niFrameSurface1_t *nisurface = NULL;
  gint deviceid = -1;
  ni_aux_data_t *aux_data = NULL;
  // data buffer for various SEI: HDR mastering display color volume, HDR
  // content light level, close caption, User data unregistered, HDR10+ etc.
  int send_sei_with_idr;
  uint8_t mdcv_data[NI_MAX_SEI_DATA];
  uint8_t cll_data[NI_MAX_SEI_DATA];
  uint8_t cc_data[NI_MAX_SEI_DATA];
  uint8_t udu_data[NI_MAX_SEI_DATA];
  uint8_t hdrp_data[NI_MAX_SEI_DATA];

  GST_DEBUG_OBJECT (enc, "XCoder send frame");

  *send_frame = FALSE;

  p_param = (ni_xcoder_params_t *) p_ctx->p_session_config;
  alignment_2pass_wa = ((p_param->cfg_enc_params.lookAheadDepth ||
          p_param->cfg_enc_params.crf >= 0 ||
          p_param->cfg_enc_params.crfFloat >= 0) &&
      ((enc->codec_format == NI_CODEC_FORMAT_H265) ||
          (enc->codec_format == NI_CODEC_FORMAT_AV1)));

  // leave encoder instance open to when the first frame buffer arrives so that
  // its stride size is known and handled accordingly.
  if (enc->started == 0) {
    if (g_queue_get_length (enc->pending_frames) != 0) {
      first_frame = g_queue_peek_head (enc->pending_frames);
    } else if (frame) {
      first_frame = frame;
    } else {
      GST_ERROR_OBJECT (enc, "first frame: NULL is unexpected!");
    }
  } else if (p_ctx->session_run_state == SESSION_RUN_STATE_SEQ_CHANGE_OPENING) {
    if (g_queue_get_length (enc->pending_frames) > 0) {
      first_frame = g_queue_peek_head (enc->pending_frames);
    } else {
      GST_ERROR_OBJECT (enc, "No buffered frame - Sequence Change Fail");
      return GST_FLOW_ERROR;
    }
  }

  /*1. open encoder session */
  if (first_frame && enc->started == 0) {
    // if frame stride size is not as we expect it,
    // adjust using xcoder-params conf_win_right
    int linesize_aligned = info->width;
    int height_aligned = info->height;
    if (enc->hardware_mode) {
      linesize_aligned = enc->input_state->info.width;
      height_aligned = enc->input_state->info.height;
    }
    if (linesize_aligned < NI_MIN_WIDTH) {
      p_param->cfg_enc_params.conf_win_right +=
          (NI_MIN_WIDTH - info->width) / 2 * 2;
      linesize_aligned = NI_MIN_WIDTH;
    } else {
      if (GST_NI_QUADRA_FORMAT_IS_TILED (enc->pix_fmt)) {
        linesize_aligned = NI_ALIGN (info->width, 4);
        p_param->cfg_enc_params.conf_win_right +=
            (linesize_aligned - info->width) / 2 * 2;
      } else {
        linesize_aligned = NI_ALIGN (info->width, 2);
        p_param->cfg_enc_params.conf_win_right +=
            (linesize_aligned - info->width) / 2 * 2;
      }
    }
    p_param->source_width = linesize_aligned;

    if (height_aligned < NI_MIN_HEIGHT) {
      p_param->cfg_enc_params.conf_win_bottom +=
          (NI_MIN_HEIGHT - info->height) / 2 * 2;
      height_aligned = NI_MIN_HEIGHT;
    } else {
      if (GST_NI_QUADRA_FORMAT_IS_TILED (enc->pix_fmt)) {
        height_aligned = NI_ALIGN (info->height, 4);
        p_param->cfg_enc_params.conf_win_bottom +=
            (height_aligned - info->height) / 2 * 2;
      } else {
        height_aligned = NI_ALIGN (info->height, 2);
        p_param->cfg_enc_params.conf_win_bottom +=
            (height_aligned - info->height) / 2 * 2;
      }
    }
    p_param->source_height = height_aligned;

    ni_color_primaries_t ni_color_primaries = 0;
    ni_color_transfer_characteristic_t ni_color_transfer = 0;
    ni_color_space_t ni_color_space = 0;

    if (enc->input_state->info.colorimetry.primaries !=
        GST_VIDEO_COLOR_PRIMARIES_UNKNOWN) {
      ni_color_primaries = gst_video_color_primaries_to_iso
          (enc->input_state->info.colorimetry.primaries);
      p_enc_params->color_primaries = ni_color_primaries;
    }

    if (enc->input_state->info.colorimetry.transfer !=
        GST_VIDEO_TRANSFER_UNKNOWN) {
      ni_color_transfer = gst_video_transfer_function_to_iso
          (enc->input_state->info.colorimetry.transfer);
    }

    if (enc->input_state->info.colorimetry.matrix !=
        GST_VIDEO_COLOR_MATRIX_UNKNOWN) {
      ni_color_space = gst_video_color_matrix_to_iso
          (enc->input_state->info.colorimetry.matrix);
    }

    if (enc->codec_format == NI_CODEC_FORMAT_H265 && (ni_color_transfer != 0
            || ni_color_primaries != 0 || ni_color_space != 0)) {
      p_enc_params->cfg_enc_params.colorDescPresent = 1;
      p_enc_params->cfg_enc_params.colorPrimaries = ni_color_primaries;
      p_enc_params->cfg_enc_params.colorSpace = ni_color_space;
      p_enc_params->cfg_enc_params.colorTrc = ni_color_transfer;

      GST_DEBUG_OBJECT (enc,
          "ni matrix=%d, transfer=%d, primaries=%d,profile=%d\n",
          p_enc_params->color_space,
          p_enc_params->color_transfer_characteristic,
          p_enc_params->color_primaries, p_enc_params->dolby_vision_profile);
    }

    if (xcoder_encoder_header_check_set (enc) != NI_PLUGIN_OK) {
      return GST_FLOW_ERROR;
    }

    p_ctx->ori_width = enc->width;
    p_ctx->ori_height = enc->height;

    GST_DEBUG_OBJECT (enc,
        "XCoder frame conf_win_right %d  conf_win_bottom %d , color primaries %u trc %u space %u",
        p_param->cfg_enc_params.conf_win_right,
        p_param->cfg_enc_params.conf_win_bottom,
        ni_color_primaries, ni_color_transfer, ni_color_space);

    if (SESSION_RUN_STATE_SEQ_CHANGE_OPENING != p_ctx->session_run_state) {
      // sequence change backup / restore encoder device handles, hw_id and
      // block device name, so no need to overwrite hw_id/blk_dev_name to user
      // set values
      p_ctx->hw_id = enc->dev_enc_idx;

      gst_xcoder_strncpy (p_ctx->dev_xcoder_name, enc->dev_xcoder_name,
          MAX_CHAR_IN_DEVICE_NAME);
      gst_xcoder_strncpy (p_ctx->blk_dev_name, enc->blk_xcoder_name,
          NI_MAX_DEVICE_NAME_LEN);
    }

    if (enc->hardware_mode) {
      GstMemory *nimem = gst_buffer_peek_memory (first_frame->input_buffer, 0);
      nisurface = gst_surface_from_ni_hw_memory (nimem);
      nisession = gst_session_from_ni_hw_memory (nimem);
      deviceid = gst_deviceid_from_ni_hw_memory (nimem);
      if (nisurface == NULL) {
        return GST_FLOW_ERROR;
      }
    }

    p_param->rootBufId = (ishwframe) ? (nisurface->ui16FrameIdx) : 0;
    if (ishwframe) {
      p_ctx->hw_action = NI_CODEC_HW_ENABLE;
      p_ctx->sender_handle = (ni_device_handle_t) (
          (int64_t) (nisurface->device_handle));
      ff_xcoder_strncpy (p_ctx->blk_dev_name, nisession->blk_dev_name,
          NI_MAX_DEVICE_NAME_LEN);
      GST_DEBUG_OBJECT (enc,
          "open encoder, rootBufId=%d, sender_handle=%d,%dx%d",
          p_param->rootBufId, p_ctx->sender_handle, nisurface->ui16width,
          nisurface->ui16height);
      p_ctx->ori_width = p_enc_params->source_width;
      p_ctx->ori_height = p_enc_params->source_height;
    }

    if (enc->hardware_mode && p_ctx->hw_id == -1
        && 0 == strcmp (p_ctx->blk_dev_name, "")) {
      p_ctx->hw_id = deviceid;
      GST_DEBUG_OBJECT (enc,
          "xcoder_send_frame: hw_id -1, empty blk_dev_name, collocated to %d",
          p_ctx->hw_id);
    }

    p_ctx->p_session_config = p_enc_params;

    // config linesize for zero copy (if input resolution is zero copy compatible)
    ni_encoder_frame_zerocopy_check (p_ctx, p_param, info->width, info->height,
        info->stride, true);

    ret = ni_device_session_open (p_ctx, NI_DEVICE_TYPE_ENCODER);

    // // As the file handle may change we need to assign back
    enc->dev_xcoder_name = p_ctx->dev_xcoder_name;
    enc->blk_xcoder_name = p_ctx->blk_xcoder_name;
    enc->dev_enc_idx = p_ctx->hw_id;

    switch (ret) {
      case NI_RETCODE_SUCCESS:
        GST_DEBUG_OBJECT (enc,
            "XCoder %s.%d (inst: %d) opened successfully\n",
            enc->dev_xcoder_name, enc->dev_enc_idx, p_ctx->session_id);
        enc->opened = TRUE;
        break;
      case NI_RETCODE_INVALID_PARAM:
        GST_ERROR_OBJECT (enc,
            "Failed to open encoder (status = %d), invalid parameter values given: %s",
            ret, p_ctx->param_err_msg);
        ni_device_session_close (p_ctx, enc->encoder_eof,
            NI_DEVICE_TYPE_ENCODER);
        ni_device_session_context_clear (p_ctx);
        return GST_FLOW_ERROR;
      default:
        GST_ERROR_OBJECT (enc,
            "Failed to open encoder (status = %d), resource unavailable", ret);
        ni_device_session_close (p_ctx, enc->encoder_eof,
            NI_DEVICE_TYPE_ENCODER);
        ni_device_session_context_clear (p_ctx);
        return GST_FLOW_ERROR;
    }
    // set up ROI map if in ROI demo mode
    // Note: this is for demo purpose, and its direct access to QP map in
    //       session context is not the usual way to do ROI; the normal way is
    //       through side data of GstVideoCodec frame, or aux data of ni_frame
    //       in libxcoder
    if (p_param->cfg_enc_params.roi_enable &&
        (1 == p_param->roi_demo_mode || 2 == p_param->roi_demo_mode)) {
      if (ni_set_demo_roi_map (p_ctx) < 0) {
        return GST_FLOW_ERROR;
      }
    }
  }                             //end if(first_frame && ctx->started == 0)

  if (enc->encoder_flushing) {
    if (!frame && g_queue_get_length (enc->pending_frames) == 0) {
      GST_DEBUG_OBJECT (enc, "XCoder EOF: null frame && fifo empty\n");
      return GST_FLOW_EOS;
    }
  }

  if (!frame) {
    if (g_queue_get_length (enc->pending_frames) == 0) {
      enc->eos_fme_received = 1;
      GST_DEBUG_OBJECT (enc, "null frame, eos_fme_received = 1\n");
    }
  } else {
    GST_DEBUG_OBJECT (enc,
        "XCoder send frame #%" PRIu64 "\n", p_ctx->frame_num);

    // queue up the frame if fifo is NOT empty, or: sequence change ongoing !
    if (g_queue_get_length (enc->pending_frames) > 0 ||
        SESSION_RUN_STATE_SEQ_CHANGE_DRAINING == p_ctx->session_run_state) {
      g_queue_push_tail (enc->pending_frames,
          gst_video_codec_frame_ref (frame));

      if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING == p_ctx->session_run_state) {
        GST_DEBUG_OBJECT (enc,
            "XCoder doing sequence change, frame #%" PRIu64
            " queued and return 0 !", p_ctx->frame_num);
        return GST_FLOW_OK;
      }
    } else {
      cur_frame = frame;
    }
  }

  if (enc->started == 0) {
    p_frame->data.frame.start_of_stream = 1;
    enc->started = 1;
  } else if (p_ctx->session_run_state == SESSION_RUN_STATE_SEQ_CHANGE_OPENING) {
    p_frame->data.frame.start_of_stream = 1;

  } else {
    p_frame->data.frame.start_of_stream = 0;
  }

  if (g_queue_get_length (enc->pending_frames) == 0) {
    GST_DEBUG_OBJECT (enc, "no frame in fifo to send, just send/receive ..\n");
    if (enc->eos_fme_received) {
      GST_DEBUG_OBJECT (enc, "no frame in fifo to send, send eos ..\n");
    }
  } else {
    GST_DEBUG_OBJECT (enc, "av_fifo_generic_peek fme\n");
    cur_frame = g_queue_peek_head (enc->pending_frames);
  }

  if (!enc->eos_fme_received) {
    int8_t bit_depth = 1;
    if (ishwframe) {
      GstMemory *ni_mem = gst_buffer_peek_memory (cur_frame->input_buffer, 0);
      nisession = gst_session_from_ni_hw_memory (ni_mem);
      nisurface = gst_surface_from_ni_hw_memory (ni_mem);
      if (GST_VIDEO_FORMAT_ARGB == info->finfo->format ||
          GST_VIDEO_FORMAT_ABGR == info->finfo->format ||
          GST_VIDEO_FORMAT_RGBA == info->finfo->format ||
          GST_VIDEO_FORMAT_BGRA == info->finfo->format) {
        bit_depth = 1;
      } else {
        bit_depth = nisurface->bit_depth;
      }
    } else {
      if (GST_VIDEO_FORMAT_I420_10LE == info->finfo->format ||
          GST_VIDEO_FORMAT_I420_10BE == info->finfo->format ||
          GST_VIDEO_FORMAT_P010_10LE == info->finfo->format) {
        bit_depth = 2;
      }
    }

    if ((info->height && info->width) &&
        (info->height != enc->height ||
            info->width != enc->width ||
            bit_depth != p_ctx->bit_depth_factor)) {
      GST_DEBUG_OBJECT (enc,
          "xcoder_send_frame resolution change %dx%d -> %dx%d or bit depth change %d -> %d\n",
          enc->width, enc->height, info->width, info->height,
          p_ctx->bit_depth_factor, bit_depth);

      p_ctx->session_run_state = SESSION_RUN_STATE_SEQ_CHANGE_DRAINING;
      enc->eos_fme_received = 1;

      // have to queue this frame if not done so: an empty queue
      if (g_queue_get_length (enc->pending_frames) == 0) {
        GST_DEBUG_OBJECT (enc,
            "resolution change when fifo empty, frame #%" PRIu64
            " being queued ..", p_ctx->frame_num);

        g_queue_push_tail (enc->pending_frames,
            gst_video_codec_frame_ref (frame));
      }
    }
  }

  p_frame->data.frame.preferred_characteristics_data_len = 0;
  p_frame->data.frame.end_of_stream = 0;
  p_frame->data.frame.force_key_frame =
      p_frame->data.frame.use_cur_src_as_long_term_pic =
      p_frame->data.frame.use_long_term_ref = 0;

  p_frame->data.frame.sei_total_len =
      p_frame->data.frame.sei_cc_offset = p_frame->data.frame.sei_cc_len =
      p_frame->data.frame.sei_hdr_mastering_display_color_vol_offset =
      p_frame->data.frame.sei_hdr_mastering_display_color_vol_len =
      p_frame->data.frame.sei_hdr_content_light_level_info_offset =
      p_frame->data.frame.sei_hdr_content_light_level_info_len =
      p_frame->data.frame.sei_hdr_plus_offset =
      p_frame->data.frame.sei_hdr_plus_len = 0;

  p_frame->data.frame.roi_len = 0;
  p_frame->data.frame.reconf_len = 0;
  p_frame->data.frame.force_pic_qp = 0;

  if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
      p_ctx->session_run_state ||
      (enc->eos_fme_received
          && g_queue_get_length (enc->pending_frames) == 0)) {
    GST_DEBUG_OBJECT (enc, "XCoder start flushing\n");
    p_frame->data.frame.end_of_stream = 1;
    enc->encoder_flushing = 1;
  } else {
    format_in_use = convertGstVideoFormatToXcoderPixFmt
        (enc->input_state->info.finfo->format);
    // NETINT_INTERNAL - currently only for internal testing
    // reset encoder change data buffer for reconf parameters
    if (p_param->reconf_demo_mode > XCODER_TEST_RECONF_OFF &&
        p_param->reconf_demo_mode < XCODER_TEST_RECONF_END) {
      memset (p_ctx->enc_change_params, 0, sizeof (ni_encoder_change_params_t));
    }
    // extra data starts with metadata header, various aux data sizes
    // have been reset above
    p_frame->data.frame.extra_data_len = NI_APP_ENC_FRAME_META_DATA_SIZE;

    gst_ni_add_metadata (enc, cur_frame, &dec_frame, aux_data);

    p_frame->data.frame.ni_pict_type = 0;

    if (GST_VIDEO_CODEC_FRAME_IS_FORCE_KEYFRAME (cur_frame)) {
      p_frame->data.frame.force_key_frame = 1;
      p_frame->data.frame.ni_pict_type = PIC_TYPE_IDR;
    }

    GST_INFO_OBJECT (enc,
        "xcoder_send_frame: #%" PRIu64
        " ni_pict_type %d forced_header_enable %d intraPeriod %d",
        p_ctx->frame_num, p_frame->data.frame.ni_pict_type,
        p_param->cfg_enc_params.forced_header_enable,
        p_param->cfg_enc_params.intra_period);

    // whether should send SEI with this frame
    send_sei_with_idr =
        ni_should_send_sei_with_frame (p_ctx, p_frame->data.frame.ni_pict_type,
        p_param);

    // prep for auxiliary data (various SEI, ROI) in encode frame, based on the
    // data returned in decoded frame
    ni_enc_prep_aux_data (p_ctx, &p_frame->data.frame, &dec_frame,
        p_ctx->codec_format, send_sei_with_idr,
        mdcv_data, cll_data, cc_data, udu_data, hdrp_data);

    // Netint Custom Sei Meta Data
    GstNetintPrivateMeta *custom_meta;
    gpointer iter_md = NULL;

    while ((custom_meta = (GstNetintPrivateMeta *)
            gst_buffer_iterate_meta_filtered (cur_frame->input_buffer,
                &iter_md, GST_NETINT_PRIVATE_META_API_TYPE))) {
      int64_t local_pts = cur_frame->pts;
      uint8_t *p_src_sei_data, *p_dst_sei_data;
      int sei_size;
      uint8_t sei_type;
      int size;
      ni_custom_sei_set_t *src_custom_sei_set, *dst_custom_sei_set;
      ni_custom_sei_t *p_src_custom_sei, *p_dst_custom_sei;

      // if one picture can be skipped, nienc will send that frame but will not
      // receive packet, therefore it will skip the free in receive packet as
      // well and cause memory leak. So check the last pkt_custom_sei_set has
      // been released or not.
      dst_custom_sei_set = p_ctx->pkt_custom_sei_set[local_pts % NI_FIFO_SZ];
      if (dst_custom_sei_set) {
        free (dst_custom_sei_set);
      }

      /* copy the whole SEI data */
      src_custom_sei_set = (ni_custom_sei_set_t *) custom_meta->data;
      dst_custom_sei_set = g_new (ni_custom_sei_set_t, 1);
      if (dst_custom_sei_set == NULL) {
        GST_ERROR_OBJECT (enc,
            "failed to allocate memory for custom meta data\n");
        return GST_FLOW_ERROR;
      }
      memset (dst_custom_sei_set, 0, sizeof (ni_custom_sei_set_t));

      /* fill sei data */
      for (i = 0; i < src_custom_sei_set->count; i++) {
        int len;
        p_src_custom_sei = &src_custom_sei_set->custom_sei[i];
        sei_size = p_src_custom_sei->size;
        sei_type = p_src_custom_sei->type;
        p_src_sei_data = &p_src_custom_sei->data[0];

        p_dst_custom_sei = &dst_custom_sei_set->custom_sei[i];
        p_dst_sei_data = &p_dst_custom_sei->data[0];
        size = 0;

        // long start code
        p_dst_sei_data[size++] = 0x00;
        p_dst_sei_data[size++] = 0x00;
        p_dst_sei_data[size++] = 0x00;
        p_dst_sei_data[size++] = 0x01;

        if (enc->codec_format == NI_CODEC_FORMAT_H264) {
          p_dst_sei_data[size++] = 0x06;        //nal type: SEI
        } else {
          p_dst_sei_data[size++] = 0x4e;        //nal type: SEI
          p_dst_sei_data[size++] = 0x01;
        }

        // SEI type
        p_dst_sei_data[size++] = sei_type;

        // original payload size
        len = sei_size;
        while (len >= 0) {
          p_dst_sei_data[size++] = len > 0xff ? 0xff : len;
          len -= 0xff;
        }

        // payload data
        for (j = 0; j < sei_size && size < NI_MAX_CUSTOM_SEI_DATA - 1; j++) {
          if (j >= 2 && !p_dst_sei_data[size - 2] && !p_dst_sei_data[size - 1]
              && p_src_sei_data[j] <= 0x03) {
            /* insert 0x3 as emulation_prevention_three_byte */
            p_dst_sei_data[size++] = 0x03;
          }
          p_dst_sei_data[size++] = p_src_sei_data[j];
        }

        if (j != sei_size) {
          GST_INFO_OBJECT (enc,
              "%s: sei RBSP size out of limit(%d), idx=%u, type=%u, size=%d, custom_sei_loc=%d.",
              __func__, NI_MAX_CUSTOM_SEI_DATA, i, sei_type, sei_size,
              p_src_custom_sei->location);
          free (dst_custom_sei_set);
          break;
        }
        // trailing byte
        p_dst_sei_data[size++] = 0x80;

        p_dst_custom_sei->size = size;
        p_dst_custom_sei->type = sei_type;
        p_dst_custom_sei->location = p_src_custom_sei->location;
        GST_DEBUG_OBJECT (enc,
            "%s: custom sei idx %d type %u len %d loc %d.\n",
            __func__, i, sei_type, size, p_dst_custom_sei->location);
      }

      dst_custom_sei_set->count = src_custom_sei_set->count;
      p_ctx->pkt_custom_sei_set[local_pts % NI_FIFO_SZ] = dst_custom_sei_set;
      GST_DEBUG_OBJECT (enc,
          "%s: sei number %d pts %" PRId64 ".\n",
          __func__, dst_custom_sei_set->count, local_pts);
    }

    if (p_frame->data.frame.sei_total_len > NI_ENC_MAX_SEI_BUF_SIZE) {
      GST_ERROR_OBJECT (enc,
          "xcoder_send_frame: sei total length %u exceeds maximum sei size %u.",
          p_frame->data.frame.sei_total_len, NI_ENC_MAX_SEI_BUF_SIZE);
      return GST_FLOW_ERROR;
    }

    p_frame->data.frame.extra_data_len += p_frame->data.frame.sei_total_len;

    // data layout requirement: leave space for reconfig data if at least one of
    // reconfig, SEI or ROI is present
    // Note: ROI is present when enabled, so use encode config flag instead of
    //       frame's roi_len as it can be 0 indicating a 0'd ROI map setting !
    if (p_frame->data.frame.reconf_len ||
        p_frame->data.frame.sei_total_len ||
        p_param->cfg_enc_params.roi_enable) {
      p_frame->data.frame.extra_data_len += sizeof (ni_encoder_change_params_t);
    }
    // Assign system_frame_number rather than PTS to avoid issues caused by
    // repeated PTS. It can also be returned along with the corresponding
    // encoded packet
    p_frame->data.frame.pts = cur_frame->system_frame_number;
    p_frame->data.frame.dts = cur_frame->dts;

    p_frame->data.frame.video_width = enc->width;
    p_frame->data.frame.video_height = enc->height;

    ishwframe = enc->hardware_mode;
    if (p_ctx->auto_dl_handle != 0 || (enc->height < NI_MIN_HEIGHT) ||
        (enc->width < NI_MIN_WIDTH)) {
      p_ctx->hw_action = 0;
      ishwframe = 0;
    }
    isnv12frame = (format_in_use == GST_VIDEO_FORMAT_NV12
        || format_in_use == GST_VIDEO_FORMAT_P010_10LE);

    if (ishwframe) {
      ret = sizeof (niFrameSurface1_t);
    } else {
      ret =
          calculateSwFrameSize (info->width, info->height,
          convertGstVideoFormatToXcoderPixFmt (info->finfo->format));
    }

    GST_DEBUG_OBJECT (enc,
        "xcoder_send_frame: frame->format=%d, frame->width=%d, frame->height=%d, size=%d",
        format_in_use, info->width, info->height, ret);
    if (ret < 0) {
      return GST_FLOW_ERROR;
    }

    int dst_stride[NI_MAX_NUM_DATA_POINTERS] = { 0 };
    int height_aligned[NI_MAX_NUM_DATA_POINTERS] = { 0 };
    int src_height[NI_MAX_NUM_DATA_POINTERS] = { 0 };

    src_height[0] = info->height;
    src_height[1] = info->height / 2;
    src_height[2] = (isnv12frame) ? 0 : (info->height / 2);
    ni_pix_fmt_t ni_format = NI_PIX_FMT_NONE;
    switch (enc->pix_fmt) {
      case GST_VIDEO_FORMAT_I420:
        ni_format = NI_PIX_FMT_YUV420P;
        break;
      case GST_VIDEO_FORMAT_I420_10LE:
        ni_format = NI_PIX_FMT_YUV420P10LE;
        break;
      case GST_VIDEO_FORMAT_NV12:
        ni_format = NI_PIX_FMT_NV12;
        break;
      case GST_VIDEO_FORMAT_P010_10LE:
        ni_format = NI_PIX_FMT_P010LE;
        break;
      case GST_VIDEO_FORMAT_P010_10BE:
        ni_format = NI_PIX_FMT_P010LE;
        break;
      case GST_VIDEO_FORMAT_I420_10BE:
        ni_format = NI_PIX_FMT_YUV420P10LE;
        break;
      case GST_VIDEO_FORMAT_ARGB:
        ni_format = NI_PIX_FMT_ARGB;
        break;
      case GST_VIDEO_FORMAT_RGBA:
        ni_format = NI_PIX_FMT_RGBA;
        break;
      case GST_VIDEO_FORMAT_ABGR:
        ni_format = NI_PIX_FMT_ABGR;
        break;
      case GST_VIDEO_FORMAT_BGRA:
        ni_format = NI_PIX_FMT_BGRA;
        break;
#if ENABLE_NI_GST_FRAMEWORK_PATCH
      case GST_VIDEO_FORMAT_NI_QUAD_8_4L4:
        ni_format = NI_PIX_FMT_8_TILED4X4;
        break;
      case GST_VIDEO_FORMAT_NI_QUAD_10_4L4:
        ni_format = NI_PIX_FMT_10_TILED4X4;
        break;
#endif
      default:
        GST_ERROR_OBJECT (enc, "Ni enc don't support [%d]", enc->pix_fmt);
        return GST_FLOW_NOT_SUPPORTED;
    }
    if (enc->pix_fmt == GST_VIDEO_FORMAT_ARGB ||
        enc->pix_fmt == GST_VIDEO_FORMAT_RGBA ||
        enc->pix_fmt == GST_VIDEO_FORMAT_ABGR ||
        enc->pix_fmt == GST_VIDEO_FORMAT_BGRA) {
      src_height[0] = info->width;
      src_height[1] = 0;
      src_height[2] = 0;
      alignment_2pass_wa = 0;
    }

    if (p_param->luma_linesize) {
      alignment_2pass_wa = false;
    }

    ni_get_min_frame_dim (info->width, info->height,
        ni_format, dst_stride, height_aligned);

    GST_DEBUG_OBJECT (enc,
        "xcoder_send_frame frame->width %d ctx->api_ctx.bit_depth_factor %d dst_stride[0/1/2] %d/%d/%d",
        info->width, p_ctx->bit_depth_factor,
        dst_stride[0], dst_stride[1], dst_stride[2]);

    if (alignment_2pass_wa && !ishwframe) {
      if (isnv12frame) {
        // for 2-pass encode output mismatch WA, need to extend (and // pad)
        // CbCr plane height, because 1st pass assume input 32 align
        height_aligned[1] = NI_ALIGN (height_aligned[0], 32) / 2;
      } else {
        // for 2-pass encode output mismatch WA, need to extend (and
        // pad) Cr plane height, because 1st pass assume input 32 align
        height_aligned[2] = NI_ALIGN (height_aligned[0], 32) / 2;
      }
    }
    // alignment(16) extra padding for H.264 encoding
    if (ishwframe) {
      uint8_t *dsthw;
      const uint8_t *srchw;

      ni_frame_buffer_alloc_hwenc (&(p_frame->data.frame),
          info->width, info->height, (int) p_frame->data.frame.extra_data_len);
      if (!p_frame->data.frame.p_data[3]) {
        return GST_FLOW_ERROR;
      }
      dsthw = p_frame->data.frame.p_data[3];
      srchw = (const uint8_t *) nisurface;
      memcpy (dsthw, srchw, sizeof (niFrameSurface1_t));
    } else {
      uint8_t *vdata[GST_VIDEO_MAX_PLANES] = { 0 };
      gint vstride[GST_VIDEO_MAX_PLANES] = { 0 };

      //filter case: hw-frame is less than min resolution, and need to download first, then expand it
      if (!enc->hardware_mode) {
        GstVideoFrame vframe;
        memset (&vframe, 0, sizeof (GstVideoFrame));

        if (gst_video_frame_map (&vframe, info, cur_frame->input_buffer,
                GST_MAP_READ)) {
          for (int i = 0; i < GST_VIDEO_MAX_PLANES; i++) {
            vdata[i] = GST_VIDEO_FRAME_PLANE_DATA (&vframe, i);
            vstride[i] = GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, i);
          }
          gst_video_frame_unmap (&vframe);
        } else {
          GST_ERROR_OBJECT (enc, "Faile to gst_video_frame_map for cur_frame");
          return GST_FLOW_ERROR;
        }

        GST_DEBUG_OBJECT (enc,
            "[0] %p stride[0] %u height %u data[1] %p data[3] %p\n",
            vdata[0], dst_stride[0], info->height, vdata[1], vdata[3]);
      }
      // Use actual buffer stride from gst_video_frame_map (respects GstVideoMeta)
      // rather than info->stride from caps, as upstream elements like videoscale
      // may change stride via GstVideoMeta without matching GstVideoInfo stride.
      const gint *frame_stride = vstride[0] ? vstride : info->stride;

      // Check if zero copy is still valid for this frame. Skip the call to
      // ni_encoder_frame_zerocopy_check when the buffer stride clearly differs
      // from the encoder's configured linesize, to avoid per-frame error logging
      // from libxcoder ("linesize changed ... resolution change?").
      if (p_param->luma_linesize &&
          (int) frame_stride[0] == p_param->luma_linesize &&
          (!p_param->chroma_linesize ||
              (int) frame_stride[1] == p_param->chroma_linesize) &&
          ni_encoder_frame_zerocopy_check (p_ctx,
              p_param, info->width, info->height,
              (const int *) frame_stride, false) == NI_RETCODE_SUCCESS) {
        need_to_copy = 0;
        // alloc metadata buffer etc. (if needed)
        ret = ni_encoder_frame_zerocopy_buffer_alloc (&(p_frame->data.frame),
            info->width, info->height, (const int *) frame_stride,
            (const uint8_t **) vdata, (int) p_frame->data.frame.extra_data_len);
        if (ret != NI_RETCODE_SUCCESS)
          return GST_FLOW_ERROR;
      } else {
        // Reset zero-copy transfer flags from any prior zerocopy_buffer_alloc
        // call.  Without this, the NVMe write path would use
        // p_buffer + total_start_len as the data pointer, offsetting the
        // 4096-aligned base and triggering an extra memcpy on every frame.
        p_frame->data.frame.separate_start = 0;
        p_frame->data.frame.total_start_len = 0;
        memset (p_frame->data.frame.start_len, 0,
            sizeof (p_frame->data.frame.start_len));
        p_frame->data.frame.inconsecutive_transfer = 0;
        p_frame->data.frame.separate_metadata = 0;

        // if linesize changes (while resolution remains the same), copy to previously configured linesizes
        if (p_param->luma_linesize && p_param->chroma_linesize) {
          dst_stride[0] = p_param->luma_linesize;
          dst_stride[1] = dst_stride[2] = p_param->chroma_linesize;
        }
        ret =
            ni_encoder_sw_frame_buffer_alloc (!isnv12frame,
            &(p_frame->data.frame), info->width, height_aligned[0], dst_stride,
            (enc->codec_format == NI_CODEC_FORMAT_H264),
            (int) p_frame->data.frame.extra_data_len, alignment_2pass_wa);
        if (ret != NI_RETCODE_SUCCESS)
          return GST_FLOW_ERROR;
      }
      GST_DEBUG_OBJECT (enc,
          "%p need_to_copy %d!\n", p_frame->data.frame.p_buffer, need_to_copy);
      if (!p_frame->data.frame.p_data[0]) {
        return GST_FLOW_ERROR;
      }

      if (!enc->hardware_mode) {
        GST_DEBUG_OBJECT (enc,
            "xcoder_send_frame: fme.data_len[0]=%d, "
            "buf_fme->linesize=%d/%d/%d, dst alloc linesize = %d/%d/%d, "
            "src height = %d/%d/%d, dst height aligned = %d/%d/%d, "
            "force_key_frame=%d, extra_data_len=%d sei_size=%d "
            "(hdr_content_light_level %u hdr_mastering_display_color_vol %u "
            "hdr10+ %u cc %u udu %u prefC %u) roi_size=%u reconf_size=%u "
            "force_pic_qp=%u "
            "use_cur_src_as_long_term_pic %u use_long_term_ref %u\n",
            p_frame->data.frame.data_len[0],
            vstride[0], vstride[1], vstride[2],
            dst_stride[0], dst_stride[1], dst_stride[2], src_height[0],
            src_height[1], src_height[2], height_aligned[0], height_aligned[1],
            height_aligned[2], p_frame->data.frame.force_key_frame,
            p_frame->data.frame.extra_data_len,
            p_frame->data.frame.sei_total_len,
            p_frame->data.frame.sei_hdr_content_light_level_info_len,
            p_frame->data.frame.sei_hdr_mastering_display_color_vol_len,
            p_frame->data.frame.sei_hdr_plus_len,
            p_frame->data.frame.sei_cc_len,
            p_frame->data.frame.sei_user_data_unreg_len,
            p_frame->data.frame.preferred_characteristics_data_len,
            (p_param->cfg_enc_params.roi_enable ? p_ctx->roi_len : 0),
            p_frame->data.frame.reconf_len, p_frame->data.frame.force_pic_qp,
            p_frame->data.frame.use_cur_src_as_long_term_pic,
            p_frame->data.frame.use_long_term_ref);

        // YUV part of the encoder input data layout
        if (need_to_copy) {
          ni_copy_frame_data (
              (uint8_t **) (p_frame->data.frame.p_data),
              vdata, info->width, info->height, p_ctx->bit_depth_factor,
              ni_format, p_param->cfg_enc_params.conf_win_right, dst_stride,
              height_aligned, (int *) frame_stride, src_height);
        }
      } else {
        ni_session_data_io_t *p_session_data;
        ni_session_data_io_t niframe;
        niFrameSurface1_t *src_surf;

        GST_DEBUG_OBJECT (enc,
            "xcoder_send_frame:Autodownload to be run: hdl: %d w: %d h: %d\n",
            p_ctx->auto_dl_handle, enc->width, enc->height);

        src_surf = (niFrameSurface1_t *) nisurface;

        if (enc->height < NI_MIN_HEIGHT || enc->width < NI_MIN_WIDTH) {
          int bit_depth;
          int is_planar;

          p_session_data = &niframe;
          memset (&niframe, 0, sizeof (niframe));
          bit_depth = ((enc->input_state->info.finfo->format ==
                  GST_VIDEO_FORMAT_I420_10LE)
              || (enc->input_state->info.finfo->format ==
                  GST_VIDEO_FORMAT_P010_10LE))
              ? 2 : 1;
          is_planar =
              (enc->input_state->info.finfo->format == GST_VIDEO_FORMAT_I420)
              || (enc->input_state->info.finfo->format ==
              GST_VIDEO_FORMAT_I420_10LE);

          /* Allocate a minimal frame */
          ni_enc_frame_buffer_alloc (&niframe.data.frame, enc->width, enc->height, 0,   /* alignment */
              1,                /* metadata */
              bit_depth, 0,     /* hw_frame_count */
              is_planar, ni_format);
        } else {
          p_session_data = p_frame;
        }

        nisession->is_auto_dl = true;
        ret = ni_device_session_hwdl (nisession, p_session_data, src_surf);
        ishwframe = false;
        if (ret <= 0) {
          GST_DEBUG_OBJECT (enc,
              "ni_device_session_hwdl() failed to retrieve frame\n");
          return GST_FLOW_ERROR;
        }
        //try to recycle HWframe after download
        gst_niquadraenc_free_input_frame (enc, src_surf->ui16FrameIdx);

        if ((enc->height < NI_MIN_HEIGHT) || (enc->width < NI_MIN_WIDTH)) {
          expand_ni_frame (&p_frame->data.frame,
              &p_session_data->data.frame, dst_stride,
              enc->width, enc->height, enc->input_state->info.finfo->format);

          ni_frame_buffer_free (&niframe.data.frame);
        }
      }
    }                           // end if hwframe else

    // auxiliary data part of the encoder input data layout
    ni_enc_copy_aux_data (p_ctx, &p_frame->data.frame, &dec_frame,
        p_ctx->codec_format, mdcv_data, cll_data,
        cc_data, udu_data, hdrp_data, ishwframe, isnv12frame);

    ni_frame_buffer_free (&dec_frame);

    // end of encode input frame data layout
  }                             // end non seq change

  sent = ni_device_session_write (p_ctx, p_frame, NI_DEVICE_TYPE_ENCODER);
  GST_DEBUG_OBJECT (enc, "xcoder_send_frame: size %d sent to xcoder\n", sent);

  if (NI_RETCODE_ERROR_VPU_RECOVERY == sent) {
    if (!xcoder_encode_reset (enc)) {
      flow_ret = GST_FLOW_ERROR;
    }
  } else if (sent < 0) {
    GST_ERROR_OBJECT (enc, "xcoder_send_frame(): failure sent (%d)\n", sent);

    // if rejected due to sequence change in progress, revert resolution
    // setting and will do it again next time.
    if (p_frame->data.frame.start_of_stream &&
        (enc->width != orig_avctx_width || enc->height != orig_avctx_height)) {
      enc->width = orig_avctx_width;
      enc->height = orig_avctx_height;
    }
    return GST_FLOW_ERROR;
  } else {
    GST_DEBUG_OBJECT (enc, "xcoder_send_frame(): sent (%d)\n", sent);
    if (sent == 0) {
      // case of sequence change in progress
      if (p_frame->data.frame.start_of_stream &&
          (enc->width != orig_avctx_width ||
              enc->height != orig_avctx_height)) {
        enc->width = orig_avctx_width;
        enc->height = orig_avctx_height;
      }
      // when buffer_full, drop the frame and return EAGAIN if in strict timeout
      // mode, otherwise buffer the frame and it is to be sent out using encode2
      // API: queue the frame only if not done so yet, i.e. queue is empty
      // *and* it's a valid frame.
      if (p_ctx->status == NI_RETCODE_NVME_SC_WRITE_BUFFER_FULL) {
        ishwframe = enc->hardware_mode;
        if (ishwframe) {
          // Do not queue frames to avoid gstreamer stuck when multiple HW frames are queued up, causing decoder unable to acquire buffer, which led to gstreamer stuck
          GST_ERROR_OBJECT (enc,
              "xcoder_send_frame(): device WRITE_BUFFER_FULL cause frame drop! "
              "(approx. Frame num #%" PRIu64 "\n", p_ctx->frame_num);
          flow_ret = GST_FLOW_ERROR;
        } else {
          GST_DEBUG_OBJECT (enc, "xcoder_send_frame(): Write buffer full\n");
          flow_ret = GST_FLOW_OK;

          if (frame && g_queue_get_length (enc->pending_frames) == 0) {
            g_queue_push_tail (enc->pending_frames,
                gst_video_codec_frame_ref (frame));
          }
        }
      }
    } else {
      // only if it's NOT sequence change flushing (in which case only the eos
      // was sent and not the first sc pkt) AND
      // only after successful sending will it be removed from list
      if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING != p_ctx->session_run_state) {
        if (g_queue_get_length (enc->pending_frames) > 0) {
          GstVideoCodecFrame *pframe = g_queue_pop_head (enc->pending_frames);
          gst_video_codec_frame_unref (pframe);
        }
      }
      // pushing input pts in circular FIFO
      p_ctx->enc_pts_list[p_ctx->enc_pts_w_idx % NI_FIFO_SZ] = cur_frame->pts;
      p_ctx->enc_pts_w_idx++;

      // have another check before return: if no more frames in fifo to send and
      // we've got eos (NULL) frame from upper stream, flag for flushing
      if (enc->eos_fme_received
          && g_queue_get_length (enc->pending_frames) == 0) {
        GST_DEBUG_OBJECT (enc,
            "Upper stream EOS frame received, fifo empty, start flushing ..");
        enc->encoder_flushing = 1;
      }

      *send_frame = TRUE;
      flow_ret = GST_FLOW_OK;
    }
  }

  if (enc->encoder_flushing) {
    GST_DEBUG_OBJECT (enc, "xcoder_send_frame flushing ..");
    ret = ni_device_session_flush (p_ctx, NI_DEVICE_TYPE_ENCODER);
    if (ret < 0) {
      flow_ret = GST_FLOW_ERROR;
    }
  }

  return flow_ret;
}

static gboolean
allocate_and_map_output_buffer (GstNiquadraEnc * enc, GstBuffer ** outbuf,
    GstMapInfo * map_info, gsize size)
{
  *outbuf = gst_video_encoder_allocate_output_buffer (GST_VIDEO_ENCODER (enc),
      size);
  if (G_UNLIKELY (!*outbuf)) {
    GST_ERROR_OBJECT (enc, "Failed to allocate buffer from pool");
    return FALSE;
  }

  if (!gst_buffer_map (*outbuf, map_info, GST_MAP_WRITE)) {
    GST_ERROR_OBJECT (enc, "Failed to map allocated buffer memory");
    gst_buffer_unref (*outbuf);
    *outbuf = NULL;
    return FALSE;
  }

  return TRUE;
}

static NiPluginError
xcoder_receive_packet (GstNiquadraEnc * enc, ni_packet_t * xpkt,
    GstBuffer ** outbuf, gint * size)
{
  ni_session_data_io_t *p_pkt =
      gst_niquadra_context_get_data_pkt (enc->context);
  ni_session_context_t *p_ctx =
      gst_niquadra_context_get_session_context (enc->context);
  ni_xcoder_params_t *p_enc_params =
      gst_niquadra_context_get_xcoder_param (enc->context);
  GstVideoCodecFrame *received_frame = NULL;
  gboolean send_frame = FALSE;
  NiPluginError ret = NI_PLUGIN_OK;
  int i, recv;
  bool av1_output_frame = 0;
  GstMapInfo map_info = GST_MAP_INFO_INIT;
  uint8_t *p_data = NULL;

  GST_DEBUG_OBJECT (enc, "XCoder receive packet\n");

  *outbuf = NULL;

  if (enc->encoder_eof) {
    GST_DEBUG_OBJECT (enc, "xcoder_receive_packet: EOS\n");
    return NI_PLUGIN_EOF;
  }

  if (ni_packet_buffer_alloc (xpkt, NI_MAX_TX_SZ)) {
    GST_ERROR_OBJECT (enc,
        "xcoder_receive_packet: packet buffer size %d allocation failed\n",
        NI_MAX_TX_SZ);
    return NI_PLUGIN_ENOMEM;
  }

  if (enc->codec_format == NI_CODEC_FORMAT_JPEG && (!enc->spsPpsArrived)) {
    enc->spsPpsArrived = 1;
    // for Jpeg, start pkt_num counter from 1, because unlike video codecs
    // (1st packet is header), there is no header for Jpeg
    p_ctx->pkt_num = 1;
  }

  while (1) {
    xpkt->recycle_index = -1;
    recv = ni_device_session_read (p_ctx, p_pkt, NI_DEVICE_TYPE_ENCODER);

    GST_DEBUG_OBJECT (enc,
        "XCoder receive packet: xpkt.end_of_stream=%d, xpkt.data_len=%d,"
        "xpkt.frame_type=%d, recv=%d, encoder_flushing=%d, encoder_eof=%d\n",
        xpkt->end_of_stream, xpkt->data_len, xpkt->frame_type, recv,
        enc->encoder_flushing, enc->encoder_eof);

    if (recv <= 0) {
      enc->encoder_eof = xpkt->end_of_stream;
      if (enc->encoder_eof || xpkt->end_of_stream) {
        if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING == p_ctx->session_run_state) {
          // after sequence change completes, reset codec state
          GST_DEBUG_OBJECT (enc,
              "xcoder_receive_packet 1: sequence change completed, "
              "return NI_PLUGIN_EAGAIN and will reopen codec!\n");

          ret = xcoder_encode_reinit (enc);
          GST_DEBUG_OBJECT (enc,
              "xcoder_receive_packet: xcoder_encode_reinit ret %d\n", ret);
          if (ret == NI_PLUGIN_OK) {
            ret = NI_PLUGIN_EAGAIN;

            xcoder_send_frame (enc, NULL, &send_frame);

            p_ctx->session_run_state = SESSION_RUN_STATE_NORMAL;
          }
          break;
        }

        ret = NI_PLUGIN_EOF;
        GST_DEBUG_OBJECT (enc,
            "xcoder_receive_packet: got encoder_eof, return NI_PLUGIN_EOF");
        break;
      } else {
        bool bIsReset = false;
        if (NI_RETCODE_ERROR_VPU_RECOVERY == recv) {
          xcoder_encode_reset (enc);
          bIsReset = true;
        }
        ret = NI_PLUGIN_EAGAIN;
        // if encode session was reset, can't read again with invalid session, must break out first
        if ((!enc->encoder_flushing && !enc->eos_fme_received) || bIsReset) {
          GST_DEBUG_OBJECT (enc,
              "xcoder_receive_packet: NOT encoder_flushing, NOT eos_fme_received, "
              "return NI_PLUGIN_EAGAIN");
          break;
        }
      }
    } else {
      /* got encoded data back */
      ret = NI_PLUGIN_OK;
      int meta_size = p_ctx->meta_size;

      if (enc->hardware_mode && xpkt->recycle_index >= 0 &&
          enc->height >= NI_MIN_HEIGHT && enc->width >= NI_MIN_WIDTH &&
          xpkt->recycle_index <
          NI_GET_MAX_HWDESC_FRAME_INDEX (p_ctx->ddr_config)) {
        GST_DEBUG_OBJECT (enc,
            "UNREF trace ui16FrameIdx = [%d].\n", xpkt->recycle_index);

        gst_niquadraenc_free_input_frame (enc, xpkt->recycle_index);
        xpkt->recycle_index = -1;
      }

      if (!enc->spsPpsArrived) {
        ret = NI_PLUGIN_EAGAIN;
        enc->spsPpsArrived = 1;
        enc->spsPpsHdrLen = recv - meta_size;
        enc->p_spsPpsHdr = g_malloc (enc->spsPpsHdrLen);
        if (!enc->p_spsPpsHdr) {
          ret = NI_PLUGIN_ENOMEM;
          break;
        }

        memcpy (enc->p_spsPpsHdr, (uint8_t *) xpkt->p_data + meta_size,
            xpkt->data_len - meta_size);

        // start pkt_num counter from 1 to get the real first frame
        p_ctx->pkt_num = 1;
        // for low-latency mode, keep reading until the first frame is back
        if (p_enc_params->low_delay_mode) {
          GST_DEBUG_OBJECT (enc,
              "XCoder receive packet: low delay mode, keep reading until 1st pkt arrives\n");
          continue;
        }
        break;
      }
      // The encoded packet carries the PTS that was assigned when its
      // corresponding frame was sent to firmware
      enc->cur_frame_index = xpkt->pts;
      received_frame =
          gst_niquadraenc_find_best_frame (enc, enc->cur_frame_index);
      if (!received_frame) {
        received_frame = gst_video_encoder_get_oldest_frame
            (GST_VIDEO_ENCODER (enc));
      }
      xpkt->pts = received_frame->pts;
      gst_video_codec_frame_unref (received_frame);
      received_frame = NULL;

      GST_DEBUG_OBJECT (enc,
          "Frame_type=%d,ret=%d,sps=%d\n",
          xpkt->frame_type, ret, enc->spsPpsArrived);
      // handle pic skip
#if ((LIBXCODER_API_VERSION_MAJOR > 2) || \
     (LIBXCODER_API_VERSION_MAJOR == 2 && LIBXCODER_API_VERSION_MINOR >= 86))
      if (xpkt->frame_type == PIC_NOT_CODED) {
#else
      // 0=I, 1=P, 2=B, 3=not coded / skip
      if (xpkt->frame_type == 3) {
#endif
        ret = NI_PLUGIN_EAGAIN;
        if (enc->first_frame_pts == INT_MIN)
          enc->first_frame_pts = xpkt->pts;
        if (NI_CODEC_FORMAT_AV1 == enc->codec_format) {
          enc->latest_dts = xpkt->pts;
        } else if (enc->total_frames_received < enc->dtsOffset) {
          // guess dts
          enc->latest_dts = enc->first_frame_pts +
              ((enc->gop_offset_count - enc->dtsOffset) * 1);
          enc->gop_offset_count++;
        } else {
          // get dts from pts FIFO
          enc->latest_dts =
              p_ctx->enc_pts_list[p_ctx->enc_pts_r_idx % NI_FIFO_SZ];
          p_ctx->enc_pts_r_idx++;
        }
        if (enc->latest_dts > xpkt->pts) {
          enc->latest_dts = xpkt->pts;
        }
        enc->total_frames_received++;

        if (!enc->encoder_flushing && !enc->eos_fme_received) {
          GST_DEBUG_OBJECT (enc,
              "xcoder_receive_packet: skip picture output, return NI_PLUGIN_EAGAIN\n");
          break;
        } else
          continue;
      }
      // store av1 packets to be merged & sent along with future packet
      int temp_index;
      uint32_t data_len = 0;
      if (enc->codec_format == NI_CODEC_FORMAT_AV1) {
        GST_DEBUG_OBJECT (enc,
            "xcoder_receive_packet: AV1 xpkt buf %p size %d show_frame %d\n",
            xpkt->p_data, xpkt->data_len, xpkt->av1_show_frame);

        if (!xpkt->av1_show_frame) {
          xpkt->av1_p_buffer[xpkt->av1_buffer_index] = xpkt->p_buffer;
          xpkt->av1_p_data[xpkt->av1_buffer_index] = xpkt->p_data;
          xpkt->av1_buffer_size[xpkt->av1_buffer_index] = xpkt->buffer_size;
          xpkt->av1_data_len[xpkt->av1_buffer_index] = xpkt->data_len;
          xpkt->av1_buffer_index++;
          xpkt->p_buffer = NULL;
          xpkt->p_data = NULL;
          xpkt->buffer_size = 0;
          xpkt->data_len = 0;

#if GST_CHECK_VERSION (1, 26, 2)
          ni_encoder_cfg_params_t *p_enc_cfg_params =
              &p_enc_params->cfg_enc_params;
          if (p_enc_cfg_params->spatial_layers > 1) {
            GstVideoCodecFrame *frame =
                gst_niquadraenc_find_best_frame (enc, enc->cur_frame_index);
            if (!frame) {
              frame = gst_video_encoder_get_oldest_frame
                  (GST_VIDEO_ENCODER (enc));
            }
            gst_video_encoder_release_frame (GST_VIDEO_ENCODER (enc), frame);
          }
#endif

          if (xpkt->av1_buffer_index >= MAX_AV1_ENCODER_GOP_NUM) {
            GST_DEBUG_OBJECT (enc,
                "xcoder_receive_packet: recv AV1 not shown frame number %d >= %d, return FAILURE\n",
                xpkt->av1_buffer_index, MAX_AV1_ENCODER_GOP_NUM);
            ret = NI_PLUGIN_FAILURE;
            break;
          } else if (!enc->encoder_flushing && !enc->eos_fme_received) {
            GST_DEBUG_OBJECT (enc,
                "xcoder_receive_packet: recv AV1 not shown frame, return EAGAIN\n");
            ret = NI_PLUGIN_EAGAIN;
            break;
          } else {
            if (ni_packet_buffer_alloc (xpkt, NI_MAX_TX_SZ)) {
              GST_DEBUG_OBJECT (enc,
                  "xcoder_receive_packet: AV1 packet buffer size %d allocation failed during flush",
                  NI_MAX_TX_SZ);
              ret = NI_PLUGIN_ENOMEM;
              break;
            }
            GST_DEBUG_OBJECT (enc,
                "xcoder_receive_packet: recv AV1 not shown frame during flush, continue..");
            continue;
          }
        } else {
          // calculate length of previously received AV1 packets pending for merge
          av1_output_frame = 1;
          for (temp_index = 0; temp_index < xpkt->av1_buffer_index;
              temp_index++) {
            data_len += xpkt->av1_data_len[temp_index] - meta_size;
          }
        }
      }

      uint32_t nalu_type = 0;
      const uint8_t *p_start_code;
      uint32_t stc = -1;
      uint32_t copy_len = 0;
      uint8_t *p_src = (uint8_t *) xpkt->p_data + meta_size;
      uint8_t *p_end = p_src + (xpkt->data_len - meta_size);
      int64_t local_pts = xpkt->pts;
      int total_custom_sei_size = 0;
      int custom_sei_count = 0;
      ni_custom_sei_set_t *p_custom_sei_set;

      p_custom_sei_set = p_ctx->pkt_custom_sei_set[local_pts % NI_FIFO_SZ];
      if (p_custom_sei_set != NULL) {
        custom_sei_count = p_custom_sei_set->count;
        for (i = 0; i < p_custom_sei_set->count; i++) {
          total_custom_sei_size += p_custom_sei_set->custom_sei[i].size;
        }
      }

      if (custom_sei_count) {
        // if HRD or custom sei enabled, search for pic_timing or custom SEI insertion point by
        // skipping non-VCL until video data is found.
        p_start_code = p_src;
        if (NI_CODEC_FORMAT_H265 == enc->codec_format) {
          do {
            stc = -1;
            p_start_code = find_start_code (p_start_code, p_end, &stc);
            nalu_type = (stc >> 1) & 0x3F;
          } while (nalu_type > 31);

          // calc. length to copy
          copy_len = p_start_code - 5 - p_src;
        } else if (NI_CODEC_FORMAT_H264 == enc->codec_format) {
          do {
            stc = -1;
            p_start_code = find_start_code (p_start_code, p_end, &stc);
            nalu_type = stc & 0x1F;
          } while (nalu_type > 5);

          // calc. length to copy
          copy_len = p_start_code - 5 - p_src;
        } else {
          GST_ERROR_OBJECT (enc,
              "xcoder_receive packet: codec %d not supported for SEI !",
              enc->codec_format);
        }
      }

      if (enc->codec_format == NI_CODEC_FORMAT_JPEG && !enc->firstPktArrived) {
        // there is no header for Jpeg, so skip header copy
        enc->firstPktArrived = 1;
        if (enc->first_frame_pts == INT_MIN)
          enc->first_frame_pts = xpkt->pts;
      }

      if (!enc->firstPktArrived) {
        data_len +=
            xpkt->data_len - meta_size + enc->spsPpsHdrLen +
            total_custom_sei_size;
      } else {
        data_len += xpkt->data_len - meta_size + total_custom_sei_size;
      }

      if (!allocate_and_map_output_buffer (enc, outbuf, &map_info, data_len)) {
        ret = NI_PLUGIN_ENOMEM;
        break;
      }
      p_data = map_info.data;

      if (!enc->firstPktArrived) {
        uint8_t *p_dst;
        int sizeof_spspps_attached_to_idr = enc->spsPpsHdrLen;
        enc->firstPktArrived = 1;
        if (enc->first_frame_pts == INT_MIN)
          enc->first_frame_pts = xpkt->pts;

        if (enc->codec_format == NI_CODEC_FORMAT_AV1) {
          GST_DEBUG_OBJECT (enc,
              "xcoder_receive_packet: AV1 first output pkt size %d", data_len);
        }

        p_dst = p_data;
        if (sizeof_spspps_attached_to_idr) {
          memcpy (p_dst, enc->p_spsPpsHdr, enc->spsPpsHdrLen);
          *size += enc->spsPpsHdrLen;
          p_dst += enc->spsPpsHdrLen;
        }

        if (custom_sei_count && enc->codec_format != NI_CODEC_FORMAT_AV1) {
          // copy buf_period
          memcpy (p_dst, p_src, copy_len);
          p_dst += copy_len;
          *size += copy_len;

          for (i = 0; i < custom_sei_count; i++) {
            // copy custom sei
            ni_custom_sei_t *p_custom_sei = &p_custom_sei_set->custom_sei[i];
            if (p_custom_sei->location == NI_CUSTOM_SEI_LOC_AFTER_VCL) {
              break;
            }
            memcpy (p_dst, &p_custom_sei->data[0], p_custom_sei->size);
            p_dst += p_custom_sei->size;
            *size += p_custom_sei->size;
          }

          // copy the IDR data
          memcpy (p_dst, p_src + copy_len,
              xpkt->data_len - meta_size - copy_len);
          p_dst += xpkt->data_len - meta_size - copy_len;
          *size += xpkt->data_len - meta_size - copy_len;

          // copy custom sei after slice
          for (; i < custom_sei_count; i++) {
            ni_custom_sei_t *p_custom_sei = &p_custom_sei_set->custom_sei[i];
            memcpy (p_dst, &p_custom_sei->data[0], p_custom_sei->size);
            p_dst += p_custom_sei->size;
            *size += p_custom_sei->size;
          }
        } else {
          // merge AV1 packets
          if (enc->codec_format == NI_CODEC_FORMAT_AV1) {
            for (temp_index = 0; temp_index < xpkt->av1_buffer_index;
                temp_index++) {
              memcpy (p_dst,
                  (uint8_t *) xpkt->av1_p_data[temp_index] + meta_size,
                  xpkt->av1_data_len[temp_index] - meta_size);
              p_dst += (xpkt->av1_data_len[temp_index] - meta_size);
              *size += (xpkt->av1_data_len[temp_index] - meta_size);
            }
          }

          memcpy (p_dst, (uint8_t *) xpkt->p_data + meta_size,
              xpkt->data_len - meta_size);
          *size += xpkt->data_len - meta_size;
        }
      } else {
        ret = NI_PLUGIN_OK;
        uint8_t *p_dst = p_data;

        if (enc->codec_format == NI_CODEC_FORMAT_AV1) {
          GST_DEBUG_OBJECT (enc,
              "xcoder_receive_packet: AV1 output pkt size %d", data_len);
        }

        if (custom_sei_count && enc->codec_format != NI_CODEC_FORMAT_AV1) {
          memcpy (p_dst, p_src, copy_len);
          p_dst += copy_len;

          for (i = 0; i < custom_sei_count; i++) {
            ni_custom_sei_t *p_custom_sei = &p_custom_sei_set->custom_sei[i];
            if (p_custom_sei->location == NI_CUSTOM_SEI_LOC_AFTER_VCL) {
              break;
            }
            memcpy (p_dst, &p_custom_sei->data[0], p_custom_sei->size);
            p_dst += p_custom_sei->size;
            *size += p_custom_sei->size;
          }

          // copy the packet data
          memcpy (p_dst, p_src + copy_len,
              xpkt->data_len - meta_size - copy_len);
          p_dst += xpkt->data_len - meta_size - copy_len;
          *size += xpkt->data_len - meta_size - copy_len;

          // copy custom sei after slice
          for (; i < custom_sei_count; i++) {
            ni_custom_sei_t *p_custom_sei = &p_custom_sei_set->custom_sei[i];
            memcpy (p_dst, &p_custom_sei->data[0], p_custom_sei->size);
            p_dst += p_custom_sei->size;
            *size += p_custom_sei->size;
          }
        } else {
          // merge AV1 packets
          if (enc->codec_format == NI_CODEC_FORMAT_AV1) {
            for (temp_index = 0; temp_index < xpkt->av1_buffer_index;
                temp_index++) {
              memcpy (p_dst,
                  (uint8_t *) xpkt->av1_p_data[temp_index] + meta_size,
                  xpkt->av1_data_len[temp_index] - meta_size);
              p_dst += (xpkt->av1_data_len[temp_index] - meta_size);
              *size += (xpkt->av1_data_len[temp_index] - meta_size);
            }
          }

          memcpy (p_dst, (uint8_t *) xpkt->p_data + meta_size,
              xpkt->data_len - meta_size);
          *size += xpkt->data_len - meta_size;
        }
      }

      if (custom_sei_count) {
        free (p_custom_sei_set);
        p_ctx->pkt_custom_sei_set[local_pts % NI_FIFO_SZ] = NULL;
      }

      GST_DEBUG_OBJECT (enc, "ret=%d", ret);

      if (ret == NI_PLUGIN_OK) {
        /* to ensure pts>dts for all frames, we assign a guess pts for the first 'dtsOffset' frames and then the pts from input stream
         * is extracted from input pts FIFO.
         * if GOP = IBBBP and PTSs = 0 1 2 3 4 5 .. then out DTSs = -3 -2 -1 0 1 ... and -3 -2 -1 are the guessed values
         * if GOP = IBPBP and PTSs = 0 1 2 3 4 5 .. then out DTSs = -1 0 1 2 3 ... and -1 is the guessed value
         * the number of guessed values is equal to dtsOffset
         */
        if (NI_CODEC_FORMAT_AV1 == enc->codec_format) {
          xpkt->dts = xpkt->pts;
          GST_DEBUG_OBJECT (enc, "Packet dts (av1): %lld", xpkt->dts);
        } else if (enc->total_frames_received < enc->dtsOffset) {
          // guess dts
          xpkt->dts = enc->first_frame_pts +
              ((enc->gop_offset_count - enc->dtsOffset) * enc->ticks_per_frame);
          enc->gop_offset_count++;
          GST_DEBUG_OBJECT (enc, "Packet dts (guessed): %lld", xpkt->dts);
        } else {
          // get dts from pts FIFO
          xpkt->dts = p_ctx->enc_pts_list[p_ctx->enc_pts_r_idx % NI_FIFO_SZ];
          p_ctx->enc_pts_r_idx++;
          GST_DEBUG_OBJECT (enc, "Packet dts: %lld", xpkt->dts);
        }
        if (enc->total_frames_received >= 1) {
          if (xpkt->dts < enc->latest_dts) {
            GST_DEBUG_OBJECT (enc,
                "dts: %lld < latest_dts: %ld", xpkt->dts, enc->latest_dts);
          }
        }

        enc->total_frames_received++;
        enc->latest_dts = xpkt->dts;
        GST_DEBUG_OBJECT (enc,
            "XCoder recv pkt #%" PRId64 " pts %lld dts %lld size %d "
            "frame_type %u avg qp %u",
            p_ctx->pkt_num - 1, xpkt->pts, xpkt->dts, *size, xpkt->frame_type,
            xpkt->avg_frame_qp);
      }
      enc->encoder_eof = xpkt->end_of_stream;
      if (enc->encoder_eof &&
          SESSION_RUN_STATE_SEQ_CHANGE_DRAINING == p_ctx->session_run_state) {
        // after sequence change completes, reset codec state
        GST_DEBUG_OBJECT (enc,
            "xcoder_receive_packet 2: sequence change completed, return 0 and will reopen codec !");
        ret = xcoder_encode_reinit (enc);
        GST_DEBUG_OBJECT (enc,
            "xcoder_receive_packet: xcoder_encode_reinit ret %d", ret);
        if (ret == NI_PLUGIN_OK) {
          xcoder_send_frame (enc, NULL, &send_frame);
          p_ctx->session_run_state = SESSION_RUN_STATE_NORMAL;
        }
      }
      break;
    }
  }

  if ((NI_CODEC_FORMAT_AV1 == enc->codec_format) && xpkt->av1_buffer_index &&
      av1_output_frame) {
    GST_DEBUG_OBJECT (enc,
        "xcoder_receive_packet: ni_packet_buffer_free_av1 %d packtes",
        xpkt->av1_buffer_index);
    ni_packet_buffer_free_av1 (xpkt);
  }

  if (*outbuf) {
    gst_buffer_unmap (*outbuf, &map_info);
    if (ret != NI_PLUGIN_OK) {
      gst_buffer_unref (*outbuf);
      *outbuf = NULL;
    }
  }

  GST_DEBUG_OBJECT (enc, "xcoder_receive_packet: return %d", ret);
  return ret;
}

static GstFlowReturn
gst_niquadraenc_receive_packet (GstNiquadraEnc * enc, gboolean * got_packet,
    gboolean send)
{
  GstBuffer *outbuf = NULL;
  ni_session_data_io_t *p_pkt =
      gst_niquadra_context_get_data_pkt (enc->context);
  GstVideoCodecFrame *frame;
  NiPluginError res = NI_PLUGIN_OK;
  int got_pkt_size = 0;
  GstFlowReturn ret = GST_FLOW_OK;

  *got_packet = FALSE;

  ni_packet_t *xpkt = &(p_pkt->data.packet);

  res = xcoder_receive_packet (enc, xpkt, &outbuf, &got_pkt_size);

  if (res == NI_PLUGIN_EAGAIN) {
    ni_packet_buffer_free (xpkt);
    goto done;
  } else if (res == NI_PLUGIN_EOF) {
    ret = GST_FLOW_EOS;
    goto done;
  } else if (res < 0) {
    ret = GST_FLOW_ERROR;
    goto done;
  }

  *got_packet = TRUE;

  frame = gst_niquadraenc_find_best_frame (enc, enc->cur_frame_index);
  if (!frame) {
    frame = gst_video_encoder_get_oldest_frame (GST_VIDEO_ENCODER (enc));
  }

  if (send) {
    gst_buffer_set_size (outbuf, got_pkt_size);
    frame->output_buffer = outbuf;
    frame->pts = xpkt->pts;
    frame->dts = xpkt->dts;

    enc->encoder_eof = xpkt->end_of_stream;

    GST_DEBUG_OBJECT (enc,
        "xcoder_pts=%lld, xcoder_dts=%lld,output_Pts=%ld,output_Dts=%ld,num=%d",
        xpkt->pts, xpkt->dts, frame->pts, frame->dts,
        frame->system_frame_number);

#if ((LIBXCODER_API_VERSION_MAJOR > 2) || \
     (LIBXCODER_API_VERSION_MAJOR == 2 && LIBXCODER_API_VERSION_MINOR >= 86))
    if (xpkt->frame_type == PIC_TYPE_IDR)
#else
    if (xpkt->frame_type == 0)
#endif
      GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT (frame);
    else
      GST_VIDEO_CODEC_FRAME_UNSET_SYNC_POINT (frame);
  } else {
    gst_buffer_unref (outbuf);
    outbuf = NULL;
  }

  GST_DEBUG_OBJECT (enc,
      "Try to finish frame,size=%d,send=%d,ret=%d,num=%d,ref=%d\n",
      got_pkt_size, send, ret, frame->system_frame_number, frame->ref_count);

  ret = gst_video_encoder_finish_frame (GST_VIDEO_ENCODER (enc), frame);

done:
  GST_DEBUG_OBJECT (enc, "ret=%d,res=%d,got_packet=%d", ret, res, *got_packet);
  return ret;
}

static GstFlowReturn
gst_niquadraenc_handle_frame (GstVideoEncoder * encoder,
    GstVideoCodecFrame * frame)
{
  GstNiquadraEnc *thiz = GST_NIQUADRAENC (encoder);
  gboolean send_frame = FALSE;
  gboolean got_packet = FALSE;
  GstFlowReturn ret = GST_FLOW_OK;

  GST_DEBUG_OBJECT (thiz,
      "encoder handle frame,PTS=%lu,DTS=%lu,num=%d",
      frame->pts, frame->dts, frame->system_frame_number);

  thiz->ticks_per_frame = frame->duration;

#if !GST_CHECK_VERSION (1, 26, 2)
  ni_xcoder_params_t *p_enc_params =
      gst_niquadra_context_get_xcoder_param (thiz->context);
  ni_encoder_cfg_params_t *p_enc_cfg_params = &p_enc_params->cfg_enc_params;
  if (p_enc_cfg_params->spatial_layers > 1) {
    GST_ERROR_OBJECT (thiz,
        "spatialLayers isn't supported for current gst version");
    goto ERR_EXIT;
  }
#endif

  ret = xcoder_send_frame (thiz, frame, &send_frame);
  if (ret != GST_FLOW_OK) {
    goto ERR_EXIT;
  }

  gst_video_codec_frame_unref (frame);

  do {
    ret = gst_niquadraenc_receive_packet (thiz, &got_packet, TRUE);
    if (ret != GST_FLOW_OK)
      break;
  } while (got_packet);

  return ret;

ERR_EXIT:
  {
    GST_ERROR_OBJECT (thiz, "Can't handle error on encoder");
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  };
}

static GstFlowReturn
gst_niquadraenc_flush_buffers (GstNiquadraEnc * nienc, gboolean send)
{
  GstFlowReturn ret = GST_FLOW_OK;
  gboolean send_frame = FALSE;
  gboolean got_packet = FALSE;

  GST_DEBUG_OBJECT (nienc, "flushing buffers with sending %d", send);

  /* no need to empty codec if there is none */
  if (!nienc->started)
    goto done;

  ret = xcoder_send_frame (nienc, NULL, &send_frame);
  if (ret != GST_FLOW_OK)
    goto done;

  do {
    ret = gst_niquadraenc_receive_packet (nienc, &got_packet, send);
    if (ret != GST_FLOW_OK)
      break;
  } while (got_packet);

done:
  return ret;
}

static gboolean
gst_niquadraenc_stop (GstVideoEncoder * encoder)
{
  GstNiquadraEnc *nienc = (GstNiquadraEnc *) encoder;

  gst_niquadraenc_flush_buffers (nienc, FALSE);
  gst_niquadraenc_close (encoder);
  nienc->started = FALSE;

  if (nienc->pending_frames) {
    g_queue_free_full (nienc->pending_frames,
        (GDestroyNotify) gst_video_codec_frame_unref);
    nienc->pending_frames = NULL;
  }

  if (nienc->input_state) {
    gst_video_codec_state_unref (nienc->input_state);
    nienc->input_state = NULL;
  }

  return TRUE;
}

static GstFlowReturn
gst_niquadraenc_finish (GstVideoEncoder * encoder)
{
  GstNiquadraEnc *nienc = (GstNiquadraEnc *) encoder;

  return gst_niquadraenc_flush_buffers (nienc, TRUE);
}

static void
gst_niquadraenc_finalize (GObject * object)
{
  GstNiquadraEnc *nienc = GST_NIQUADRAENC (object);

  if (nienc->input_state) {
    gst_video_codec_state_unref (nienc->input_state);
    nienc->input_state = NULL;
  }

  if (nienc->xcoder_opts) {
    g_free (nienc->xcoder_opts);
    nienc->xcoder_opts = NULL;
  }

  if (nienc->orig_xcoder_opts) {
    g_free (nienc->orig_xcoder_opts);
    nienc->orig_xcoder_opts = NULL;
  }

  if (nienc->device_name) {
    g_free (nienc->device_name);
    nienc->device_name = NULL;
  }

  if (nienc->xcoder_gop) {
    g_free (nienc->xcoder_gop);
    nienc->xcoder_gop = NULL;
  }

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_niquadraenc_sink_query (GstVideoEncoder * enc, GstQuery * query)
{
  GstPad *pad = GST_VIDEO_ENCODER_SINK_PAD (enc);
  gboolean ret = FALSE;
  GstNiquadraEnc *nienc = (GstNiquadraEnc *) enc;
  GstStructure *params = NULL;
  guint buffercnt = 8;          // Default to 8 for adaptive gop
  gint cardno = nienc->dev_enc_idx;

  GST_DEBUG_OBJECT (nienc,
      "Received %s query on sinkpad, %" GST_PTR_FORMAT,
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
    case GST_QUERY_ALLOCATION:{
      /* Propose allocation to upstream element based on xcoder-params */
      if (nienc->orig_xcoder_opts) {
        ni_xcoder_params_t *dummy_params =
            g_malloc0 (sizeof (ni_xcoder_params_t));
        ni_session_context_t *dummy_ctx =
            g_malloc0 (sizeof (ni_session_context_t));
        if (!dummy_params || !dummy_ctx) {
          GST_ERROR_OBJECT (nienc, "Failed allocate params or context\n");
          g_free (dummy_params);
          g_free (dummy_ctx);
          goto fallback;
        }

        if (!gst_niquadraenc_parse_xcoder_params (nienc,
                nienc->orig_xcoder_opts, dummy_params, dummy_ctx)) {

          // If encoder is in low delay mode, suggest to the
          // upstream element to allocate no extra buffers
          if (dummy_params->low_delay_mode == 1)
            buffercnt = 0;

          // If encoder is using a lookahead buffer, suggest to the
          // upstream to allocate the size of the lookahead buffer + 8.
          // lookahead depth > 0 is not compatible with low delay mode.
          if (dummy_params->cfg_enc_params.lookAheadDepth > 0)
            buffercnt = dummy_params->cfg_enc_params.lookAheadDepth + 8;

          // If encoder is in multicore joint mode, add an extra 3 buffers
          if (dummy_params->cfg_enc_params.multicoreJointMode > 0)
            buffercnt += 3;
        }
        g_free (dummy_params);
        g_free (dummy_ctx);
      }

    fallback:
      params = gst_structure_new (NI_PREALLOCATE_STRUCTURE_NAME,
          NI_VIDEO_META_BUFCNT, G_TYPE_UINT, buffercnt, NI_VIDEO_META_CARDNO,
          G_TYPE_INT, cardno, NULL);

      gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, params);
      gst_structure_free (params);

      ret = GST_VIDEO_ENCODER_CLASS (parent_class)->sink_query (enc, query);
    }
      break;

    default:
      ret = GST_VIDEO_ENCODER_CLASS (parent_class)->sink_query (enc, query);
      break;
  }

  return ret;
}

static void
gst_niquadraenc_class_init (GstNiquadraEncClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstVideoEncoderClass *gstencoder_class;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  gstencoder_class = GST_VIDEO_ENCODER_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (gst_niquadraenc_debug, "niquadraenc", 0,
      "niquadraenc");

  gobject_class->finalize = gst_niquadraenc_finalize;

  element_class->set_context = gst_niquadraenc_set_context;

  gstencoder_class->set_format = GST_DEBUG_FUNCPTR (gst_niquadraenc_set_format);
  gstencoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_niquadraenc_handle_frame);
  gstencoder_class->start = GST_DEBUG_FUNCPTR (gst_niquadraenc_start);
  gstencoder_class->stop = GST_DEBUG_FUNCPTR (gst_niquadraenc_stop);
  gstencoder_class->flush = GST_DEBUG_FUNCPTR (gst_niquadraenc_flush);
  gstencoder_class->finish = GST_DEBUG_FUNCPTR (gst_niquadraenc_finish);
  gstencoder_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_niquadraenc_propose_allocation);
  gstencoder_class->getcaps = GST_DEBUG_FUNCPTR (gst_niquadraenc_sink_getcaps);
  gstencoder_class->sink_query = GST_DEBUG_FUNCPTR (gst_niquadraenc_sink_query);
}

static gboolean
gst_niquadraenc_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstVideoEncoder *videoenc = NULL;
  GstVideoEncoderClass *videoencclass = NULL;
  GstNiquadraEnc *nienc = (GstNiquadraEnc *) parent;
  gboolean ret = FALSE;

  videoenc = GST_VIDEO_ENCODER (parent);
  videoencclass = GST_VIDEO_ENCODER_GET_CLASS (videoenc);

  if (GST_EVENT_CAPS == GST_EVENT_TYPE (event)) {
    GstCaps *caps;
    GstCapsFeatures *features;

    gst_event_parse_caps (event, &caps);
    features = gst_caps_get_features (caps, 0);
    if (gst_caps_features_contains (features,
            GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY)) {
      nienc->hardware_mode = true;
    }
  }

  GST_DEBUG_OBJECT (videoenc, "received event %d, %s", GST_EVENT_TYPE (event),
      GST_EVENT_TYPE_NAME (event));

  if (videoencclass->sink_event)
    ret = videoencclass->sink_event (videoenc, event);

  return ret;
}

static void
gst_niquadraenc_init (GstNiquadraEnc * nienc)
{
  GstVideoEncoder *videoenc = NULL;

  videoenc = GST_VIDEO_ENCODER (nienc);

  nienc->keep_alive_timeout = NI_DEFAULT_KEEP_ALIVE_TIMEOUT;
  nienc->dev_enc_idx = PROP_ENC_INDEX;
  nienc->hardware_mode = false;

  gst_pad_set_event_function (videoenc->sinkpad, gst_niquadraenc_sink_event);
  GST_PAD_SET_ACCEPT_TEMPLATE (GST_VIDEO_ENCODER_SINK_PAD (nienc));
}

gboolean
gst_niquadraenc_set_common_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiquadraEnc *nienc = GST_NIQUADRAENC (object);
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
    case GST_NIQUADRA_ENC_PROP_NAME:
      g_free (nienc->device_name);
      nienc->device_name = g_strdup (g_value_get_string (value));
      break;
    case GST_NIQUADRA_ENC_PROP_CARD_NUM:
      nienc->dev_enc_idx = g_value_get_int (value);
      break;
    case GST_NIQUADRA_ENC_PROP_TIMEOUT:
      nienc->keep_alive_timeout = g_value_get_uint (value);
      break;
    case GST_NIQUADRA_ENC_PROP_IO_SIZE:
      nienc->nvme_io_size = g_value_get_int (value);
      break;
    case GST_NIQUADRA_ENC_PROP_XCODER_PARAM:
      g_free (nienc->xcoder_opts);
      g_free (nienc->orig_xcoder_opts);
      nienc->xcoder_opts = g_strdup (g_value_get_string (value));
      nienc->orig_xcoder_opts = g_strdup (g_value_get_string (value));
      break;
    case GST_NIQUADRA_ENC_PROP_XCODER_GOP:
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
gst_niquadraenc_get_common_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiquadraEnc *nienc = GST_NIQUADRAENC (object);
  gboolean ret = TRUE;

  GST_OBJECT_LOCK (nienc);
  switch (prop_id) {
    case GST_NIQUADRA_ENC_PROP_CARD_NUM:
      g_value_set_int (value, nienc->dev_enc_idx);
      break;
    case GST_NIQUADRA_ENC_PROP_NAME:
      g_value_set_string (value, nienc->device_name);
      break;
    case GST_NIQUADRA_ENC_PROP_IO_SIZE:
      g_value_set_int (value, nienc->nvme_io_size);
      break;
    case GST_NIQUADRA_ENC_PROP_TIMEOUT:
      g_value_set_uint (value, nienc->keep_alive_timeout);
      break;
    case GST_NIQUADRA_ENC_PROP_XCODER_PARAM:
      g_value_set_string (value, nienc->xcoder_opts);
      break;
    case GST_NIQUADRA_ENC_PROP_XCODER_GOP:
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
gst_niquadraenc_install_common_properties (GstNiquadraEncClass * enc_class)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (enc_class);
  GParamSpec *obj_properties[GST_NIQUADRA_ENC_PROP_MAX] = { NULL, };

  obj_properties[GST_NIQUADRA_ENC_PROP_CARD_NUM] =
      g_param_spec_int ("enc", "Enc",
      "Select which encoder to use by index. First is 0, second is 1, and so on",
      -1, INT_MAX, PROP_ENC_INDEX, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NIQUADRA_ENC_PROP_NAME] =
      g_param_spec_string ("device-name", "Device-Name",
      "Select which encoder to use by NVMe block device name, e.g. /dev/nvme0n1",
      NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);


  obj_properties[GST_NIQUADRA_ENC_PROP_IO_SIZE] =
      g_param_spec_int ("iosize", "IoSize",
      "Specify a custom NVMe IO transfer size (multiples of 4096 only).", -1,
      INT_MAX, PROP_ENC_IO_SIZE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NIQUADRA_ENC_PROP_XCODER_PARAM] =
      g_param_spec_string ("xcoder-params", "XCODER-PARAMS",
      "Set the XCoder configuration using a :-separated list of key=value parameters",
      NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NIQUADRA_ENC_PROP_XCODER_GOP] =
      g_param_spec_string ("xcoder-gop", "XCODER-GOP",
      "Set the XCoder custom gop using a :-separated list of key=value parameters",
      NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NIQUADRA_ENC_PROP_TIMEOUT] =
      g_param_spec_uint ("keep-alive-timeout", "TIMEOUT",
      "Specify a custom session keep alive timeout in seconds.",
      NI_MIN_KEEP_ALIVE_TIMEOUT, NI_MAX_KEEP_ALIVE_TIMEOUT,
      NI_DEFAULT_KEEP_ALIVE_TIMEOUT,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, GST_NIQUADRA_ENC_PROP_MAX,
      obj_properties);
}
