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
 *  \file   gstniquadrajpegdec.h
 *
 *  \brief  Header of NetInt Quadra jpeg decoder.
 ******************************************************************************/

#ifndef _GST_NIQUADRA_JPEG_ENC_H
#define _GST_NIQUADRA_JPEG_ENC_H

#include "gstniquadraenc.h"

G_BEGIN_DECLS
#define GST_TYPE_NIQUADRAJPEGENC \
  (gst_niquadrajpegenc_get_type())
#define GST_NIQUADRAJPEGENC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRAJPEGENC,GstNiquadraJpegEnc))
#define GST_NIQUADRAJPEGENC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRAJPEGENC,GstNiquadraJpegEncClass))
#define GST_IS_NIQUADRAJPEGENC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRAJPEGENC))
#define GST_IS_NIQUADRAJPEGENC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRAJPEGENC))
typedef struct _GstNiquadraJpegEnc GstNiquadraJpegEnc;
typedef struct _GstNiquadraJpegEncClass GstNiquadraJpegEncClass;

struct _GstNiquadraJpegEnc
{
  GstNiquadraEnc base;
};

struct _GstNiquadraJpegEncClass
{
  GstNiquadraEncClass parent_class;
};

GType gst_niquadrajpegenc_get_type (void);

G_END_DECLS
#endif //_GST_NIQUADRA_JPEG_ENC_H
