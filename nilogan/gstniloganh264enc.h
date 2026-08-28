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

#ifndef _GST_NILOGAN_H264_ENC_H
#define _GST_NILOGAN_H264_ENC_H

#include "gstniloganenc.h"

G_BEGIN_DECLS
#define GST_TYPE_NILOGANH264ENC \
  (gst_niloganh264enc_get_type())
#define GST_NILOGANH264ENC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NILOGANH264ENC,GstNiloganH264Enc))
#define GST_NILOGANH264ENC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NILOGANH264ENC,GstNiloganH264EncClass))
#define GST_IS_NILOGANH264ENC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NILOGANH264ENC))
#define GST_IS_NILOGANH264ENC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NILOGANH264ENC))
typedef struct _GstNiloganH264Enc GstNiloganH264Enc;
typedef struct _GstNiloganH264EncClass GstNiloganH264EncClass;

struct _GstNiloganH264Enc
{
  GstNiloganEnc base;

  gint profile;
  gint level;
};

struct _GstNiloganH264EncClass
{
  GstNiloganEncClass parent_class;
};

GType gst_niloganh264enc_get_type (void);

G_END_DECLS
#endif //_GST_NILOGAN_H264_ENC_H
