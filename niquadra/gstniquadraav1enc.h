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
 *  \file   gstniquadraav1dec.h
 *
 *  \brief  Header of NetInt Quadra av1 decoder.
 ******************************************************************************/

#ifndef _GST_NIQUADRA_AV1_ENC_H
#define _GST_NIQUADRA_AV1_ENC_H

#include "gstniquadraenc.h"

G_BEGIN_DECLS
#define GST_TYPE_NIQUADRAAV1ENC \
  (gst_niquadraav1enc_get_type())
#define GST_NIQUADRAAV1ENC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRAAV1ENC,GstNiquadraAV1Enc))
#define GST_NIQUADRAAV1ENC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRAAV1ENC,GstNiquadraAV1EncClass))
#define GST_IS_NIQUADRAAV1ENC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRAAV1ENC))
#define GST_IS_NIQUADRAAV1ENC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRAAV1ENC))
typedef struct _GstNiquadraAV1Enc GstNiquadraAV1Enc;
typedef struct _GstNiquadraAV1EncClass GstNiquadraAV1EncClass;

struct _GstNiquadraAV1Enc
{
  GstNiquadraEnc base;

  gint profile;
  gint level;
};

struct _GstNiquadraAV1EncClass
{
  GstNiquadraEncClass parent_class;
};

GType gst_niquadraav1enc_get_type (void);

G_END_DECLS
#endif //_GST_NIQUADRA_AV1_ENC_H
