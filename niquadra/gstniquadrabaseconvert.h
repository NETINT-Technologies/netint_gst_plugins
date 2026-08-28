/*******************************************************************************
 *
 * Copyright (C) 2025 NETINT Technologies
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
 *  \file   gstniquadrabaseconvert.h
 *
 *  \brief  Implement of NetInt Quadra base class for scale, crop, pad filter.
 ******************************************************************************/

#ifndef __GST_NI_QUADRA_BASE_CONVERT_H__
#define __GST_NI_QUADRA_BASE_CONVERT_H__

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/base/gstbasetransform.h>

#include "niquadra.h"
#include "ni_device_api.h"
#include "gstniquadrautils.h"

G_BEGIN_DECLS

#define GST_TYPE_NI_QUADRA_BASE_CONVERT (gst_ni_quadra_base_convert_get_type())
#define GST_NI_QUADRA_BASE_CONVERT(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NI_QUADRA_BASE_CONVERT,GstNiQuadraBaseConvert))
#define GST_NI_QUADRA_BASE_CONVERT_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NI_QUADRA_BASE_CONVERT,GstNiQuadraBaseConvertClass))
#define GST_IS_NI_QUADRA_BASE_CONVERT(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NI_QUADRA_BASE_CONVERT))
#define GST_IS_NI_QUADRA_BASE_CONVERT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NI_QUADRA_BASE_CONVERT))

typedef struct _GstNiQuadraBaseConvert GstNiQuadraBaseConvert;
typedef struct _GstNiQuadraBaseConvertClass GstNiQuadraBaseConvertClass;

struct _GstNiQuadraBaseConvert
{
  GstBaseTransform parent;

  guint keep_alive_timeout;

  gboolean initialized;
  GstVideoInfo in_info, out_info;

  ni_session_context_t api_ctx;
};

struct _GstNiQuadraBaseConvertClass
{
  GstBaseTransformClass parent_class;

  GstCaps *(*get_fixed_format) (GstBaseTransform *trans,
                                GstPadDirection direction,
                                GstCaps *caps,
                                GstCaps *othercaps);

  GstCaps *(*fixate_size) (GstBaseTransform *base, GstPadDirection direction,
                           GstCaps *caps, GstCaps *othercaps);
};

#define gst_ni_quadra_base_convert_parent_class parent_class

GType gst_ni_quadra_base_convert_get_type(void);

G_END_DECLS

#endif /* __GST_NI_QUADRA_BASE_CONVERT_H__ */
