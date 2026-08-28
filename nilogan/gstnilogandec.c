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
 *  \file   gstnilogandec.c
 *
 *  \brief  Implement of NetInt Logan common decoder.
 ******************************************************************************/

#include <stdlib.h>
#include <ni_av_codec_logan.h>

#include "gstnilogandec.h"
#include "gstnimetadata.h"

GST_DEBUG_CATEGORY_EXTERN (gst_nilogandec_debug);

#define GST_CAT_DEFAULT gst_nilogandec_debug

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

#define COMMON_FORMAT \
  "{ I420, I420_10 }"

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_NI_CAPS_STR (COMMON_FORMAT, COMMON_FORMAT))
    );

#define gst_nilogandec_parent_class parent_class
G_DEFINE_TYPE (GstNiloganDec, gst_nilogandec, GST_TYPE_VIDEO_DECODER);

static void
gst_nilogandec_clear_all_frames (GstNiloganDec * nidec)
{
  GList *l;

  for (l = nidec->frame_list; l;) {
    GstNilogandecVideoFrame *buffer_info = l->data;
    GList *l1 = l;

    l = l->next;

    gst_video_frame_unmap (&buffer_info->vframe);
    if (buffer_info->frame->ref_count > 0) {
      gst_video_decoder_release_frame (GST_VIDEO_DECODER (nidec),
          buffer_info->frame);
      buffer_info->frame = NULL;
    }
    g_slice_free (GstNilogandecVideoFrame, buffer_info);
    nidec->frame_list = g_list_delete_link (nidec->frame_list, l1);
  }
  g_list_free (nidec->frame_list);
  nidec->frame_list = NULL;
}

static void
gst_nilogandec_close_decoder (GstNiloganDec * nidec, gboolean reset_param)
{
  if (!nidec->opened) {
    return;
  }
  ni_logan_session_context_t *p_ctx =
      gst_nilogan_context_get_session_context (nidec->context);
  nidec->opened = FALSE;
  gst_nilogandec_clear_all_frames (nidec);
  GST_DEBUG ("close_decoder\n");
  if (p_ctx) {
    ni_logan_retcode_t ret;
    ret =
        ni_logan_device_session_close (p_ctx, nidec->eos,
        NI_LOGAN_DEVICE_TYPE_DECODER);
    if (ret != NI_LOGAN_RETCODE_SUCCESS) {
      GST_ERROR ("Failed to close Decode Session (status = %d)", ret);
    }
  }
  if (p_ctx) {
    ni_logan_device_close (p_ctx->device_handle);
    ni_logan_device_close (p_ctx->blk_io_handle);
  }
  gst_object_unref (nidec->context);
}

static void
gst_nilogandec_set_context (GstElement * element, GstContext * context)
{
  GST_ELEMENT_CLASS (parent_class)->set_context (element, context);
}

static gboolean
gst_nilogandec_start (GstVideoDecoder * decoder)
{
  GstNiloganDec *thiz = GST_NILOGANDEC (decoder);

  GST_OBJECT_LOCK (decoder);
  gst_nilogandec_close_decoder (thiz, FALSE);
  GST_OBJECT_UNLOCK (decoder);
  return TRUE;
}

static gboolean
gst_nilogandec_close (GstVideoDecoder * decoder)
{
  GstNiloganDec *nienc = GST_NILOGANDEC (decoder);
  gst_nilogandec_close_decoder (nienc, FALSE);

  return TRUE;
}

/*
 * Map to nidec_logan.c "xcoder_logan_setup_decoder"
 * */
static gint
gst_nilogandec_xcoder_setup_decoder (GstNiloganDec * nidec)
{
  GstNiloganDecClass *klass = GST_NILOGANDEC_GET_CLASS (nidec);
  gint ret;
  nidec->context = gst_nilogan_context_new ();
  ni_logan_decoder_params_t *p_param =
      gst_nilogan_context_get_xcoder_dec_param (nidec->context);
  ni_logan_session_context_t *p_session =
      gst_nilogan_context_get_session_context (nidec->context);
  //Todo disable hw frame now
  if (nidec->hardware_mode) {
    return GST_FLOW_ERROR;
  }
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

  nidec->width = nidec->input_state->info.width;
  nidec->height = nidec->input_state->info.height;
  nidec->pix_fmt = nidec->input_state->info.finfo->format;

  if (0 == nidec->input_state->info.width
      || 0 == nidec->input_state->info.height) {
    GST_ERROR_OBJECT (nidec, "Error probing input stream\n");
    return -1;
  }

  GST_DEBUG_OBJECT (nidec, "field_order = %d\n",
      nidec->input_state->info.interlace_mode);
  if (nidec->input_state->info.interlace_mode >
      GST_VIDEO_INTERLACE_MODE_PROGRESSIVE) {
    GST_ERROR_OBJECT (nidec, "interlaced video not supported!\n");
    return -1;
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
  if (depth == 10) {
    p_session->bit_depth_factor = 2;
    p_session->src_bit_depth = 10;
  } else {
    p_session->bit_depth_factor = 1;
    p_session->src_bit_depth = 8;
  }

  //Xcoder User Configuration
  if (ni_logan_decoder_init_default_params (p_param,
          nidec->input_state->info.fps_n, nidec->input_state->info.fps_d,
          nidec->input_state->info.finfo->bits, nidec->input_state->info.width,
          nidec->input_state->info.height) < 0) {

    GST_DEBUG_OBJECT (nidec, "Error setting params\n");

    return -1;
  }
  // hwframes can be set from 'out=hw' or 'hwframes 1' param
  p_param->dec_input_params.hwframes =
      nidec->hardware_mode | p_param->dec_input_params.hwframes;

  if (p_param->dec_input_params.hwframes) {     //need to set before open decoder
    p_session->hw_action = NI_LOGAN_CODEC_HW_ENABLE;
    nidec->hardware_mode = TRUE;
  } else {
    p_session->hw_action = NI_LOGAN_CODEC_HW_NONE;
    nidec->hardware_mode = FALSE;
  }

  // overwrite set_high_priority value here with a custom value if it was provided
  if (p_param->dec_input_params.set_high_priority != 0) {
    p_session->set_high_priority = p_param->dec_input_params.set_high_priority;
  } else {
    p_session->set_high_priority = nidec->set_high_priority;
  }
  GST_DEBUG ("Custom NVMe set_high_priority set to = %d\n",
      p_session->set_high_priority);

  // overwrite lowdelay value here with a custom value if it was provided
  if (p_param->dec_input_params.lowdelay != 0) {
    nidec->low_delay = p_param->dec_input_params.lowdelay;
  } else {
    p_param->dec_input_params.lowdelay = nidec->low_delay;
  }
  p_session->decoder_low_delay = nidec->low_delay;

  if (nidec->input_state->info.width >= 7000) {
    p_session->decoder_low_delay = 1;
    p_param->dec_input_params.lowdelay = 1;
    nidec->low_delay = 1;
  }
  nidec->started = 0;
  ni_logan_session_data_io_t *api_pkt =
      gst_nilogan_context_get_data_pkt (nidec->context);
  memset (api_pkt, 0, sizeof (ni_logan_packet_t));
  nidec->pkt_nal_bitmap = 0;

  // overwrite keep alive timeout value here with a custom value if it was
  // provided
  // if xcoder option is set then overwrite the (legacy) decoder option
  uint32_t xcoder_timeout = p_param->dec_input_params.keep_alive_timeout;
  if (xcoder_timeout != NI_LOGAN_DEFAULT_KEEP_ALIVE_TIMEOUT) {
    p_session->keep_alive_timeout = xcoder_timeout;
  } else {
    p_session->keep_alive_timeout = nidec->keep_alive_timeout;
  }
  GST_DEBUG_OBJECT (nidec, "Custom NVME Keep Alive Timeout set to %d\n",
      p_session->keep_alive_timeout);
  p_session->hw_id = nidec->dev_dec_idx;
  p_session->decoder_low_delay = nidec->low_delay =
      p_param->dec_input_params.lowdelay;
  strcpy (p_session->dev_xcoder, nidec->dev_xcoder);

  p_session->p_session_config = p_param;

  p_session->session_id = NI_LOGAN_INVALID_SESSION_ID;

  // assign the card GUID in the encoder context and let session open
  // take care of the rest
  p_session->device_handle = NI_INVALID_DEVICE_HANDLE;
  p_session->blk_io_handle = NI_INVALID_DEVICE_HANDLE;
  p_session->codec_format = nidec->codec_format;

  ret = ni_logan_device_session_open (p_session, NI_LOGAN_DEVICE_TYPE_DECODER);

  if (ret != 0) {
    GST_ERROR_OBJECT (nidec, "Failed to open decoder (status = %d)\n", ret);
  } else {
    nidec->dev_xcoder_name = p_session->dev_xcoder_name;
    nidec->blk_xcoder_name = p_session->blk_xcoder_name;
    nidec->dev_dec_idx = p_session->hw_id;
    GST_DEBUG ("XCoder %s.%d (inst: %d) %p opened successfully\n",
        nidec->dev_xcoder_name, nidec->dev_dec_idx, p_session->session_id,
        p_session);
  }
  nidec->current_pts = 0;
  nidec->opened = TRUE;

  return ret;
}

static gboolean
gst_nilogandec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state)
{
  GstNiloganDec *nidec = GST_NILOGANDEC (decoder);
  gint ret;

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
    GST_DEBUG ("set format, sequence change\n");
    return TRUE;
  }

  ret = gst_nilogandec_xcoder_setup_decoder (nidec);

  if (ret != 0) {
    GST_ERROR_OBJECT (nidec, "XCoder init failure[%d]\n", ret);
    return FALSE;
  }

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

/*
 * Map to nidec_logan.c "xcoder_logan_decode_reset".
 * */
static gint
gst_nilogandec_reset (GstNiloganDec * nidec)
{
  ni_logan_retcode_t ret = NI_LOGAN_RETCODE_FAILURE;
  ni_logan_session_context_t *p_session =
      gst_nilogan_context_get_session_context (nidec->context);
  ni_logan_session_data_io_t *p_pkt =
      gst_nilogan_context_get_data_pkt (nidec->context);
  GST_DEBUG ("XCode decode reset\n");

  ni_logan_device_session_close (p_session, nidec->eos,
      NI_LOGAN_DEVICE_TYPE_DECODER);

  ni_logan_device_close (p_session->device_handle);
  ni_logan_device_close (p_session->blk_io_handle);
  p_session->device_handle = NI_INVALID_DEVICE_HANDLE;
  p_session->blk_io_handle = NI_INVALID_DEVICE_HANDLE;

  ni_logan_packet_buffer_free (&(p_pkt->data.packet));
  gint64 bcp_current_pts = nidec->current_pts;
  ret = gst_nilogandec_xcoder_setup_decoder (nidec);
  nidec->current_pts = bcp_current_pts;
  p_session->session_run_state = LOGAN_SESSION_RUN_STATE_RESETTING;
  return ret;
}

static GstNilogandecVideoFrame *
gst_nilogandec_video_frame_new (GstNiloganDec * nidec,
    GstVideoCodecFrame * frame)
{
  GstNilogandecVideoFrame *dframe;

  dframe = g_slice_new0 (GstNilogandecVideoFrame);
  dframe->frame = frame;
  dframe->buffer = frame->input_buffer;

  return dframe;
}

static void
gst_nilogandec_video_frame_free (GstNiloganDec * nidec,
    GstNilogandecVideoFrame * frame)
{
  GList *l;

  for (l = nidec->frame_list; l;) {
    GstNilogandecVideoFrame *buffer_info = l->data;
    GList *l1 = l;

    l = l->next;

    if (buffer_info != frame)
      continue;

    gst_video_frame_unmap (&frame->vframe);
    if (frame->frame) {
      gst_video_decoder_release_frame (GST_VIDEO_DECODER (nidec), frame->frame);
    }
    //gst_video_decoder_release_frame (GST_VIDEO_DECODER (nidec), frame->frame);
    g_slice_free (GstNilogandecVideoFrame, frame);

    nidec->frame_list = g_list_delete_link (nidec->frame_list, l1);
    return;
  }
}

/*
 * Map to nicodec_logan.c "ff_xcoder_logan_dec_send"
 * */
static int
gst_nilogandec_send_frame (GstNiloganDec * decoder, GstMapInfo * info,
    GstBuffer * input_buffer)
{
  /* call ni_decoder_session_write to send compressed video packet to the decoder
     instance */
  int need_draining = 0;
  size_t size;
  ni_logan_session_data_io_t *api_pkt =
      gst_nilogan_context_get_data_pkt (decoder->context);
  ni_logan_session_context_t *p_session =
      gst_nilogan_context_get_session_context (decoder->context);
  ni_logan_packet_t *xpkt = &(api_pkt->data.packet);
  int ret;
  int sent;
  int send_size = 0;
  int new_packet = 0;
  int extra_prev_size = 0;

  size = info->size;

  if (decoder->flushing) {
    GST_ERROR_OBJECT (decoder, "Decoder is flushing and cannot accept new"
        "buffer until all output buffer have been released\n");
    return GST_FLOW_CUSTOM_ERROR_2;
  }

  if (info->size == 0) {
    need_draining = 1;
  }

  if (decoder->draining && decoder->eos) {
    GST_DEBUG_OBJECT (decoder, "Decoder is draining, eos\n");
    return GST_FLOW_EOS;
  }

  if (xpkt->data_len == 0) {
    memset (xpkt, 0, sizeof (ni_logan_packet_t));
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
    xpkt->p_all_custom_sei = NULL;
    xpkt->len_of_sei_after_vcl = 0;

    GST_DEBUG ("size=%ld,prev_size=%d", info->size, p_session->prev_size);

    if (decoder->low_delay && decoder->got_first_key_frame &&
        !(decoder->pkt_nal_bitmap & NI_LOGAN_GENERATE_ALL_NAL_HEADER_BIT)) {
      p_session->decoder_low_delay = decoder->low_delay;
      decoder->pkt_nal_bitmap |= NI_LOGAN_GENERATE_ALL_NAL_HEADER_BIT;
    }


    if (info->size + p_session->prev_size > 0) {
      ni_logan_packet_buffer_alloc (xpkt, (info->size + p_session->prev_size));
      if (!xpkt->p_data) {
        ret = GST_FLOW_ERROR;
        goto fail;
      }
    }
    new_packet = 1;
  } else {
    send_size = xpkt->data_len;
  }

  GST_DEBUG ("xcoder_dec_send: pkt->size=%ld\n", info->size);

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
          ni_logan_packet_copy (xpkt->p_data, info->data, info->size,
          p_session->p_leftover, &p_session->prev_size);
      // increment offset of data sent to decoder and save it
      xpkt->pos = (long long) decoder->offset;
      decoder->offset += info->size + extra_prev_size;
    }
    GST_DEBUG_OBJECT (decoder,
        "ni_packet_copy after: size=%ld, s->prev_size=%d, send_size=%d, xpkt->data_len=%u (end of stream)\n",
        info->size, p_session->prev_size, send_size, xpkt->data_len);

    if (send_size < 0) {
      GST_DEBUG_OBJECT (decoder, "Failed to copy pkt (status = "
          "%d)\n", send_size);
      ret = -1;
      goto fail;
    }
    xpkt->data_len += extra_prev_size;

    sent = 0;
    if (xpkt->data_len > 0) {
      sent =
          ni_logan_device_session_write (p_session, api_pkt,
          NI_LOGAN_DEVICE_TYPE_DECODER);
    }
    if (sent < 0) {
      GST_ERROR ("Failed to send eos signal (status = %d)\n", sent);
      if (NI_LOGAN_RETCODE_ERROR_VPU_RECOVERY == sent) {
        ret = gst_nilogandec_reset (decoder);
        if (0 == ret) {
          ret = GST_FLOW_CUSTOM_ERROR;
        }
      } else {
        ret = GST_FLOW_ERROR;
      }
      goto fail;
    }
    GST_DEBUG_OBJECT (decoder, "Queued eos (status = %d) ts=%lld\n",
        sent, xpkt->pts);
    decoder->draining = 1;

    ni_logan_device_session_flush (p_session, NI_LOGAN_DEVICE_TYPE_DECODER);
  } else {
    GST_DEBUG
        ("ni_packet_copy before: size=%ld, s->prev_size=%d, send_size=%d\n",
        info->size, p_session->prev_size, send_size);
    if (new_packet) {
      extra_prev_size = p_session->prev_size;
      send_size =
          ni_logan_packet_copy (xpkt->p_data, info->data, info->size,
          p_session->p_leftover, &p_session->prev_size);
      // increment offset of data sent to decoder and save it
      xpkt->pos = (long long) decoder->offset;
      decoder->offset += info->size + extra_prev_size;
    }
    GST_DEBUG
        ("ni_packet_copy after: size=%ld, s->prev_size=%d, send_size=%d, xpkt->data_len=%u\n",
        info->size, p_session->prev_size, send_size, xpkt->data_len);

    if (send_size < 0) {
      GST_ERROR ("Failed to copy pkt (status = " "%d)\n", send_size);
      ret = -1;
      goto fail;
    }

    xpkt->data_len += extra_prev_size;

    sent = 0;
    if (xpkt->data_len > 0) {
      sent =
          ni_logan_device_session_write (p_session, api_pkt,
          NI_LOGAN_DEVICE_TYPE_DECODER);
      GST_DEBUG ("ff_xcoder_dec_send pts=%" PRIi64 ", dts=%" PRIi64 ", pos=%"
          PRIi64 ", sent=%d\n", input_buffer->pts, input_buffer->dts,
          input_buffer->offset, sent);
    }
    if (sent < 0) {
      GST_ERROR ("Failed to send compressed pkt (status = " "%d)\n", sent);
      if (NI_LOGAN_RETCODE_ERROR_VPU_RECOVERY == sent) {
        ret = gst_nilogandec_reset (decoder);
        if (0 == ret) {
          ret = GST_FLOW_CUSTOM_ERROR;
        }
      } else {
        ret = GST_FLOW_ERROR;
      }
      goto fail;
    } else if (sent == 0) {
      GST_DEBUG_OBJECT (decoder, "Queued input buffer size=0\n");
    } else if (sent < size) {   /* partial sent; keep trying */
      GST_DEBUG ("Queued input buffer size=%d\n", sent);
    }
  }

  if (xpkt->data_len == 0) {
    /* if this packet is done sending, free any sei buffer. */
    free (xpkt->p_all_custom_sei);
    xpkt->p_all_custom_sei = NULL;
  }

  if (sent != 0) {
    //keep the current pkt to resend next time
    ni_logan_packet_buffer_free (xpkt);
    return sent;
  } else {
    return GST_FLOW_CUSTOM_ERROR_1;
  }


fail:
  ni_logan_packet_buffer_free (xpkt);
  free (xpkt->p_all_custom_sei);
  xpkt->p_all_custom_sei = NULL;
  decoder->draining = 1;
  decoder->eos = 1;

  return ret;
}

/*
 * Map to nicodec_logan.c "ff_xcoder_logan_dec_receive"
 * */
static int
gst_nilogandec_recv_frame (GstNiloganDec * decoder,
    ni_logan_session_data_io_t * p_ni_frame, GstVideoCodecFrame * frame)
{
  /* call xcode_dec_receive to get a decoded YUV frame from the decoder
     instance */
  ni_logan_session_context_t *p_session =
      gst_nilogan_context_get_session_context (decoder->context);
  int ret = 0;
  int cropped_width, cropped_height;

  if (decoder->draining && decoder->eos) {
    return GST_FLOW_EOS;
  }

  if (decoder->hardware_mode) {
    ret = ni_logan_device_session_read_hwdesc (p_session, p_ni_frame);
  } else {
    ret =
        ni_logan_device_session_read (p_session, p_ni_frame,
        NI_LOGAN_DEVICE_TYPE_DECODER);
  }

  if (ret == 0) {
    decoder->eos = p_ni_frame->data.frame.end_of_stream;
    if (decoder->hardware_mode) {
      ni_logan_frame_buffer_free (&(p_ni_frame->data.frame));
    } else {
      ni_logan_decoder_frame_buffer_free (&(p_ni_frame->data.frame));
    }
    return GST_FLOW_CUSTOM_ERROR_1;
  } else if (ret > 0) {
    if (p_ni_frame->data.frame.flags & 0x0004) {
      GST_DEBUG ("Current frame is dropped when AV_PKT_FLAG_DISCARD is set\n");
      if (decoder->hardware_mode) {
        ni_logan_frame_buffer_free (&(p_ni_frame->data.frame));
      } else {
        ni_logan_decoder_frame_buffer_free (&(p_ni_frame->data.frame));
      }
      return GST_FLOW_CUSTOM_ERROR_1;
    }

    GST_DEBUG ("Got output buffer pts=%lld "
        "dts=%lld eos=%d sos=%d\n",
        p_ni_frame->data.frame.pts, p_ni_frame->data.frame.dts,
        p_ni_frame->data.frame.end_of_stream,
        p_ni_frame->data.frame.start_of_stream);

    decoder->eos = p_ni_frame->data.frame.end_of_stream;

    int crop_top = p_ni_frame->data.frame.crop_top;
    int crop_bottom =
        p_session->active_video_height - p_ni_frame->data.frame.crop_bottom;
    int crop_left = p_ni_frame->data.frame.crop_left;
    int crop_right =
        p_session->active_video_width - p_ni_frame->data.frame.crop_right;

    cropped_width = p_session->active_video_width - crop_left - crop_right;
    cropped_height = p_session->active_video_height - crop_top - crop_bottom;

    if (cropped_width != decoder->width || cropped_height != decoder->height) {
      GST_DEBUG ("Decoder sequence change %dx%d -----> %dx%d ", decoder->width,
          decoder->height, cropped_width, cropped_height);
      decoder->width = cropped_width;
      decoder->height = cropped_height;
      decoder->configured = false;
    }

    GST_DEBUG ("ff_xcoder_dec_receive: frame->pts=%lld"
        ", frame->pkt_dts=%lld\n", p_ni_frame->data.frame.pts,
        p_ni_frame->data.frame.dts);

    if (decoder->hardware_mode) {
      ni_logan_device_session_copy (p_session, &decoder->hw_api_ctx);
    }
  } else {
    GST_DEBUG ("Failed to get output buffer (status = %d)\n", ret);

    if (NI_LOGAN_RETCODE_ERROR_VPU_RECOVERY == ret) {
      GST_DEBUG ("ff_xcoder_dec_receive VPU recovery, need to reset ..\n");
      if (decoder->hardware_mode) {
        ni_logan_frame_buffer_free (&(p_ni_frame->data.frame));
      } else {
        ni_logan_decoder_frame_buffer_free (&(p_ni_frame->data.frame));
      }
      return ret;
    }

    return GST_FLOW_ERROR;
  }

  ret = 0;

  return ret;
}

static gint
gst_nilogandec_alloc_frame (GstNiloganDec * decoder,
    ni_logan_session_data_io_t * p_ni_frame)
{
  ni_logan_session_context_t *p_session =
      gst_nilogan_context_get_session_context (decoder->context);
  int ret = 0;
  int alloc_mem, height, width;

  memset (p_ni_frame, 0, sizeof (ni_logan_session_data_io_t));

  height =
      (int) (p_session->active_video_height > 0 ? p_session->active_video_height
      : decoder->input_state->info.height);
  width =
      (int) (p_session->active_video_width > 0 ? p_session->active_video_width
      : decoder->input_state->info.width);

  GST_DEBUG ("active=%dx%d, info=%dx%d\n", p_session->active_video_width,
      p_session->active_video_height, decoder->input_state->info.width,
      decoder->input_state->info.height);

  // allocate memory only after resolution is known (buffer pool set up)
  alloc_mem = (p_session->active_video_width > 0 &&
      p_session->active_video_height > 0 ? 1 : 0);

  if (decoder->hardware_mode) {
    ret = ni_logan_frame_buffer_alloc (&(p_ni_frame->data.frame), width,
        height, (p_session->codec_format == NI_LOGAN_CODEC_FORMAT_H264),
        1, p_session->bit_depth_factor, 1);
  } else {
    ret =
        ni_logan_decoder_frame_buffer_alloc (p_session->dec_fme_buf_pool,
        &(p_ni_frame->data.frame), alloc_mem, width,
        height,
        (p_session->codec_format == NI_LOGAN_CODEC_FORMAT_H264),
        p_session->bit_depth_factor);
  }

  return ret;
}

/*
 * Map to nicodec_logan.c "retrieve_logan_frame"
 * */
static gboolean
retrieve_frame (GstNiloganDec * decoder,
    ni_logan_session_data_io_t * p_ni_frame, GstVideoCodecFrame * out_frame)
{
  ni_logan_frame_t *xfme = &p_ni_frame->data.frame;
  ni_aux_data_t *aux_data = NULL;
  out_frame->output_buffer->pts = p_ni_frame->data.frame.pts;
  out_frame->output_buffer->dts = p_ni_frame->data.frame.dts;
  out_frame->pts = p_ni_frame->data.frame.pts;
  out_frame->dts = p_ni_frame->data.frame.dts;
  ni_logan_dec_retrieve_aux_data (xfme);
  if (xfme->sei_user_data_unreg_offset) {
    if ((aux_data =
            ni_logan_frame_get_aux_data (xfme, NI_FRAME_AUX_DATA_UDU_SEI))) {
      GST_DEBUG ("Found UDU data size %d", aux_data->size);

      /* do not add UDU if it already exists */
      if (!gst_buffer_get_meta (out_frame->input_buffer,
              GST_VIDEO_UDU_META_API_TYPE)) {
        out_frame->output_buffer =
            gst_buffer_make_writable (out_frame->output_buffer);
        gst_buffer_add_video_user_data_unregistered_meta
            (out_frame->output_buffer, aux_data->data, aux_data->size);
      } else {
        GST_DEBUG ("UDU already exists: will not add new udu meta");
      }
    }
  }
  // closed caption
  if ((aux_data =
          ni_logan_frame_get_aux_data (&p_ni_frame->data.frame,
              NI_FRAME_AUX_DATA_A53_CC))
      && p_ni_frame->data.frame.sei_alt_transfer_characteristics_len > 0) {
    if (aux_data) {
      if (!gst_buffer_get_meta (out_frame->input_buffer,
              GST_VIDEO_CAPTION_META_API_TYPE)) {
        out_frame->output_buffer =
            gst_buffer_make_writable (out_frame->output_buffer);
        gst_buffer_add_video_caption_meta (out_frame->output_buffer,
            GST_VIDEO_CAPTION_TYPE_CEA708_RAW, aux_data->data, aux_data->size);
      } else {
        GST_LOG_OBJECT (decoder,
            "Closed caption meta already exists: will not add new caption meta");
      }
    }
  }
  // hdr10+
  if ((aux_data =
          ni_logan_frame_get_aux_data (&p_ni_frame->data.frame,
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
  ni_logan_frame_wipe_aux_data (xfme);
  return TRUE;
}

static gboolean
gst_nilogandec_negotiate (GstNiloganDec * decoder,
    ni_logan_session_data_io_t * p_ni_frame, GstVideoCodecFrame * out_frame)
{
  GstVideoFormat fmt;
  GstVideoInfo *in_info, *out_info;
  GstVideoCodecState *output_state;
  gint fps_n = 0, fps_d = 0;
  gint in_fps_n = 0, in_fps_d = 0;
  GstStructure *in_s;
  const gchar *s;
  ni_logan_session_context_t *p_ctx =
      gst_nilogan_context_get_session_context (decoder->context);

  if (p_ctx->bit_depth_factor == 1) {
    fmt = GST_VIDEO_FORMAT_I420;
  } else {
    fmt = GST_VIDEO_FORMAT_I420_10LE;
  }

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
    out_info->colorimetry.primaries =
        gst_video_color_primaries_from_iso
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

  /* try to find a good framerate */
  if ((in_info->fps_d && in_info->fps_n) ||
      GST_VIDEO_INFO_FLAG_IS_SET (in_info, GST_VIDEO_FLAG_VARIABLE_FPS)) {
    /* take framerate from input when it was specified (#313970) */
    fps_n = in_info->fps_n;
    fps_d = in_info->fps_d;
  }
  if (gst_structure_has_field (in_s, "framerate")) {
    gst_structure_get_fraction (in_s, "framerate", &in_fps_n, &in_fps_d);
  }
  if (in_fps_d != 0 && in_fps_n != 0) {
    out_info->fps_n = in_fps_n;
    out_info->fps_d = in_fps_d;
  } else if (in_fps_d == 0 || in_fps_n == 0) {
    out_info->fps_n = 30;
    out_info->fps_d = 1;
  } else {
    out_info->fps_n = fps_n;
    out_info->fps_d = fps_d;
  }

  GST_LOG_OBJECT (decoder, "setting framerate: %d/%d", out_info->fps_n,
      out_info->fps_d);

  /* To passing HDR information to caps directly */
  if (output_state->caps == NULL) {
    output_state->caps = gst_video_info_to_caps (out_info);
  } else {
    output_state->caps = gst_caps_make_writable (output_state->caps);
  }

  if (!gst_structure_has_field (in_s, "mastering-display-info")) {
    // retrieve side data if available
    ni_aux_data_t *aux_data = NULL;
    ni_logan_dec_retrieve_aux_data (&p_ni_frame->data.frame);
    GstVideoMasteringDisplayInfo minfo;

    // master_display_color
    if ((aux_data =
            ni_logan_frame_get_aux_data (&p_ni_frame->data.frame,
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
            ni_logan_frame_get_aux_data (&p_ni_frame->data.frame,
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
    if ((aux_data =
            ni_logan_frame_get_aux_data (&p_ni_frame->data.frame,
                NI_FRAME_AUX_DATA_A53_CC))
        && p_ni_frame->data.frame.sei_alt_transfer_characteristics_len > 0) {
      if (aux_data) {
        if (!gst_buffer_get_meta (out_frame->input_buffer,
                GST_VIDEO_CAPTION_META_API_TYPE)) {
          out_frame->output_buffer =
              gst_buffer_make_writable (out_frame->output_buffer);
          gst_buffer_add_video_caption_meta (out_frame->output_buffer,
              GST_VIDEO_CAPTION_TYPE_CEA708_RAW, aux_data->data,
              aux_data->size);
        } else {
          GST_LOG_OBJECT (decoder,
              "Closed caption meta already exists: will not add new caption meta");
        }
      }
    }

    ni_logan_frame_wipe_aux_data (&p_ni_frame->data.frame);
  }
  if (decoder->hardware_mode) {
    gst_caps_set_simple (output_state->caps, "hw_pix_fmt", G_TYPE_INT,
        PIX_FMT_NI_LOGAN, NULL);
  }
  if (!gst_video_decoder_negotiate (GST_VIDEO_DECODER (decoder))) {
    GST_ERROR_OBJECT (decoder, "Failed to negotiate");
    return FALSE;
  }

  return TRUE;
}

static GstNilogandecVideoFrame *
gst_nilogandec_find_best_frame (GstNiloganDec * dec, GList * frames,
    GstClockTime dts)
{
  GList *iter;
  GstNilogandecVideoFrame *ret = NULL;
  for (iter = frames; iter; iter = g_list_next (iter)) {
    GstNilogandecVideoFrame *frame = (GstNilogandecVideoFrame *) iter->data;

    if (frame->frame->dts == dts) {
      ret = frame;
      GST_DEBUG ("Found pts=%lu,sys=%d", frame->frame->dts,
          frame->frame->system_frame_number);
      break;
    }
  }

  return ret;
}


static gboolean
gst_nilogandec_video_frame (GstNiloganDec * decoder,
    GstVideoCodecFrame * frame, GstFlowReturn * ret)
{
  gint res;
  gboolean got_frame = FALSE;
  GstVideoInfo *output_info;
  GstVideoCodecFrame *out_frame = NULL;
  GstNilogandecVideoFrame *dframe;
  ni_logan_session_context_t *p_ctx =
      gst_nilogan_context_get_session_context (decoder->context);

  *ret = GST_FLOW_OK;

  ni_logan_session_data_io_t ni_frame;
  res = gst_nilogandec_alloc_frame (decoder, &ni_frame);
  if (res < 0) {
    GST_ERROR_OBJECT (decoder, "alloc frame error\n");
    goto no_output;
  }

  res = gst_nilogandec_recv_frame (decoder, &ni_frame, frame);

  /* No frames available at this time */
  if (res == GST_FLOW_CUSTOM_ERROR_1)   // EAGAIN
    goto beach;
  else if (res == GST_FLOW_EOS) {
    *ret = GST_FLOW_EOS;
    GST_DEBUG_OBJECT (decoder, "Context was entirely flushed");
    goto beach;
  } else if (res < 0) {
    *ret = GST_FLOW_OK;
    GST_WARNING_OBJECT (decoder, "Legitimate decoding error");
    goto beach;
  }

  got_frame = TRUE;

  dframe =
      gst_nilogandec_find_best_frame (decoder, decoder->frame_list,
      ni_frame.data.frame.dts);

  if (!dframe) {
    GST_DEBUG ("Not found");
    dframe = g_list_first (decoder->frame_list)->data;
  }

  out_frame = gst_video_codec_frame_ref (dframe->frame);

  if (!gst_nilogandec_negotiate (decoder, &ni_frame, out_frame))
    goto no_output;

  *ret = gst_video_decoder_allocate_output_frame (GST_VIDEO_DECODER (decoder),
      out_frame);

  output_info = &decoder->output_state->info;

  GST_DEBUG ("Decoding pts=%ld,dts=%ld,sys_frame=%d", out_frame->pts,
      out_frame->dts, out_frame->system_frame_number);

  retrieve_frame (decoder, &ni_frame, out_frame);

  if (!decoder->hardware_mode) {
    if (!gst_video_frame_map (&dframe->vframe, output_info,
            out_frame->output_buffer, GST_MAP_READ | GST_MAP_WRITE))
      GST_ERROR_OBJECT (decoder, "video frame map failed\n");
    int i, j;
    uint8_t *src;
    uint8_t *dst;
    for (i = 0; i < 3; i++) {
      src = ni_frame.data.frame.p_data[i];

      int plane_height = p_ctx->active_video_height;
      int plane_width = p_ctx->active_video_width;
      int write_height = GST_VIDEO_FRAME_HEIGHT (&dframe->vframe);
      int write_width = GST_VIDEO_FRAME_WIDTH (&dframe->vframe);

      GST_DEBUG ("wxh=%dx%d, wxh=%dx%d, bit_depth=%d\n", plane_height,
          plane_width, write_width, write_height, p_ctx->bit_depth_factor);

      // support for 8/10 bit depth
      // plane_width is the actual Y stride size

      if (i == 1 || i == 2) {
        plane_height /= 2;
        plane_width /= 2;
        write_height /= 2;
        write_width /= 2;
      }

      write_width *= p_ctx->bit_depth_factor;
      plane_width *= p_ctx->bit_depth_factor;

      dst = GST_VIDEO_FRAME_PLANE_DATA (&dframe->vframe, i);
      // apply the cropping windown in writing out the YUV frame
      // for now the windown is usually crop-left = crop-top = 0, and we
      // use this to simplify the cropping logic
      for (j = 0; j < plane_height; j++) {
        if (j < write_height) {
          memcpy (dst, src, GST_VIDEO_FRAME_PLANE_STRIDE (&dframe->vframe, i));
          dst += GST_VIDEO_FRAME_PLANE_STRIDE (&dframe->vframe, i);
        }
        src += plane_width;
      }
    }
    //gst_video_frame_unmap(&dframe->vframe);
  } else {
    // hwframe todo
//        if (p_ctx->frame_num == 1) {
//            ni_logan_device_session_copy (p_ctx, &decoder->hw_api_ctx);
//        }
//
//        niFrameSurface1_t *p_data3;
//        p_data3 =
//                (niFrameSurface1_t *) (ni_frame.data.frame.p_buffer +
//                                       ni_frame.data.frame.data_len[0] + ni_frame.data.frame.data_len[1] +
//                                       ni_frame.data.frame.data_len[2]);
//
//        GstNiFrameContext *hwFrame =
//                gst_ni_hw_frame_context_new (&decoder->hw_api_ctx, p_data3,
//                                             decoder->dev_dec_idx);
//
//        GST_DEBUG
//        ("retrieve_frame: OUT0 data[3] trace ui16FrameIdx = [%d], device_handle=%d bitdep=%d, WxH %d x %d\n",
//         p_data3->ui16FrameIdx, p_data3->device_handle, p_data3->bit_depth,
//         p_data3->ui16width, p_data3->ui16height);
//        gst_buffer_add_ni_hwframe_meta (out_frame->output_buffer, hwFrame);
//        gst_ni_hw_frame_context_unref (hwFrame);
  }

  *ret =
      gst_video_decoder_finish_frame (GST_VIDEO_DECODER (decoder), out_frame);

  if (!decoder->hardware_mode) {
    ni_logan_decoder_frame_buffer_free (&(ni_frame.data.frame));
  }

  gst_nilogandec_video_frame_free (decoder, dframe);

  if (G_UNLIKELY (*ret != GST_FLOW_OK))
    goto no_output;
beach:
  GST_DEBUG_OBJECT (decoder, "return flow %s, got frame: %d",
      gst_flow_get_name (*ret), got_frame);
  if (decoder->hardware_mode) {
    ni_logan_frame_buffer_free (&(ni_frame.data.frame));
  } else {
    ni_logan_decoder_frame_buffer_free (&(ni_frame.data.frame));
  }

  return got_frame;

  /* special cases */
no_output:
  {
    GST_DEBUG_OBJECT (decoder, "no output buffer");
    if (out_frame->ref_count > 0) {
      gst_video_decoder_drop_frame (GST_VIDEO_DECODER (decoder), out_frame);
    }
    goto beach;
  }
}

static gboolean
gst_nilogandec_frame (GstNiloganDec * decoder, GstVideoCodecFrame * frame,
    GstFlowReturn * ret)
{
  gboolean got_frame = FALSE;

  *ret = GST_FLOW_OK;
  decoder->frame_number++;

  got_frame = gst_nilogandec_video_frame (decoder, frame, ret);

  return got_frame;
}

static GstFlowReturn
gst_nilogandec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  GstNiloganDec *thiz = GST_NILOGANDEC (decoder);
  GstFlowReturn ret = GST_FLOW_OK;
  gboolean got_frame;
  GstBuffer *input_buffer = NULL;
  GstNilogandecVideoFrame *dframe;
  GstMapInfo map_info;

  // 1. Receive pkt data from upstream plugin.
  GST_DEBUG ("Received new data of size %lu, dts %" GST_TIME_FORMAT ", pts:%"
      GST_TIME_FORMAT ",buffer pts:%" GST_TIME_FORMAT ", dur:%" GST_TIME_FORMAT
      "", gst_buffer_get_size (frame->input_buffer), GST_TIME_ARGS (frame->dts),
      GST_TIME_ARGS (frame->pts), GST_TIME_ARGS (frame->input_buffer->pts),
      GST_TIME_ARGS (frame->duration));
  // Map upstream buffer to map_info
  input_buffer = gst_buffer_ref (frame->input_buffer);
  if (!gst_buffer_map (input_buffer, &map_info, GST_MAP_READ)) {
    gst_buffer_unref (input_buffer);
    goto error;
  }


  GST_DEBUG ("Handle_frame, pts=%lu,dts=%lu, sys=%d, dec=%d",
      frame->input_buffer->pts, frame->input_buffer->dts,
      frame->system_frame_number, frame->decode_frame_number);

  /* treat frame as void until a buffer is requested for it */
  GST_VIDEO_CODEC_FRAME_FLAG_SET (frame,
      GST_VIDEO_CODEC_FRAME_FLAG_DECODE_ONLY);

  GST_VIDEO_DECODER_STREAM_UNLOCK (thiz);
  // 2. send frame.
  do {
    ret = gst_nilogandec_send_frame (thiz, &map_info, input_buffer);
    if (ret < 0 && ret != GST_FLOW_CUSTOM_ERROR_1) {
      GST_DEBUG ("nilogandec send frame error, ret=%d\n", ret);
      ret = GST_FLOW_ERROR;
      GST_VIDEO_DECODER_STREAM_LOCK (thiz);
      goto error;
    }
  } while (ret == GST_FLOW_CUSTOM_ERROR_1);
  GST_VIDEO_DECODER_STREAM_LOCK (thiz);


  GST_DEBUG_OBJECT (decoder, "Decoding, sys_frame=%d\n",
      frame->system_frame_number);

  GST_VIDEO_CODEC_FRAME_FLAG_UNSET (frame,
      GST_VIDEO_CODEC_FRAME_FLAG_DECODE_ONLY);

  // Use new function to new dframe.
  dframe = gst_nilogandec_video_frame_new (thiz, frame);

  thiz->frame_list = g_list_append (thiz->frame_list, dframe);

  do {
    got_frame = gst_nilogandec_frame (thiz, frame, &ret);

    if (ret != GST_FLOW_OK) {
      GST_LOG_OBJECT (thiz, "breaking because of flow ret %s",
          gst_flow_get_name (ret));
      break;
    }
  } while (got_frame);

  gst_buffer_unmap (input_buffer, &map_info);
  gst_buffer_unref (input_buffer);
  return ret;

error:
  if (input_buffer) {
    gst_buffer_unmap (input_buffer, &map_info);
    gst_buffer_unref (input_buffer);
  }
  return ret;
}

static void
gst_nilogandec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiloganDec *thiz = GST_NILOGANDEC (object);
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

  /* ERROR */
wrong_state:
  {
    GST_WARNING_OBJECT (thiz, "setting property in wrong state");
    GST_OBJECT_UNLOCK (thiz);
  }
}

static void
gst_nilogandec_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstNiloganDec *thiz = GST_NILOGANDEC (object);

  GST_OBJECT_LOCK (thiz);
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (thiz);
}

static GstFlowReturn
gst_nilogandec_drain (GstVideoDecoder * decoder)
{
  GstNiloganDec *thiz = GST_NILOGANDEC (decoder);
  GstFlowReturn ret;
  gboolean got_frame = FALSE;
  GstMapInfo info;
  info.size = 0;
  info.data = NULL;

  if (!thiz->opened)
    return GST_FLOW_OK;

  ret = gst_nilogandec_send_frame (thiz, &info, NULL);

  ret = GST_FLOW_OK;
  do {
    got_frame = gst_nilogandec_frame (thiz, NULL, &ret);
    GST_DEBUG ("Drain, got=%d, ret=%d\n", got_frame, ret);
  } while (got_frame && ret == GST_FLOW_OK);

  return GST_FLOW_OK;
}

static gboolean
gst_nilogandec_flush (GstVideoDecoder * decoder)
{
  GstNiloganDec *thiz = GST_NILOGANDEC (decoder);
  GstFlowReturn ret;

  ret = gst_nilogandec_drain (GST_VIDEO_DECODER_CAST (thiz));

  return ret == GST_FLOW_OK;
}

static gboolean
gst_nilogandec_stop (GstVideoDecoder * decoder)
{
  GstNiloganDec *thiz = GST_NILOGANDEC (decoder);

  gst_nilogandec_flush (decoder);

  gst_nilogandec_close_decoder (thiz, TRUE);

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

  if (thiz->dev_xcoder) {
    g_free (thiz->dev_xcoder);
    thiz->dev_xcoder = NULL;
  }


  return TRUE;
}

static GstFlowReturn
gst_nilogandec_finish (GstVideoDecoder * decoder)
{
  return gst_nilogandec_drain (decoder);
}

static gboolean
gst_nilogandec_decide_allocation (GstVideoDecoder * decoder, GstQuery * query)
{
  GstNiloganDec *thiz = GST_NILOGANDEC (decoder);
  GstVideoCodecState *state;
  GstBufferPool *pool;
  guint size, min, max;
  GstStructure *config;
  gboolean have_videometa = FALSE, update_pool = FALSE;
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
gst_nilogandec_propose_allocation (GstVideoDecoder * decoder, GstQuery * query)
{
  GstAllocationParams params;

  gst_allocation_params_init (&params);
  params.flags = GST_MEMORY_FLAG_ZERO_PADDED;
  params.align = DEFAULT_STRIDE_ALIGN;
  //params.padding = 64;
  /* we would like to have some padding so that we don't have to
   * memcpy. We don't suggest an allocator. */
  gst_query_add_allocation_param (query, NULL, &params);

  return GST_VIDEO_DECODER_CLASS (parent_class)->propose_allocation (decoder,
      query);
}

static void
gst_nilogandec_dispose (GObject * object)
{
  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_nilogandec_finalize (GObject * object)
{
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_nilogandec_transform_meta (GstVideoDecoder * decoder,
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

static void
gst_nilogandec_class_init (GstNiloganDecClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstVideoDecoderClass *decoder_class;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  decoder_class = GST_VIDEO_DECODER_CLASS (klass);

  gobject_class->set_property = gst_nilogandec_set_property;
  gobject_class->get_property = gst_nilogandec_get_property;
  gobject_class->dispose = gst_nilogandec_dispose;
  gobject_class->finalize = gst_nilogandec_finalize;

  element_class->set_context = gst_nilogandec_set_context;

  decoder_class->close = GST_DEBUG_FUNCPTR (gst_nilogandec_close);
  decoder_class->start = GST_DEBUG_FUNCPTR (gst_nilogandec_start);
  decoder_class->stop = GST_DEBUG_FUNCPTR (gst_nilogandec_stop);
  decoder_class->set_format = GST_DEBUG_FUNCPTR (gst_nilogandec_set_format);
  decoder_class->finish = GST_DEBUG_FUNCPTR (gst_nilogandec_finish);
  decoder_class->handle_frame = GST_DEBUG_FUNCPTR (gst_nilogandec_handle_frame);
  decoder_class->parse = NULL;
  decoder_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_nilogandec_decide_allocation);
  decoder_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_nilogandec_propose_allocation);
  decoder_class->flush = GST_DEBUG_FUNCPTR (gst_nilogandec_flush);
  decoder_class->drain = GST_DEBUG_FUNCPTR (gst_nilogandec_drain);
  decoder_class->transform_meta =
      GST_DEBUG_FUNCPTR (gst_nilogandec_transform_meta);

  gst_element_class_add_static_pad_template (element_class, &src_factory);
}

#define PROP_DEC_INDEX 0
#define PROP_DEC_NAME "/dev/nvme0n1"
#define PROP_DEC_TIMEOUT 3
#define PROP_DEC_XCODER "bestmodelload"

static void
gst_nilogandec_init (GstNiloganDec * thiz)
{
  thiz->input_state = NULL;
  thiz->output_state = NULL;
  thiz->internal_pool = NULL;
  thiz->keep_alive_timeout = NI_LOGAN_DEFAULT_KEEP_ALIVE_TIMEOUT;
  thiz->dev_dec_idx = PROP_DEC_INDEX;
  thiz->hardware_mode = FALSE;
  thiz->configured = false;
  thiz->dev_xcoder = g_strdup (PROP_DEC_XCODER);
}

gboolean
gst_nilogandec_set_common_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiloganDec *nidec = GST_NILOGANDEC (object);
  GstState state;
  gboolean ret = TRUE;

  GST_OBJECT_LOCK (nidec);
  state = GST_STATE (nidec);
  if ((state != GST_STATE_READY && state != GST_STATE_NULL) &&
      !(pspec->flags & GST_PARAM_MUTABLE_PLAYING)) {
    ret = FALSE;
    goto wrong_state;
  }

  switch (prop_id) {
    case GST_NILOGAN_DEC_PROP_NAME:
      g_free (nidec->dev_xcoder_name);
      nidec->dev_xcoder_name = g_strdup (g_value_get_string (value));
      break;
    case GST_NILOGAN_DEC_PROP_CARD_NUM:
      nidec->dev_dec_idx = g_value_get_int (value);
      break;
    case GST_NILOGAN_DEC_PROP_TIMEOUT:
      nidec->keep_alive_timeout = g_value_get_uint (value);
      break;
    case GST_NILOGAN_DEC_PROP_XCODER_PARAMS:
      g_free (nidec->xcoder_opts);
      nidec->xcoder_opts = g_strdup (g_value_get_string (value));
      break;
    case GST_NILOGAN_DEC_PROP_XCODER:
      g_free (nidec->dev_xcoder);
      nidec->dev_xcoder = g_strdup (g_value_get_string (value));
      break;
    case GST_NILOGAN_DEC_PROP_HW_FRAMES:
      nidec->hardware_mode = g_value_get_boolean (value);
      break;
    case GST_NILOGAN_DEC_PROP_LOW_DELAY:
      nidec->low_delay = g_value_get_int (value);
      break;
    case GST_NILOGAN_DEC_PROP_PRIORITY:
      nidec->set_high_priority = g_value_get_int (value);
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
gst_nilogandec_get_common_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiloganDec *nidec = GST_NILOGANDEC (object);
  GstState state;
  gboolean ret = TRUE;

  GST_OBJECT_LOCK (nidec);
  state = GST_STATE (nidec);
  if ((state != GST_STATE_READY && state != GST_STATE_NULL) &&
      !(pspec->flags & GST_PARAM_MUTABLE_PLAYING)) {
    ret = FALSE;
    goto wrong_state;
  }

  switch (prop_id) {
    case GST_NILOGAN_DEC_PROP_NAME:
      g_free (nidec->dev_xcoder_name);
      nidec->dev_xcoder_name = g_strdup (g_value_get_string (value));
      break;
    case GST_NILOGAN_DEC_PROP_CARD_NUM:
      nidec->dev_dec_idx = g_value_get_int (value);
      break;
    case GST_NILOGAN_DEC_PROP_TIMEOUT:
      nidec->keep_alive_timeout = g_value_get_uint (value);
      break;
    case GST_NILOGAN_DEC_PROP_XCODER_PARAMS:
      g_free (nidec->xcoder_opts);
      nidec->xcoder_opts = g_strdup (g_value_get_string (value));
      break;
    case GST_NILOGAN_DEC_PROP_XCODER:
      g_free (nidec->dev_xcoder);
      nidec->dev_xcoder = g_strdup (g_value_get_string (value));
      break;
    case GST_NILOGAN_DEC_PROP_HW_FRAMES:
      nidec->hardware_mode = g_value_get_boolean (value);
      break;
    case GST_NILOGAN_DEC_PROP_LOW_DELAY:
      nidec->low_delay = g_value_get_int (value);
      break;
    case GST_NILOGAN_DEC_PROP_PRIORITY:
      nidec->set_high_priority = g_value_get_int (value);
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

void
gst_nilogandec_install_common_properties (GstNiloganDecClass * decoder_class)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (decoder_class);
  GParamSpec *obj_properties[GST_NILOGAN_DEC_PROP_MAX] = { NULL, };


  obj_properties[GST_NILOGAN_DEC_PROP_XCODER] =
      g_param_spec_string ("xcoder", "Xcoder",
      "Select which XCoder card to use",
      PROP_DEC_XCODER, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NILOGAN_DEC_PROP_HW_FRAMES] =
      g_param_spec_boolean ("hwframes", "HWFrames",
      "Use hwframes to reduce YUV buffer traffic",
      FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NILOGAN_DEC_PROP_CARD_NUM] =
      g_param_spec_int ("dec", "Dec",
      "Select which decoder to use by index. First is 0, second is 1, and so on",
      -1, INT_MAX, PROP_DEC_INDEX, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NILOGAN_DEC_PROP_NAME] =
      g_param_spec_string ("device-name", "Device-Name",
      "Select which decoder to use by NVMe block device name, e.g. /dev/nvme0n1",
      PROP_DEC_NAME, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NILOGAN_DEC_PROP_XCODER_PARAMS] =
      g_param_spec_string ("xcoder-params", "XCODER-PARAMS",
      "Set the XCoder configuration using a :-separated list of key=value parameters",
      NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NILOGAN_DEC_PROP_TIMEOUT] =
      g_param_spec_uint ("keep-alive-timeout", "TIMEOUT",
      "Specify a custom session keep alive timeout in seconds.", 0, INT_MAX,
      PROP_DEC_TIMEOUT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NILOGAN_DEC_PROP_PRIORITY] =
      g_param_spec_uint ("set-high-priority", "high-priority",
      "Specify a custom session set high priority in 0 or 1.", 0, INT_MAX,
      0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[GST_NILOGAN_DEC_PROP_LOW_DELAY] =
      g_param_spec_int ("low-delay",
      "LOW-DELAY",
      "Enable low delay decoding mode for 1 in, 1 out decoding sequence. set 1 \"\n"
      "     \"to enable low delay mode. Should be used only for streams that are in \"\n"
      "     \"sequence",
      0, INT_MAX, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);


  g_object_class_install_properties (gobject_class, GST_NILOGAN_DEC_PROP_MAX,
      obj_properties);
}
