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
 *  \file   gstniloganh265dec.c
 *
 *  \brief  Implement of NetInt Logan h265 encoder.
 ******************************************************************************/

#include "gstniloganh265enc.h"

#include <gst/base/base.h>
#include <gst/pbutils/pbutils.h>
#include <string.h>

GST_DEBUG_CATEGORY_EXTERN (gst_niloganh265enc_debug);
#define GST_CAT_DEFAULT gst_niloganh265enc_debug

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h265, "
        "framerate = (fraction) [0/1, MAX], "
        "width = (int) [ 1, MAX ], height = (int) [ 1, MAX ], "
        "stream-format = (string) byte-stream , alignment = (string) au , "
        "profile = (string) { high, main, baseline, constrained-baseline }")
    );

static GstStaticPadTemplate enc_sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{ I420, I420_10 }"))
    );

#define DOCUMENTATION_SINK_CAPS_COMM \
    "format = (string) { I420, I420_10LE }, " \
    "width = (int) [ 0, 8192 ], " \
    "height = (int) [ 0, 8192 ], " \
    "framerate = " GST_VIDEO_FPS_RANGE ", " \
    "interlace-mode = (string) { progressive } "

#define DOCUMENTATION_SINK_CAPS \
    "video/x-raw, " DOCUMENTATION_SINK_CAPS_COMM

#define gst_niloganh265enc_parent_class parent_class
G_DEFINE_TYPE (GstNiloganH265Enc, gst_niloganh265enc, GST_TYPE_NILOGANENC);

static gboolean
gst_niloganh265enc_configure (GstNiloganEnc * encoder)
{
  encoder->codec_format = NI_LOGAN_CODEC_FORMAT_H265;

  return TRUE;
}

static void
gst_niloganh265enc_dispose (GObject * object)
{
  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_niloganh265enc_finalize (GObject * object)
{
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_niloganh265enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiloganH265Enc *thiz = GST_NILOGANH265ENC (object);

  if (gst_niloganenc_set_common_property (object, prop_id, value, pspec))
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
gst_niloganh265enc_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiloganH265Enc *thiz = GST_NILOGANH265ENC (object);

  if (gst_niloganenc_get_common_property (object, prop_id, value, pspec))
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
gst_niloganh265enc_class_init (GstNiloganH265EncClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstVideoEncoderClass *videoencoder_class;
  GstNiloganEncClass *encoder_class;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  videoencoder_class = GST_VIDEO_ENCODER_CLASS (klass);
  encoder_class = GST_NILOGANENC_CLASS (klass);
  GstPadTemplate *pad_templ;
  GstCaps *doc_caps;

  gobject_class->dispose = gst_niloganh265enc_dispose;
  gobject_class->finalize = gst_niloganh265enc_finalize;
  gobject_class->set_property = gst_niloganh265enc_set_property;
  gobject_class->get_property = gst_niloganh265enc_get_property;

  encoder_class->configure = gst_niloganh265enc_configure;

  gst_niloganenc_install_common_properties (encoder_class);

  gst_element_class_set_static_metadata (element_class,
      "NETINT Logan H265 encoder", "Codec/Encoder/Video/Hardware",
      "H265 video encoder based on NetInt libxcoder SDK",
      "Simon Shi <simon.shi@netint.cn>");

  GstCaps *sinkcaps =
      gst_caps_from_string
      ("video/x-raw, format=(string){ I420, I420_10LE }, "
      "width = (int) [ 32, 8192 ], height = (int) [ 32, 5120 ], framerate = "
      GST_VIDEO_FPS_RANGE " , " "interlace-mode = (string) { progressive }");
  GstCaps *srccaps =
      gst_caps_from_string ("video/x-h265, alignment=(string)au");

  /* pad templates */
  GstPadTemplate *sinktempl = gst_pad_template_new ("sink", GST_PAD_SINK,
      GST_PAD_ALWAYS, sinkcaps);
  GstPadTemplate *srctempl =
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, srccaps);

  gst_element_class_add_pad_template (element_class, srctempl);
  gst_element_class_add_pad_template (element_class, sinktempl);

  gst_caps_unref (sinkcaps);
  gst_caps_unref (srccaps);
}

static void
gst_niloganh265enc_init (GstNiloganH265Enc * thiz)
{

}
