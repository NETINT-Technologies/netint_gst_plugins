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
 *  \file   gstniquadrah265dec.c
 *
 *  \brief  Implement of NetInt Quadra h265 decoder.
 ******************************************************************************/

#define GST_USE_UNSTABLE_API
#include "gstniquadrah265dec.h"

#include <gst/base/gstbitreader.h>

GST_DEBUG_CATEGORY_STATIC (gst_niquadrah265dec_debug);
#define GST_CAT_DEFAULT gst_niquadrah265dec_debug

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h265, "
        "width = (int) [ 144, 8192 ], height = (int) [ 144, 8192 ], "
        "stream-format = (string) {byte-stream, hev1, hvc1}, alignment = (string) au")
    );

#define SUPPORTED_FORMATS "{ I420, NV12, I420_10LE, P010_10LE }"
static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (SUPPORTED_FORMATS) ";"
        GST_VIDEO_CAPS_MAKE_WITH_FEATURES ("memory:NiQuadraMemory",
            SUPPORTED_FORMATS))
    );

static gboolean niquadrah265dec_element_init (GstPlugin * plugin);

#define gst_niquadrah265dec_parent_class parent_class

G_DEFINE_TYPE (GstNiquadraH265Dec, gst_niquadrah265dec, GST_TYPE_NIQUADRADEC);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadrah265dec,
    niquadrah265dec_element_init);

enum
{
  PROP_USER_DATA_SEI_PASSTHRU = GST_NIQUADRA_DEC_PROP_MAX,
  PROP_CUSTOM_SEI_PASSTHRU,
  PROP_LOW_DELAY,
};

#define PROP_CUSTOM_SEI_PASSTHRU_DEFAULT -1

static gboolean gst_niquadrah265dec_parse_format (GstNiquadraDec * decoder,
    GstVideoCodecState * state);
static GstBuffer *gst_niquadrah265dec_process_buffer (GstNiquadraDec * decoder,
    GstBuffer * buffer);
static gboolean gst_niquadrah265dec_parse_hvcc (GstNiquadraH265Dec * self,
    const guint8 * data, gsize size);

static void
gst_niquadrah265dec_store_nal (GstNiquadraH265Dec * self, guint id,
    GstH265NalUnitType nal_type, GstH265NalUnit * nalu)
{
  GstBuffer *buf, **store;
  guint size = nalu->size, store_size;
  static const guint8 start_code[] = { 0, 0, 1 };

  if (nal_type == GST_H265_NAL_VPS) {
    store_size = GST_H265_MAX_VPS_COUNT;
    store = self->vps_nals;
  } else if (nal_type == GST_H265_NAL_SPS) {
    store_size = GST_H265_MAX_SPS_COUNT;
    store = self->sps_nals;
  } else if (nal_type == GST_H265_NAL_PPS) {
    store_size = GST_H265_MAX_PPS_COUNT;
    store = self->pps_nals;
  } else {
    return;
  }

  if (id >= store_size) {
    GST_WARNING_OBJECT (self, "id:%d out-of-range %d", id, store_size);
    return;
  }

  if (store[id])
    gst_buffer_unref (store[id]);

  buf = gst_buffer_new_allocate (NULL, size + sizeof (start_code), NULL);
  gst_buffer_fill (buf, 0, start_code, sizeof (start_code));
  gst_buffer_fill (buf, sizeof (start_code), nalu->data + nalu->offset, size);

  store[id] = buf;
}

static void
gst_niquadrah265dec_clear_hvcc (GstNiquadraH265Dec * self)
{
  guint i;

  for (i = 0; i < GST_H265_MAX_VPS_COUNT; i++) {
    if (self->vps_nals[i]) {
      gst_buffer_unref (self->vps_nals[i]);
      self->vps_nals[i] = NULL;
    }
  }

  for (i = 0; i < GST_H265_MAX_SPS_COUNT; i++) {
    if (self->sps_nals[i]) {
      gst_buffer_unref (self->sps_nals[i]);
      self->sps_nals[i] = NULL;
    }
  }

  for (i = 0; i < GST_H265_MAX_PPS_COUNT; i++) {
    if (self->pps_nals[i]) {
      gst_buffer_unref (self->pps_nals[i]);
      self->pps_nals[i] = NULL;
    }
  }
}

static gboolean
gst_niquadrah265dec_parse_hvcc (GstNiquadraH265Dec * self,
    const guint8 * data, gsize size)
{
  GstBitReader br;
  GstH265VPS vps;
  GstH265SPS sps;
  GstH265PPS pps;
  guint8 tmp8;
  guint16 tmp16;
  GstH265NalUnit nalu;
  guint num_nal_groups;
  guint num_nals, i, j;
  GstH265ParserResult pres;
  GstH265Parser *parser = self->parser;

  gst_bit_reader_init (&br, data, size);

  /* check hvcC version */
  if (!gst_bit_reader_get_bits_uint8 (&br, &tmp8, 8))
    return FALSE;

  if (tmp8 != 0 && tmp8 != 1) {
    GST_WARNING_OBJECT (self, "hvcC version wrong");
    return FALSE;
  }

  /* skip bytes 1-20 (20 bytes = 160 bits) */
  if (!gst_bit_reader_skip (&br, 160)) {
    GST_WARNING_OBJECT (self, "hvcC too small");
    return FALSE;
  }

  /* byte 21: nal_length_size */
  if (gst_bit_reader_get_bits_uint8 (&br, &tmp8, 8))
    self->nal_length_size = (tmp8 & 0x03) + 1;
  else
    return FALSE;

  /* byte 22: num_nal_groups */
  if (gst_bit_reader_get_bits_uint8 (&br, &tmp8, 8))
    num_nal_groups = tmp8;
  else
    return FALSE;


  for (i = 0; i < num_nal_groups; i++) {
    /* skip array_completeness and nal_unit_type (1 byte) */
    if (!gst_bit_reader_skip (&br, 8))
      goto too_small;

    /* num_nals (2 bytes) */
    if (!gst_bit_reader_get_bits_uint16 (&br, &tmp16, 16))
      goto too_small;

    num_nals = tmp16;

    for (j = 0; j < num_nals; j++) {
      guint offset = gst_bit_reader_get_pos (&br) / 8;

      pres = gst_h265_parser_identify_nalu_hevc (parser,
          data, offset, size, 2, &nalu);

      if (pres != GST_H265_PARSER_OK) {
        goto too_small;
      }

      switch (nalu.type) {
        case GST_H265_NAL_VPS:
          pres = gst_h265_parser_parse_vps (parser, &nalu, &vps);
          if (pres != GST_H265_PARSER_OK) {
            GST_WARNING_OBJECT (self, "Failed to parse VPS");
            return FALSE;
          }

          gst_niquadrah265dec_store_nal (self, vps.id,
              (GstH265NalUnitType) nalu.type, &nalu);
          break;
        case GST_H265_NAL_SPS:
          pres = gst_h265_parser_parse_sps (self->parser, &nalu, &sps, FALSE);
          if (pres != GST_H265_PARSER_OK) {
            GST_WARNING_OBJECT (self, "Failed to parse SPS");
            return FALSE;
          }

          gst_niquadrah265dec_store_nal (self, sps.id,
              (GstH265NalUnitType) nalu.type, &nalu);
          break;
        case GST_H265_NAL_PPS:
          pres = gst_h265_parser_parse_pps (parser, &nalu, &pps);
          if (pres != GST_H265_PARSER_OK) {
            GST_WARNING_OBJECT (self, "Failed to parse PPS");
            return FALSE;
          }

          gst_niquadrah265dec_store_nal (self, pps.id,
              (GstH265NalUnitType) nalu.type, &nalu);
          break;
        default:
          break;
      }

      /* update bit reader position to after the parsed NAL unit */
      if (!gst_bit_reader_set_pos (&br, (nalu.offset + nalu.size) * 8))
        goto too_small;
    }
  }

  return TRUE;

too_small:
  GST_WARNING_OBJECT (self, "hvcC too small");
  return FALSE;
}

static gboolean
gst_niquadrah265dec_parse_format (GstNiquadraDec * decoder,
    GstVideoCodecState * state)
{
  GstNiquadraH265Dec *self = GST_NIQUADRAH265DEC (decoder);
  GstStructure *s;
  const gchar *str;
  GstMapInfo map;

  gst_niquadrah265dec_clear_hvcc (self);
  self->packetized = FALSE;

  s = gst_caps_get_structure (state->caps, 0);
  str = gst_structure_get_string (s, "stream-format");
  if ((g_strcmp0 (str, "hvc1") == 0 || g_strcmp0 (str, "hev1") == 0) &&
      state->codec_data) {
    self->packetized = TRUE;
    self->nal_length_size = 4;
  } else
    return TRUE;

  if (!gst_buffer_map (state->codec_data, &map, GST_MAP_READ)) {
    GST_ERROR_OBJECT (self, "Failed to map codec data");
    return FALSE;
  }

  if (!gst_niquadrah265dec_parse_hvcc (self, map.data, map.size)) {
    GST_ERROR_OBJECT (self, "Failed to parse codec data");
    gst_buffer_unmap (state->codec_data, &map);
    return FALSE;
  }

  gst_buffer_unmap (state->codec_data, &map);

  return TRUE;
}

static gboolean
gst_niquadrah265dec_append_nalu_annexb (GstBuffer * buf,
    const GstH265NalUnit * nalu)
{
  static const guint8 start_code[] = { 0, 0, 1 };
  gsize size = sizeof (start_code) + nalu->size;
  guint8 *data = g_malloc (size);

  if (G_UNLIKELY (!data))
    return FALSE;

  memcpy (data, start_code, sizeof (start_code));
  memcpy (data + sizeof (start_code), nalu->data + nalu->offset, nalu->size);

  GstMemory *mem = gst_memory_new_wrapped (0, data, size, 0, size,
      data, (GDestroyNotify) g_free);

  if (G_UNLIKELY (!mem)) {
    g_free (data);
    return FALSE;
  }

  gst_buffer_append_memory (buf, mem);
  return TRUE;
}

static void
gst_niquadrah265dec_handle_parameter_set (GstNiquadraH265Dec * self,
    GstH265NalUnit * nalu)
{
  GstH265ParserResult pres;

  switch (nalu->type) {
    case GST_H265_NAL_VPS:{
      GstH265VPS vps;
      pres = gst_h265_parser_parse_vps (self->parser, nalu, &vps);
      if (pres == GST_H265_PARSER_OK)
        gst_niquadrah265dec_store_nal (self, vps.id, GST_H265_NAL_VPS, nalu);
      break;
    }
    case GST_H265_NAL_SPS:{
      GstH265SPS sps;
      pres = gst_h265_parser_parse_sps (self->parser, nalu, &sps, FALSE);
      if (pres == GST_H265_PARSER_OK)
        gst_niquadrah265dec_store_nal (self, sps.id, GST_H265_NAL_SPS, nalu);
      break;
    }
    case GST_H265_NAL_PPS:{
      GstH265PPS pps;
      pres = gst_h265_parser_parse_pps (self->parser, nalu, &pps);
      if (pres == GST_H265_PARSER_OK)
        gst_niquadrah265dec_store_nal (self, pps.id, GST_H265_NAL_PPS, nalu);
      break;
    }
    default:
      break;
  }
}

static GstBuffer *
gst_niquadrah265dec_build_prefix_buffer (GstNiquadraH265Dec * self)
{
  GstBuffer *prefix = gst_buffer_new ();
  guint i;

  if (G_UNLIKELY (!prefix))
    return NULL;

  for (i = 0; i < GST_H265_MAX_VPS_COUNT; i++) {
    if (self->vps_nals[i])
      prefix = gst_buffer_append (prefix, gst_buffer_ref (self->vps_nals[i]));
  }

  for (i = 0; i < GST_H265_MAX_SPS_COUNT; i++) {
    if (self->sps_nals[i])
      prefix = gst_buffer_append (prefix, gst_buffer_ref (self->sps_nals[i]));
  }

  for (i = 0; i < GST_H265_MAX_PPS_COUNT; i++) {
    if (self->pps_nals[i])
      prefix = gst_buffer_append (prefix, gst_buffer_ref (self->pps_nals[i]));
  }

  return prefix;
}

static GstBuffer *
gst_niquadrah265dec_process_buffer (GstNiquadraDec * decoder,
    GstBuffer * buffer)
{
  GstNiquadraH265Dec *self = GST_NIQUADRAH265DEC (decoder);
  GstMapInfo map = { 0 };
  GstBuffer *new_buf = NULL;
  GstBuffer *prefix_buf = NULL;
  GstH265NalUnit nalu = { 0 };
  GstH265ParserResult pres;

  g_return_val_if_fail (buffer != NULL, NULL);

  if (!self->packetized)
    return gst_buffer_ref (buffer);

  if (!gst_buffer_map (buffer, &map, GST_MAP_READ)) {
    GST_ERROR_OBJECT (self, "Failed to map input buffer");
    return NULL;
  }

  new_buf = gst_buffer_new ();
  if (G_UNLIKELY (!new_buf)) {
    GST_ERROR_OBJECT (self, "Failed to allocate output buffer");
    goto error;
  }

  do {
    pres = gst_h265_parser_identify_nalu_hevc (self->parser, map.data,
        nalu.offset + nalu.size, map.size, self->nal_length_size, &nalu);

    if (pres == GST_H265_PARSER_NO_NAL_END)
      pres = GST_H265_PARSER_OK;

    if (pres != GST_H265_PARSER_OK)
      break;

    gst_niquadrah265dec_handle_parameter_set (self, &nalu);

    if (!gst_niquadrah265dec_append_nalu_annexb (new_buf, &nalu)) {
      GST_ERROR_OBJECT (self, "Failed to append NAL unit");
      goto error;
    }
  } while (pres == GST_H265_PARSER_OK);

  gst_buffer_unmap (buffer, &map);
  map.data = NULL;

  if (!self->hvcc_sent) {
    self->hvcc_sent = TRUE;

    prefix_buf = gst_niquadrah265dec_build_prefix_buffer (self);
    if (G_UNLIKELY (!prefix_buf))
      goto error;

    new_buf = gst_buffer_append (prefix_buf, new_buf);
  }

  gst_buffer_copy_into (new_buf, buffer, GST_BUFFER_COPY_TIMESTAMPS, 0, -1);

  return new_buf;

error:
  if (map.data)
    gst_buffer_unmap (buffer, &map);
  gst_clear_buffer (&new_buf);
  gst_clear_buffer (&prefix_buf);
  return NULL;
}

static gboolean
gst_niquadrah265dec_prepare (GstNiquadraDec * decoder)
{
  GstNiquadraH265Dec *h265dec = GST_NIQUADRAH265DEC (decoder);

  h265dec->parser = gst_h265_parser_new ();
  if (!h265dec->parser) {
    GST_ERROR_OBJECT (h265dec, "Failed to create Quadra H265 parser");
    return FALSE;
  }

  gst_niquadrah265dec_clear_hvcc (h265dec);

  return TRUE;
}


static gboolean
gst_niquadrah265dec_release (GstNiquadraDec * decoder)
{
  GstNiquadraH265Dec *h265dec = GST_NIQUADRAH265DEC (decoder);

  if (h265dec->parser) {
    gst_h265_parser_free (h265dec->parser);
    h265dec->parser = NULL;
  }

  gst_niquadrah265dec_clear_hvcc (h265dec);

  return TRUE;
}

static gboolean
gst_niquadrah265dec_configure (GstNiquadraDec * decoder)
{
  GstNiquadraH265Dec *h265dec = GST_NIQUADRAH265DEC (decoder);
  decoder->enable_user_data_sei_passthru = h265dec->user_data_sei_passthru;
  decoder->custom_sei_type = h265dec->custom_sei_passthru;
  decoder->low_delay = h265dec->low_delay;
  decoder->codec_format = NI_CODEC_FORMAT_H265;
  return TRUE;
}

static void
gst_niquadradec_h265_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiquadraH265Dec *thiz = GST_NIQUADRAH265DEC (object);

  if (gst_niquadradec_set_common_property (object, prop_id, value, pspec))
    return;

  GST_OBJECT_LOCK (thiz);
  switch (prop_id) {
    case PROP_USER_DATA_SEI_PASSTHRU:
      thiz->user_data_sei_passthru = g_value_get_boolean (value);
      break;
    case PROP_CUSTOM_SEI_PASSTHRU:
      thiz->custom_sei_passthru = g_value_get_int (value);
      break;
    case PROP_LOW_DELAY:
      thiz->low_delay = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (thiz);
}

static void
gst_niquadradec_h265_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiquadraH265Dec *thiz = GST_NIQUADRAH265DEC (object);

  if (gst_niquadradec_get_common_property (object, prop_id, value, pspec))
    return;
  GST_OBJECT_LOCK (thiz);
  switch (prop_id) {
    case PROP_USER_DATA_SEI_PASSTHRU:
      g_value_set_boolean (value, thiz->user_data_sei_passthru);
      break;
    case PROP_CUSTOM_SEI_PASSTHRU:
      g_value_set_uint (value, thiz->custom_sei_passthru);
      break;
    case PROP_LOW_DELAY:
      g_value_set_boolean (value, thiz->low_delay);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (thiz);
}

static void
gst_niquadrah265dec_class_init (GstNiquadraH265DecClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstNiquadraDecClass *decoder_class;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  decoder_class = GST_NIQUADRADEC_CLASS (klass);

  gobject_class->set_property = gst_niquadradec_h265_set_property;
  gobject_class->get_property = gst_niquadradec_h265_get_property;

  decoder_class->configure = GST_DEBUG_FUNCPTR (gst_niquadrah265dec_configure);
  decoder_class->parse_format =
      GST_DEBUG_FUNCPTR (gst_niquadrah265dec_parse_format);
  decoder_class->process_buffer =
      GST_DEBUG_FUNCPTR (gst_niquadrah265dec_process_buffer);
  decoder_class->prepare = GST_DEBUG_FUNCPTR (gst_niquadrah265dec_prepare);
  decoder_class->release = GST_DEBUG_FUNCPTR (gst_niquadrah265dec_release);

  gst_niquadradec_install_common_properties (decoder_class);

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

  g_object_class_install_property (gobject_class, PROP_LOW_DELAY,
      g_param_spec_boolean ("low-delay",
          "LOW-DELAY",
          "Enable low delay decoding mode for 1 in, 1 out decoding sequence. set 1 \"\n"
          "     \"to enable low delay mode. Should be used only for streams that are in \"\n"
          "     \"sequence",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata (element_class,
      "NETINT Quadra H265 decoder",
      "Codec/Decoder/Video/Hardware",
      "H265 video decoder based on NetInt libxcoder SDK",
      "Simon Shi <simon.shi@netint.cn>");

  gst_element_class_add_static_pad_template (element_class, &sink_factory);
  gst_element_class_add_static_pad_template (element_class, &src_factory);
}

static void
gst_niquadrah265dec_init (GstNiquadraH265Dec * thiz)
{
  guint i;

  thiz->hvcc_sent = FALSE;
  thiz->user_data_sei_passthru = FALSE;
  thiz->custom_sei_passthru = -1;
  thiz->low_delay = FALSE;

  thiz->parser = NULL;
  thiz->packetized = FALSE;
  thiz->nal_length_size = 0;

  for (i = 0; i < GST_H265_MAX_VPS_COUNT; i++) {
    thiz->vps_nals[i] = NULL;
  }

  for (i = 0; i < GST_H265_MAX_SPS_COUNT; i++) {
    thiz->sps_nals[i] = NULL;
  }

  for (i = 0; i < GST_H265_MAX_PPS_COUNT; i++) {
    thiz->pps_nals[i] = NULL;
  }
}

static gboolean
niquadrah265dec_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadrah265dec_debug, "niquadrah265dec", 0,
      "niquadradech265");

  return gst_element_register (plugin, "niquadrah265dec", GST_RANK_NONE,
      GST_TYPE_NIQUADRAH265DEC);
}
