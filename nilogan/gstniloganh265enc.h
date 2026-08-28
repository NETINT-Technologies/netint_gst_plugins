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
 *  \file   gstniloganh265dec.h
 *
 *  \brief  Header of NetInt Logan h265 encoder.
 ******************************************************************************/

#ifndef _GST_NILOGAN_H265_ENC_H
#define _GST_NILOGAN_H265_ENC_H
#include "gstniloganenc.h"

G_BEGIN_DECLS
#define GST_TYPE_NILOGANH265ENC \
  (gst_niloganh265enc_get_type())
#define GST_NILOGANH265ENC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NILOGANH265ENC,GstNiloganH265Enc))
#define GST_NILOGANH265ENC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NILOGANH265ENC,GstNiloganH265EncClass))
#define GST_IS_NILOGANH265ENC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NILOGANH265ENC))
#define GST_IS_NILOGANH265ENC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NILOGANH265ENC))
typedef struct _GstNiloganH265Enc GstNiloganH265Enc;
typedef struct _GstNiloganH265EncClass GstNiloganH265EncClass;

struct _GstNiloganH265Enc
{
  GstNiloganEnc base;

  gint profile;
  gint level;
};

struct _GstNiloganH265EncClass
{
  GstNiloganEncClass parent_class;
};

GType gst_niloganh265enc_get_type (void);

G_END_DECLS
#endif //_GST_NILOGAN_H265_ENC_H
