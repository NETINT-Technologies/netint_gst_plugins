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
 *  \file   gstniloganh264dec.h
 *
 *  \brief  Header of NetInt Logan h264 decoder.
 ******************************************************************************/

#ifndef _GST_NILOGAN_H264_DEC_H
#define _GST_NILOGAN_H264_DEC_H

#include "gstnilogandec.h"

G_BEGIN_DECLS
#define GST_TYPE_NILOGANH264DEC \
  (gst_niloganh264dec_get_type())
#define GST_NILOGANH264DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NILOGANH264DEC,GstNiloganH264Dec))
#define GST_NILOGANH264DEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NILOGANH264DEC,GstNiloganH264DecClass))
#define GST_IS_NILOGANH264DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NILOGANH264DEC))
#define GST_IS_NILOGANH264DEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NILOGANH264DEC))
typedef struct _GstNiloganH264Dec GstNiloganH264Dec;
typedef struct _GstNiloganH264DecClass GstNiloganH264DecClass;

struct _GstNiloganH264Dec
{
  GstNiloganDec base;

  gboolean user_data_sei_passthru;
  gint custom_sei_passthru;
};

struct _GstNiloganH264DecClass
{
  GstNiloganDecClass parent_class;
};

GType gst_niloganh264dec_get_type (void);

G_END_DECLS
#endif //_GST_NILOGAN_H264_DEC_H
