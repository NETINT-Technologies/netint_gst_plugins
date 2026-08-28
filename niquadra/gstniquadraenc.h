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
 *  \file   gstniquadraenc.h
 *
 *  \brief  Header of NetInt Quadra common encoder.
 ******************************************************************************/

#ifndef _GST_NIQUADRA_ENC_H
#define _GST_NIQUADRA_ENC_H

#include <gst/gst.h>
#include <gst/video/gstvideoencoder.h>
#include "niquadra.h"
#include "gstniquadrautils.h"
#include "gstniquadracontext.h"

G_BEGIN_DECLS
#define GST_TYPE_NIQUADRAENC \
  (gst_niquadraenc_get_type())
#define GST_NIQUADRAENC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRAENC,GstNiquadraEnc))
#define GST_NIQUADRAENC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRAENC,GstNiquadraEncClass))
#define GST_NIQUADRAENC_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_NIQUADRAENC,GstNiquadraEncClass))
#define GST_IS_NIQUADRAENC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRAENC))
#define GST_IS_NIQUADRAENC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRAENC))
typedef struct _GstNiquadraEnc GstNiquadraEnc;
typedef struct _GstNiquadraEncClass GstNiquadraEncClass;

enum
{
  GST_NIQUADRA_ENC_PROP_0,
  GST_NIQUADRA_ENC_PROP_CARD_NUM,
  GST_NIQUADRA_ENC_PROP_NAME,
  GST_NIQUADRA_ENC_PROP_IO_SIZE,
  GST_NIQUADRA_ENC_PROP_XCODER_PARAM,
  GST_NIQUADRA_ENC_PROP_XCODER_GOP,
  GST_NIQUADRA_ENC_PROP_TIMEOUT,
  GST_NIQUADRA_ENC_PROP_MAX,
};

struct _GstNiquadraEnc
{
  GstVideoEncoder element;
  GstNiquadraContext *context;

  /* input description */
  GstVideoCodecState *input_state;
  ni_codec_format_t codec_format;
  gboolean hardware_mode;
  GQueue *pending_frames;

  gint width, height;
  gint fps_den, fps_num;
  gint time_base_den, time_base_num;
  gint sample_aspect_den, sample_aspect_num;
  gint bits_per_coded_sample;
  gint64 ticks_per_frame;
  GstVideoFormat pix_fmt;
  GstVideoChromaSite chroma_site;
  GstVideoColorimetry colorimetry;

  /* List of frame/buffer mapping structs for
   * pending frames */
  gint64 frame_number;
  guint32 cur_frame_index;

  guint64 xcode_load_pixel;
  gint64 bit_rate;

  gint eos_fme_received;

  gint started;
  gboolean opened;
  guint8 *p_spsPpsHdr;
  gint spsPpsHdrLen;
  gint spsPpsArrived;
  gint firstPktArrived;
  gint64 dtsOffset;
  gint gop_offset_count;
  guint64 total_frames_received;
  gint64 first_frame_pts;
  gint64 latest_dts;

  gint encoder_flushing;
  gint encoder_eof;

  gint roi_side_data_size;
  GstVideoRegionOfInterestMeta *roi_data;
  gint nb_rois;

  /* backup copy of original values of -enc command line option */
  gint orig_dev_enc_idx;

  /* backup copy of original xcoder-param command line option */
  gchar *orig_xcoder_opts;

  /* below are all command line options */
  gchar *dev_xcoder_name;       /* dev name of the xcoder card to use */
  gchar *blk_xcoder_name;       /* blk name of the xcoder card to use */
  gint dev_enc_idx;             /* user-specified encoder index */
  gchar *dev_blk_name;          /* user-specified encoder block device name */
  gint nvme_io_size;
  guint keep_alive_timeout;
  gchar *device_name;
  gchar *xcoder_opts;
  gchar *xcoder_gop;

  gint reconfigCount;
};

struct _GstNiquadraEncClass
{
  GstVideoEncoderClass parent_class;

  gboolean (*set_format) (GstNiquadraEnc *encoder);

  gboolean (*configure) (GstNiquadraEnc *encoder);

  GstCaps *(*set_src_caps) (GstNiquadraEnc *encoder);
};

GType gst_niquadraenc_get_type (void);

void gst_niquadraenc_install_common_properties (GstNiquadraEncClass *encoder_class);

gboolean gst_niquadraenc_set_common_property (GObject *object, guint prop_id,
                                              const GValue *value,
                                              GParamSpec *pspec);

gboolean gst_niquadraenc_get_common_property (GObject *object, guint prop_id,
                                              GValue *value, GParamSpec *pspec);

#endif //_GST_NIQUADRA_ENC_H
