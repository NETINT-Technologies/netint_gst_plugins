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
 *  \file   gstniquadrah264enc.c
 *
 *  \brief  Implement of NetInt Quadra h264 encoder.
 ******************************************************************************/

#include "config.h"

#include "gstniquadrah264enc.h"

#include <gst/base/base.h>
#include <gst/pbutils/pbutils.h>
#include <string.h>

GST_DEBUG_CATEGORY_STATIC (gst_niquadrah264enc_debug);
#define GST_CAT_DEFAULT gst_niquadrah264enc_debug

#if ENABLE_NI_GST_FRAMEWORK_PATCH
#define NI_QUADRA_TILED_FORMATS \
    ", NI_QUAD_8_4L4, NI_QUAD_10_4L4"
#else
#define NI_QUADRA_TILED_FORMATS ""
#endif

#define SINK_CAPS_COMM \
    "format = (string) { I420, I420_10LE, NV12, P010_10LE, ARGB, RGBA, ABGR, BGRA" \
    NI_QUADRA_TILED_FORMATS " }, " \
    "width = (int) [ 1, 8192 ], " \
    "height = (int) [ 1, 8192 ], " \
    "framerate = " GST_VIDEO_FPS_RANGE ", " \
    "interlace-mode = (string) { progressive } "

#define SINK_CAPS \
    "video/x-raw, " SINK_CAPS_COMM "; " \
    "video/x-raw(memory:NiQuadraMemory), " SINK_CAPS_COMM

#define SRC_CAPS \
    "video/x-h264, "  \
    "width = (int) [ 1, 8192 ], " \
    "height = (int) [ 1, 8192 ], " \
    "chroma-format = (string) 4:2:0, " \
    "stream-format = (string) byte-stream, " \
    "alignment = (string) au"

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (SINK_CAPS)
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (SRC_CAPS)
    );

static gboolean niquadrah264enc_element_init (GstPlugin * plugin);

#define gst_niquadrah264enc_parent_class parent_class

G_DEFINE_TYPE (GstNiquadraH264Enc, gst_niquadrah264enc, GST_TYPE_NIQUADRAENC);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadrah264enc,
    niquadrah264enc_element_init);

static gboolean
gst_niquadrah264enc_configure (GstNiquadraEnc * encoder)
{
  encoder->codec_format = NI_CODEC_FORMAT_H264;

  return TRUE;
}

static void
gst_niquadrah264enc_dispose (GObject * object)
{
  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_niquadrah264enc_finalize (GObject * object)
{
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_niquadrah264enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiquadraH264Enc *thiz = GST_NIQUADRAH264ENC (object);

  if (gst_niquadraenc_set_common_property (object, prop_id, value, pspec))
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
gst_niquadrah264enc_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiquadraH264Enc *thiz = GST_NIQUADRAH264ENC (object);

  if (gst_niquadraenc_get_common_property (object, prop_id, value, pspec))
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
gst_niquadrah264enc_class_init (GstNiquadraH264EncClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstNiquadraEncClass *encoder_class;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  encoder_class = GST_NIQUADRAENC_CLASS (klass);

  gobject_class->dispose = gst_niquadrah264enc_dispose;
  gobject_class->finalize = gst_niquadrah264enc_finalize;
  gobject_class->set_property = gst_niquadrah264enc_set_property;
  gobject_class->get_property = gst_niquadrah264enc_get_property;

  encoder_class->configure = gst_niquadrah264enc_configure;

  gst_niquadraenc_install_common_properties (encoder_class);

  gst_element_class_set_static_metadata (element_class,
      "NETINT Quadra H264 encoder", "Codec/Encoder/Video/Hardware",
      "H264 video encoder based on libxcoder SDK",
      "Simon Shi <simon.shi@netint.cn>");

  gst_element_class_add_static_pad_template (element_class, &sink_factory);
  gst_element_class_add_static_pad_template (element_class, &src_factory);
}

static void
gst_niquadrah264enc_init (GstNiquadraH264Enc * thiz)
{

}

static gboolean
niquadrah264enc_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadrah264enc_debug, "niquadrah264enc", 0,
      "niquadrah264enc");

  return gst_element_register (plugin, "niquadrah264enc", GST_RANK_NONE,
      GST_TYPE_NIQUADRAH264ENC);
}
