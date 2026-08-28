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
 *  \file   gstniquadravp9dec.h
 *
 *  \brief  Header of NetInt Quadra vp9 decoder.
 ******************************************************************************/

#ifndef _GST_NIQUADRA_VP9_DEC_H
#define _GST_NIQUADRA_VP9_DEC_H

#include "gstniquadradec.h"

G_BEGIN_DECLS
#define GST_TYPE_NIQUADRAVP9DEC \
  (gst_niquadravp9dec_get_type())
#define GST_NIQUADRAVP9DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRAVP9DEC,GstNiquadraVP9Dec))
#define GST_NIQUADRAVP9DEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRAVP9DEC,GstNiquadraVP9DecClass))
#define GST_IS_NIQUADRAVP9DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRAVP9DEC))
#define GST_IS_NIQUADRAVP9DEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRAVP9DEC))
typedef struct _GstNiquadraVP9Dec GstNiquadraVP9Dec;
typedef struct _GstNiquadraVP9DecClass GstNiquadraVP9DecClass;

struct _GstNiquadraVP9Dec
{
  GstNiquadraDec base;
};

struct _GstNiquadraVP9DecClass
{
  GstNiquadraDecClass parent_class;
};

GType gst_niquadravp9dec_get_type (void);

G_END_DECLS
#endif //_GST_NIQUADRA_VP9_DEC_H
