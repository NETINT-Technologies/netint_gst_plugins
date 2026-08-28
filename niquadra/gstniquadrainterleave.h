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
 *  \file   gstniquadrainterleave.h
 *
 *  \brief  Header of NetInt Quadra interleave filter.
 ******************************************************************************/

#ifndef __GST_NIQUADRAINTERLEAVE_H__
#define __GST_NIQUADRAINTERLEAVE_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/base/gstaggregator.h>

#include "ni_device_api.h"

G_BEGIN_DECLS

#define GST_TYPE_NIQUADRAINTERLEAVE (gst_niquadrainterleave_get_type())

G_DECLARE_FINAL_TYPE (GstNiQuadraInterleave, gst_niquadrainterleave, GST,
                      NIQUADRAINTERLEAVE, GstVideoAggregator)

/**
 * GstNiQuadraInterleave:
 *
 * The opaque #GstNiQuadraInterleave structure.
 */
struct _GstNiQuadraInterleave
{
  GstAggregator agg;
  guint nb_inputs;
};

G_END_DECLS
#endif /* __GST_NIQUADRAINTERLEAVE_H__ */
