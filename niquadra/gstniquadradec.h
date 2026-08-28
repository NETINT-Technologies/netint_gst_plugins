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
 *  \file   gstniquadradec.h
 *
 *  \brief  Header of NetInt Quadra common decoder.
 ******************************************************************************/

#ifndef _GST_NIQUADRA_DEC_H
#define _GST_NIQUADRA_DEC_H

#include <gst/gst.h>
#include <gst/video/video.h>
#include <ni_rsrc_api.h>
#include <ni_device_api.h>
#include "niquadra.h"
#include "gstniquadracontext.h"

G_BEGIN_DECLS
#define GST_TYPE_NIQUADRADEC \
  (gst_niquadradec_get_type())
#define GST_NIQUADRADEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRADEC,GstNiquadraDec))
#define GST_NIQUADRADEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRADEC,GstNiquadraDecClass))
#define GST_NIQUADRADEC_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_NIQUADRADEC,GstNiquadraDecClass))
#define GST_IS_NIQUADRADEC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRADEC))
#define GST_IS_NIQUADRADEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRADEC))
typedef struct _GstNiquadraDec GstNiquadraDec;
typedef struct _GstNiquadraDecClass GstNiquadraDecClass;

enum
{
  GST_NIQUADRA_DEC_PROP_0,
  GST_NIQUADRA_DEC_PROP_CARD_NUM,
  GST_NIQUADRA_DEC_PROP_NAME,
  GST_NIQUADRA_DEC_PROP_XCODER_PARAMS,
  GST_NIQUADRA_DEC_PROP_TIMEOUT,
  GST_NIQUADRA_DEC_PROP_MAX,
};

enum var_name
{
  VAR_IN_W,
  VAR_IW,
  VAR_IN_H,
  VAR_IH,
  VAR_OUT_W,
  VAR_OW,
  VAR_OUT_H,
  VAR_OH,
  VAR_X,
  VAR_Y,
  VAR_VARS_NB
};

static const char *const var_names[] = {
  "in_w", "iw",                 ///< width  of the input video
  "in_h", "ih",                 ///< height of the input video
  "out_w", "ow",                ///< width  of the cropped video
  "out_h", "oh",                ///< height of the cropped video
  "x",
  "y",
  NULL
};

struct _GstNiquadraDec
{
  GstVideoDecoder element;

  /* input description */
  GstVideoCodecState *input_state;
  GstVideoCodecState *output_state;
  /* downstream pool info based on allocation query */
  gboolean opened;
  gboolean hardware_mode;

  ni_session_context_t hw_api_ctx;

  GstNiquadraContext *context;

  ni_codec_format_t codec_format;

  uint8_t *extradata;
  gint extradata_size;

  gint width;
  gint height;
  gint out_width;
  gint out_height;
  gboolean enable_out1;
  gint out1_width;
  gint out1_height;
  gboolean enable_out2;
  gint out2_width;
  gint out2_height;
  GstVideoFormat pix_fmt;

  int64_t current_pts;
  unsigned long long offset;

  gint started;
  gint draining;
  gint flushing;
  gint is_lone_sei_pkt;
  gint eos;
  gboolean configured;
  gint svct_skip_next_packet;
  uint8_t *lone_pkt_data;
  gint lone_pkt_size;

  /* below are all command line options */
  gchar *dev_xcoder_name;
  gchar *blk_xcoder_name;
  gint dev_dec_idx;
  gchar *dev_blk_name;
  guint keep_alive_timeout;
  gchar *xcoder_opts;
  gboolean enable_user_data_sei_passthru;
  gint custom_sei_type;
  gint low_delay;
  gint pkt_nal_bitmap;
  gint64 frame_number;

  GstBufferPool *internal_pool;
  gint pool_width;
  gint pool_height;
  GstVideoInfo pool_info;
};

struct _GstNiquadraDecClass
{
  GstVideoDecoderClass parent_class;

  gboolean (*configure) (GstNiquadraDec *decoder);

  GstBuffer *(*process_buffer) (GstNiquadraDec *decoder, GstBuffer * buffer);
  gboolean (*parse_format) (GstNiquadraDec *decoder, GstVideoCodecState *state);

  gboolean      (*prepare)           (GstNiquadraDec *decoder);
  gboolean      (*release)           (GstNiquadraDec *decoder);
};


GType gst_niquadradec_get_type (void);

void gst_niquadradec_install_common_properties (GstNiquadraDecClass *decoder_class);

gboolean gst_niquadradec_set_common_property (GObject *object, guint prop_id,
                                             const GValue *value,
                                             GParamSpec *pspec);

gboolean gst_niquadradec_get_common_property (GObject *object, guint prop_id,
                                             GValue *value, GParamSpec *pspec);

G_END_DECLS
#endif //_GST_NIQUADRA_DEC_H
