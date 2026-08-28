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
 *  \file   gstniloganh265dec.h
 *
 *  \brief  Header of NetInt Logan h265 decoder.
 ******************************************************************************/

#ifndef _GST_NILOGAN_H265_DEC_H
#define _GST_NILOGAN_H265_DEC_H
#include "gstnilogandec.h"

G_BEGIN_DECLS
#define GST_TYPE_NILOGANH265DEC \
  (gst_niloganh265dec_get_type())
#define GST_NILOGANH265DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NILOGANH265DEC,GstNiloganH265Dec))
#define GST_NILOGANH265DEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NILOGANH265DEC,GstNiloganH265DecClass))
#define GST_IS_NILOGANH265DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NILOGANH265DEC))
#define GST_IS_NILOGANH265DEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NILOGANH265DEC))
typedef struct _GstNiloganH265Dec GstNiloganH265Dec;
typedef struct _GstNiloganH265DecClass GstNiloganH265DecClass;

struct _GstNiloganH265Dec
{
  GstNiloganDec base;
  gboolean user_data_sei_passthru;
  gint custom_sei_passthru;
};

struct _GstNiloganH265DecClass
{
  GstNiloganDecClass parent_class;
};

GType gst_niloganh265dec_get_type (void);

G_END_DECLS
#endif //_GST_NILOGAN_H265_DEC_H
