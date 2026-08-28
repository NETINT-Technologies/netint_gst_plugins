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
 *  \file   gstniquadrajpegdec.c
 *
 *  \brief  Implement of NetInt Quadra jpeg decoder.
 ******************************************************************************/

#include "gstniquadrajpegdec.h"

GST_DEBUG_CATEGORY_STATIC (gst_niquadrajpegdec_debug);
#define GST_CAT_DEFAULT gst_niquadrajpegdec_debug

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("image/jpeg, "
        "width = (int) [ 1, MAX ], height = (int) [ 1, MAX ]")
    );

#define SUPPORTED_FORMATS "{ I420 }"
static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (SUPPORTED_FORMATS) ";"
        GST_VIDEO_CAPS_MAKE_WITH_FEATURES ("memory:NiQuadraMemory",
            SUPPORTED_FORMATS))
    );

static gboolean niquadrajpegdec_element_init (GstPlugin * plugin);

#define gst_niquadrajpegdec_parent_class parent_class

G_DEFINE_TYPE (GstNiquadraJpegDec, gst_niquadrajpegdec, GST_TYPE_NIQUADRADEC);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadrajpegdec,
    niquadrajpegdec_element_init);

static gboolean
gst_niquadrajpegdec_configure (GstNiquadraDec * decoder)
{
  decoder->codec_format = NI_CODEC_FORMAT_JPEG;
  return TRUE;
}

static void
gst_niquadradec_jpeg_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiquadraJpegDec *thiz = GST_NIQUADRAJPEGDEC (object);

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
gst_niquadradec_jpeg_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiquadraJpegDec *thiz = GST_NIQUADRAJPEGDEC (object);

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
gst_niquadrajpegdec_class_init (GstNiquadraJpegDecClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstNiquadraDecClass *decoder_class;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  decoder_class = GST_NIQUADRADEC_CLASS (klass);

  gobject_class->set_property = gst_niquadradec_jpeg_set_property;
  gobject_class->get_property = gst_niquadradec_jpeg_get_property;

  decoder_class->configure = GST_DEBUG_FUNCPTR (gst_niquadrajpegdec_configure);

  gst_niquadradec_install_common_properties (decoder_class);

  gst_element_class_set_static_metadata (element_class,
      "NETINT Quadra JPEG decoder",
      "Codec/Decoder/Video/Hardware",
      "JPEG video encoder based on NetInt libxcoder SDK",
      "Simon Shi <simon.shi@netint.cn>");

  gst_element_class_add_static_pad_template (element_class, &sink_factory);
  gst_element_class_add_static_pad_template (element_class, &src_factory);
}

static void
gst_niquadrajpegdec_init (GstNiquadraJpegDec * thiz)
{
}

static gboolean
niquadrajpegdec_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadrajpegdec_debug, "niquadrajpegdec", 0,
      "niquadradecjpeg");

  return gst_element_register (plugin, "niquadrajpegdec", GST_RANK_NONE,
      GST_TYPE_NIQUADRAJPEGDEC);
}
