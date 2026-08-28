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

#ifndef _GST_NIQUADRA_H264_ENC_H
#define _GST_NIQUADRA_H264_ENC_H

#include "gstniquadraenc.h"

G_BEGIN_DECLS
#define GST_TYPE_NIQUADRAH264ENC \
  (gst_niquadrah264enc_get_type())
#define GST_NIQUADRAH264ENC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRAH264ENC,GstNiquadraH264Enc))
#define GST_NIQUADRAH264ENC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRAH264ENC,GstNiquadraH264EncClass))
#define GST_IS_NIQUADRAH264ENC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRAH264ENC))
#define GST_IS_NIQUADRAH264ENC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRAH264ENC))
typedef struct _GstNiquadraH264Enc GstNiquadraH264Enc;
typedef struct _GstNiquadraH264EncClass GstNiquadraH264EncClass;

struct _GstNiquadraH264Enc
{
  GstNiquadraEnc base;

  gint profile;
  gint level;
};

struct _GstNiquadraH264EncClass
{
  GstNiquadraEncClass parent_class;
};

GType gst_niquadrah264enc_get_type (void);

G_END_DECLS
#endif //_GST_NIQUADRA_H264_ENC_H
