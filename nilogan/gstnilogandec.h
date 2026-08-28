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
 *  \file   gstnilogandec.h
 *
 *  \brief  Header of NetInt Logan common decoder.
 ******************************************************************************/

#ifndef _GST_NILOGAN_DEC_H
#define _GST_NILOGAN_DEC_H
#include <gst/gst.h>
#include <gst/video/video.h>
#include <ni_rsrc_api_logan.h>
#include <ni_device_api_logan.h>
#include "gstnilogancontext.h"
#include "gstniloganutils.h"

G_BEGIN_DECLS
#define GST_TYPE_NILOGANDEC \
  (gst_nilogandec_get_type())
#define GST_NILOGANDEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NILOGANDEC,GstNiloganDec))
#define GST_NILOGANDEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NILOGANDEC,GstNiloganDecClass))
#define GST_NILOGANDEC_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_NILOGANDEC,GstNiloganDecClass))
#define GST_IS_NILOGANDEC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NILOGANDEC))
#define GST_IS_NILOGANDEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NILOGANDEC))
typedef struct _GstNiloganDec GstNiloganDec;
typedef struct _GstNiloganDecClass GstNiloganDecClass;

enum
{
  GST_NILOGAN_DEC_PROP_0,
  GST_NILOGAN_DEC_PROP_CARD_NUM,
  GST_NILOGAN_DEC_PROP_NAME,
  GST_NILOGAN_DEC_PROP_XCODER_PARAMS,
  GST_NILOGAN_DEC_PROP_TIMEOUT,
  GST_NILOGAN_DEC_PROP_HW_FRAMES,
  GST_NILOGAN_DEC_PROP_XCODER,
  GST_NILOGAN_DEC_PROP_LOW_DELAY,
  GST_NILOGAN_DEC_PROP_PRIORITY,
  GST_NILOGAN_DEC_PROP_MAX,
};

/*
 * Struct to store gstvideocodecframe.
 * */
typedef struct
{
  GstVideoCodecFrame *frame;
  GstVideoFrame vframe;
  GstBuffer *buffer;
} GstNilogandecVideoFrame;

struct _GstNiloganDec
{
  GstVideoDecoder element;

  /* input description */
  GstVideoCodecState *input_state;
  GstVideoCodecState *output_state;
  /* downstream pool info based on allocation query */
  gboolean opened;
  gboolean hardware_mode;

  ni_logan_session_context_t hw_api_ctx;

  GstNiloganContext *context;

  ni_logan_codec_format_t codec_format;

  gint got_first_key_frame;
  uint8_t *extradata;
  gint extradata_size;

  GList *frame_list;

  /* below are set as ffmpeg's AVFormatContext */
  gint width;
  gint height;
  GstVideoFormat pix_fmt;

  int64_t current_pts;
  unsigned long long offset;

  gint started;
  gint draining;
  gint flushing;
  gint is_lone_sei_pkt;
  gint eos;
  gboolean configured;

  /* below are all command line options */
  gchar *dev_xcoder;
  gchar *dev_xcoder_name;
  gchar *blk_xcoder_name;
  gint dev_dec_idx;
  gchar *dev_blk_name;
  guint keep_alive_timeout;
  gint set_high_priority;
  gchar *xcoder_opts;
  gboolean enable_user_data_sei_passthru;
  gint enable_check_packet;
  gint custom_sei_type;
  gint low_delay;
  gint pkt_nal_bitmap;
  gint64 frame_number;

  GstBufferPool *internal_pool;
  gint pool_width;
  gint pool_height;
  GstVideoInfo pool_info;
};

struct _GstNiloganDecClass
{
  GstVideoDecoderClass parent_class;

    gboolean (*configure) (GstNiloganDec * decoder);
};


GType gst_nilogandec_get_type (void);

void gst_nilogandec_install_common_properties (GstNiloganDecClass *
    decoder_class);

gboolean
gst_nilogandec_set_common_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);

gboolean
gst_nilogandec_get_common_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

G_END_DECLS
#endif //_GST_NILOGAN_DEC_H
