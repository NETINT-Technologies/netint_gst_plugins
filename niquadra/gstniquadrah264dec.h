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
 *  \file   gstniquadrah264dec.h
 *
 *  \brief  Header of NetInt Quadra h264 decoder.
 ******************************************************************************/

#ifndef _GST_NIQUADRA_H264_DEC_H
#define _GST_NIQUADRA_H264_DEC_H

#include <gst/codecparsers/gsth264parser.h>
#include "gstniquadradec.h"

G_BEGIN_DECLS
#define GST_TYPE_NIQUADRAH264DEC \
  (gst_niquadrah264dec_get_type())
#define GST_NIQUADRAH264DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRAH264DEC,GstNiquadraH264Dec))
#define GST_NIQUADRAH264DEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRAH264DEC,GstNiquadraH264DecClass))
#define GST_IS_NIQUADRAH264DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRAH264DEC))
#define GST_IS_NIQUADRAH264DEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRAH264DEC))
typedef struct _GstNiquadraH264Dec GstNiquadraH264Dec;
typedef struct _GstNiquadraH264DecClass GstNiquadraH264DecClass;

struct _GstNiquadraH264Dec
{
  GstNiquadraDec base;
  gboolean avcc_sent;

  gboolean user_data_sei_passthru;
  gint custom_sei_passthru;
  gboolean low_delay;

  GstH264NalParser *parser;
  gboolean packetized;
  gboolean nal_length_size;
  GstBuffer *sps_nals[GST_H264_MAX_SPS_COUNT];
  GstBuffer *pps_nals[GST_H264_MAX_PPS_COUNT];
};

struct _GstNiquadraH264DecClass
{
  GstNiquadraDecClass parent_class;
};

GType gst_niquadrah264dec_get_type (void);

G_END_DECLS
#endif //_GST_NIQUADRA_H264_DEC_H
