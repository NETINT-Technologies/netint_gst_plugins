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
 *  \file   gstniquadravp9dec.c
 *
 *  \brief  Implement of NetInt Quadra vp9 decoder.
 ******************************************************************************/

#include "gstniquadravp9dec.h"

GST_DEBUG_CATEGORY_STATIC (gst_niquadravp9dec_debug);
#define GST_CAT_DEFAULT gst_niquadravp9dec_debug

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-vp9, "
        "width = (int) [ 144, 8192 ], height = (int) [ 144, 8192 ]")
    );

#define SUPPORTED_FORMATS "{ I420, NV12, I420_10LE, P010_10LE }"
static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (SUPPORTED_FORMATS) ";"
        GST_VIDEO_CAPS_MAKE_WITH_FEATURES ("memory:NiQuadraMemory",
            SUPPORTED_FORMATS))
    );

static gboolean niquadravp9dec_element_init (GstPlugin * plugin);

#define gst_niquadravp9dec_parent_class parent_class

G_DEFINE_TYPE (GstNiquadraVP9Dec, gst_niquadravp9dec, GST_TYPE_NIQUADRADEC);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadravp9dec,
    niquadravp9dec_element_init);

static gboolean
gst_niquadravp9dec_configure (GstNiquadraDec * decoder)
{
  decoder->codec_format = NI_CODEC_FORMAT_VP9;
  return TRUE;
}

static void
gst_niquadradec_vp9_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiquadraVP9Dec *thiz = GST_NIQUADRAVP9DEC (object);

  if (gst_niquadradec_set_common_property (object, prop_id, value, pspec))
    return;

  GST_OBJECT_LOCK (thiz);

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (thiz);
}

static void
gst_niquadradec_vp9_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiquadraVP9Dec *thiz = GST_NIQUADRAVP9DEC (object);

  if (gst_niquadradec_get_common_property (object, prop_id, value, pspec))
    return;

  GST_OBJECT_LOCK (thiz);
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (thiz);
}

static void
gst_niquadravp9dec_class_init (GstNiquadraVP9DecClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstNiquadraDecClass *decoder_class;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  decoder_class = GST_NIQUADRADEC_CLASS (klass);

  gobject_class->set_property = gst_niquadradec_vp9_set_property;
  gobject_class->get_property = gst_niquadradec_vp9_get_property;

  decoder_class->configure = GST_DEBUG_FUNCPTR (gst_niquadravp9dec_configure);

  gst_niquadradec_install_common_properties (decoder_class);

  gst_element_class_set_static_metadata (element_class,
      "NETINT Quadra VP9 decoder",
      "Codec/Decoder/Video/Hardware",
      "VP9 video decoder based on NetInt libxcoder",
      "Simon Shi <simon.shi@netint.cn>");

  gst_element_class_add_static_pad_template (element_class, &sink_factory);
  gst_element_class_add_static_pad_template (element_class, &src_factory);
}

static void
gst_niquadravp9dec_init (GstNiquadraVP9Dec * thiz)
{
}

static gboolean
niquadravp9dec_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadravp9dec_debug, "niquadravp9dec", 0,
      "niquadradecvp9");

  return gst_element_register (plugin, "niquadravp9dec", GST_RANK_NONE,
      GST_TYPE_NIQUADRAVP9DEC);
}
