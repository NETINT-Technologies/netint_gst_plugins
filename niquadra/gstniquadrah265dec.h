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
 *  \file   gstniquadrah265dec.h
 *
 *  \brief  Header of NetInt Quadra h265 decoder.
 ******************************************************************************/

#ifndef _GST_NIQUADRA_H265_DEC_H
#define _GST_NIQUADRA_H265_DEC_H
#include "gstniquadradec.h"
#include <gst/codecparsers/gsth265parser.h>

G_BEGIN_DECLS
#define GST_TYPE_NIQUADRAH265DEC \
  (gst_niquadrah265dec_get_type())
#define GST_NIQUADRAH265DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRAH265DEC,GstNiquadraH265Dec))
#define GST_NIQUADRAH265DEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRAH265DEC,GstNiquadraH265DecClass))
#define GST_IS_NIQUADRAH265DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRAH265DEC))
#define GST_IS_NIQUADRAH265DEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRAH265DEC))
typedef struct _GstNiquadraH265Dec GstNiquadraH265Dec;
typedef struct _GstNiquadraH265DecClass GstNiquadraH265DecClass;

#define GST_H265_MAX_VPS_COUNT 16
#define GST_H265_MAX_SPS_COUNT 16
#define GST_H265_MAX_PPS_COUNT 64

struct _GstNiquadraH265Dec
{
  GstNiquadraDec base;
  gboolean hvcc_sent;

  gboolean user_data_sei_passthru;
  gint custom_sei_passthru;
  gboolean low_delay;
  
  GstH265Parser *parser;
  gboolean packetized;
  guint nal_length_size;
  GstBuffer *vps_nals[GST_H265_MAX_VPS_COUNT];
  GstBuffer *sps_nals[GST_H265_MAX_SPS_COUNT];
  GstBuffer *pps_nals[GST_H265_MAX_PPS_COUNT];
};

struct _GstNiquadraH265DecClass
{
  GstNiquadraDecClass parent_class;
};

GType gst_niquadrah265dec_get_type (void);

G_END_DECLS
#endif //_GST_NIQUADRA_H265_DEC_H
