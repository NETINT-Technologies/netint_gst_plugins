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
 *  \brief  Implement of NetInt Logan h265 decoder.
 ******************************************************************************/

#include "gstniloganh265dec.h"

GST_DEBUG_CATEGORY_EXTERN (gst_niloganh265dec_debug);
#define GST_CAT_DEFAULT gst_niloganh265dec_debug

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h265, "
        "width = (int) [ 32, 8192 ], height = (int) [ 32, 5120 ], "
        "stream-format = (string) byte-stream , alignment = (string) au ")
    );

#define gst_niloganh265dec_parent_class parent_class
G_DEFINE_TYPE (GstNiloganH265Dec, gst_niloganh265dec, GST_TYPE_NILOGANDEC);

enum
{
  PROP_USER_DATA_SEI_PASSTHRU = GST_NILOGAN_DEC_PROP_MAX,
  PROP_CUSTOM_SEI_PASSTHRU,
};

#define PROP_CUSTOM_SEI_PASSTHRU_DEFAULT -1

static gboolean
gst_niloganh265dec_configure (GstNiloganDec * decoder)
{
  GstNiloganH265Dec *h265dec = GST_NILOGANH265DEC (decoder);
  decoder->enable_user_data_sei_passthru = h265dec->user_data_sei_passthru;
  decoder->custom_sei_type = h265dec->custom_sei_passthru;
  decoder->codec_format = NI_LOGAN_CODEC_FORMAT_H265;
  return TRUE;
}

static void
gst_nilogandec_h265_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiloganH265Dec *thiz = GST_NILOGANH265DEC (object);

  if (gst_nilogandec_set_common_property (object, prop_id, value, pspec))
    return;

  GST_OBJECT_LOCK (thiz);
  switch (prop_id) {
    case PROP_USER_DATA_SEI_PASSTHRU:
      thiz->user_data_sei_passthru = g_value_get_boolean (value);
      break;
    case PROP_CUSTOM_SEI_PASSTHRU:
      thiz->custom_sei_passthru = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (thiz);
}

static void
gst_nilogandec_h265_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiloganH265Dec *thiz = GST_NILOGANH265DEC (object);

  if (gst_nilogandec_get_common_property (object, prop_id, value, pspec))
    return;
  GST_OBJECT_LOCK (thiz);
  switch (prop_id) {
    case PROP_USER_DATA_SEI_PASSTHRU:
      g_value_set_boolean (value, thiz->user_data_sei_passthru);
      break;
    case PROP_CUSTOM_SEI_PASSTHRU:
      g_value_set_uint (value, thiz->custom_sei_passthru);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (thiz);
}

static void
gst_niloganh265dec_class_init (GstNiloganH265DecClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstNiloganDecClass *decoder_class;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  decoder_class = GST_NILOGANDEC_CLASS (klass);

  gobject_class->set_property = gst_nilogandec_h265_set_property;
  gobject_class->get_property = gst_nilogandec_h265_get_property;

  decoder_class->configure = GST_DEBUG_FUNCPTR (gst_niloganh265dec_configure);

  gst_nilogandec_install_common_properties (decoder_class);

  g_object_class_install_property (gobject_class, PROP_USER_DATA_SEI_PASSTHRU,
      g_param_spec_boolean ("user-data-sei-passthru",
          "USER-DATA-SEI-PASSTHRU",
          "Enable user data unregistered SEI passthrough.",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_CUSTOM_SEI_PASSTHRU,
      g_param_spec_int ("custom-sei-passthru",
          "CUSTOM-SEI-PASSTHRU",
          "Specify a custom SEI type to passthrough.", -1,
          254,
          PROP_CUSTOM_SEI_PASSTHRU_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata (element_class,
      "NETINT Logan H265 decoder",
      "Codec/Decoder/Video/Hardware",
      "H265 video decoder based on NetInt libxcoder_logan SDK",
      "Simon Shi <simon.shi@netint.cn>");
  gst_element_class_add_static_pad_template (element_class, &sink_factory);
}

static void
gst_niloganh265dec_init (GstNiloganH265Dec * thiz)
{
  thiz->user_data_sei_passthru = FALSE;
  thiz->custom_sei_passthru = -1;
}
