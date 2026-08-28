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
 *  \file   gstniquadrah264dec.c
 *
 *  \brief  Implement of NetInt Quadra h264 decoder.
 ******************************************************************************/

#define GST_USE_UNSTABLE_API
#include "gstniquadrah264dec.h"

GST_DEBUG_CATEGORY_STATIC (gst_niquadrah264dec_debug);
#define GST_CAT_DEFAULT gst_niquadrah264dec_debug

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264, "
        "width = (int) [ 144, 8192 ], height = (int) [ 144, 8192 ], "
        "stream-format = (string) {byte-stream, avc}, alignment = (string) au")
    );

#define SUPPORTED_FORMATS "{ I420, NV12, I420_10LE, P010_10LE }"
static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (SUPPORTED_FORMATS) ";"
        GST_VIDEO_CAPS_MAKE_WITH_FEATURES ("memory:NiQuadraMemory",
            SUPPORTED_FORMATS))
    );

static gboolean niquadrah264dec_element_init (GstPlugin * plugin);

#define gst_niquadrah264dec_parent_class parent_class

G_DEFINE_TYPE (GstNiquadraH264Dec, gst_niquadrah264dec, GST_TYPE_NIQUADRADEC);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadrah264dec,
    niquadrah264dec_element_init);

enum
{
  PROP_USER_DATA_SEI_PASSTHRU = GST_NIQUADRA_DEC_PROP_MAX,
  PROP_CUSTOM_SEI_PASSTHRU,
  PROP_LOW_DELAY,
};

#define PROP_CUSTOM_SEI_PASSTHRU_DEFAULT -1
#define USER_DATA_UNREGISTERED_SEI_PAYLOAD_TYPE 5

static gboolean gst_niquadrah264dec_parse_format (GstNiquadraDec * decoder,
    GstVideoCodecState * state);
static GstBuffer *gst_niquadrah264dec_process_buffer (GstNiquadraDec * decoder,
    GstBuffer * buffer);

static gboolean gst_niquadrah264dec_prepare (GstNiquadraDec * decoder);
static gboolean gst_niquadrah264dec_release (GstNiquadraDec * decoder);

static gboolean gst_niquadrah264dec_parse_pps_array (GstNiquadraH264Dec * self,
    GArray * pps_array);
static gboolean gst_niquadrah264dec_parse_sps_array (GstNiquadraH264Dec * self,
    GArray * sps_array);

static GstBuffer *gst_niquadrah264dec_build_prefix_buffer (GstNiquadraH264Dec *
    self);
static gboolean gst_niquadrah264dec_parse_and_convert_nalus (GstNiquadraH264Dec
    * self, GstMapInfo * map, GstBuffer * out_buf, const guint8 * start_code,
    gsize start_code_size, gboolean * have_sps, gboolean * have_pps);
static void gst_niquadrah264dec_handle_parameter_set (GstNiquadraH264Dec * self,
    GstH264NalUnit * nalu, gboolean * have_sps, gboolean * have_pps);
static gboolean gst_niquadrah264dec_append_nalu_annexb (GstBuffer * buf,
    const GstH264NalUnit * nalu, const guint8 * start_code,
    gsize start_code_size);

static gboolean
gst_niquadrah264dec_configure (GstNiquadraDec * decoder)
{
  GstNiquadraH264Dec *h264dec = GST_NIQUADRAH264DEC (decoder);
  ni_xcoder_params_t *p_param = NULL;
  ni_decoder_input_params_t *p_dec_input_param = NULL;

  decoder->custom_sei_type = h264dec->custom_sei_passthru;
  decoder->low_delay = h264dec->low_delay;
  decoder->codec_format = NI_CODEC_FORMAT_H264;

  if (decoder->context) {
    p_param = gst_niquadra_context_get_xcoder_param (decoder->context);
    if (p_param) {
      p_dec_input_param = &(p_param->dec_input_params);
    }
  }

  if (decoder->custom_sei_type == USER_DATA_UNREGISTERED_SEI_PAYLOAD_TYPE ||
      (p_dec_input_param && p_dec_input_param->custom_sei_passthru ==
          USER_DATA_UNREGISTERED_SEI_PAYLOAD_TYPE)) {
    decoder->enable_user_data_sei_passthru = 0;
    if (p_dec_input_param) {
      p_dec_input_param->enable_user_data_sei_passthru = 0;
    }
  } else {
    decoder->enable_user_data_sei_passthru = h264dec->user_data_sei_passthru;
  }

  return TRUE;
}

static void
gst_niquadradec_h264_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiquadraH264Dec *thiz = GST_NIQUADRAH264DEC (object);

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
gst_niquadradec_h264_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiquadraH264Dec *thiz = GST_NIQUADRAH264DEC (object);

  if (gst_niquadradec_get_common_property (object, prop_id, value, pspec))
    return;

  GST_OBJECT_LOCK (thiz);
  switch (prop_id) {
    case PROP_USER_DATA_SEI_PASSTHRU:
      g_value_set_boolean (value, thiz->user_data_sei_passthru);
      break;
    case PROP_CUSTOM_SEI_PASSTHRU:
      g_value_set_int (value, thiz->custom_sei_passthru);
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
gst_niquadrah264dec_store_nal (GstNiquadraH264Dec * self, guint id,
    GstH264NalUnitType nal_type, GstH264NalUnit * nalu)
{
  GstBuffer *buf, **store;
  guint size = nalu->size, store_size;
  static const guint8 start_code[] = { 0, 0, 1 };

  if (nal_type == GST_H264_NAL_SPS || nal_type == GST_H264_NAL_SUBSET_SPS) {
    store_size = GST_H264_MAX_SPS_COUNT;
    store = self->sps_nals;
    GST_DEBUG_OBJECT (self, "storing sps %u", id);
  } else if (nal_type == GST_H264_NAL_PPS) {
    store_size = GST_H264_MAX_PPS_COUNT;
    store = self->pps_nals;
    GST_DEBUG_OBJECT (self, "storing pps %u", id);
  } else {
    return;
  }

  if (id >= store_size) {
    GST_DEBUG_OBJECT (self, "unable to store nal, id out-of-range %d", id);
    return;
  }

  buf = gst_buffer_new_allocate (NULL, size + sizeof (start_code), NULL);
  gst_buffer_fill (buf, 0, start_code, sizeof (start_code));
  gst_buffer_fill (buf, sizeof (start_code), nalu->data + nalu->offset, size);

  if (store[id])
    gst_buffer_unref (store[id]);

  store[id] = buf;
}

static void
gst_niquadrah264dec_clear_codec_data (GstNiquadraH264Dec * self)
{
  guint i;

  for (i = 0; i < GST_H264_MAX_SPS_COUNT; i++) {
    if (self->sps_nals[i]) {
      gst_buffer_unref (self->sps_nals[i]);
      self->sps_nals[i] = NULL;
    }
  }

  for (i = 0; i < GST_H264_MAX_PPS_COUNT; i++) {
    if (self->pps_nals[i]) {
      gst_buffer_unref (self->pps_nals[i]);
      self->pps_nals[i] = NULL;
    }
  }
}

static gboolean
gst_niquadrah264dec_parse_avcc (GstNiquadraH264Dec * self,
    const guint8 * data, gsize size)
{
  GstH264DecoderConfigRecord *config = NULL;
  gboolean ret = FALSE;

  g_return_val_if_fail (self != NULL && self->parser != NULL, FALSE);
  g_return_val_if_fail (data != NULL && size > 0, FALSE);

  if (gst_h264_parser_parse_decoder_config_record (self->parser, data, size,
          &config) != GST_H264_PARSER_OK) {
    GST_WARNING_OBJECT (self, "Failed to parse codec-data");
    return FALSE;
  }

  self->nal_length_size = config->length_size_minus_one + 1;

  if (!gst_niquadrah264dec_parse_sps_array (self, config->sps))
    goto out;

  if (!gst_niquadrah264dec_parse_pps_array (self, config->pps))
    goto out;

  ret = TRUE;

out:
  gst_h264_decoder_config_record_free (config);
  return ret;
}

static gboolean
gst_niquadrah264dec_parse_sps_array (GstNiquadraH264Dec * self,
    GArray * sps_array)
{
  for (guint i = 0; i < sps_array->len; i++) {
    GstH264NalUnit *nalu = &g_array_index (sps_array, GstH264NalUnit, i);
    GstH264SPS sps;
    GstH264ParserResult pres;

    switch (nalu->type) {
      case GST_H264_NAL_SPS:
        pres = gst_h264_parser_parse_sps (self->parser, nalu, &sps);
        break;
      case GST_H264_NAL_SUBSET_SPS:
        pres = gst_h264_parser_parse_subset_sps (self->parser, nalu, &sps);
        break;
      default:
        continue;
    }

    if (pres != GST_H264_PARSER_OK) {
      GST_WARNING_OBJECT (self, "Failed to parse SPS (index=%u, type=%d)", i,
          nalu->type);
      return FALSE;
    }

    gst_niquadrah264dec_store_nal (self, sps.id,
        (GstH264NalUnitType) nalu->type, nalu);
    gst_h264_sps_clear (&sps);
  }
  return TRUE;
}

static gboolean
gst_niquadrah264dec_parse_pps_array (GstNiquadraH264Dec * self,
    GArray * pps_array)
{
  for (guint i = 0; i < pps_array->len; i++) {
    GstH264NalUnit *nalu = &g_array_index (pps_array, GstH264NalUnit, i);
    GstH264PPS pps;

    if (nalu->type != GST_H264_NAL_PPS)
      continue;

    if (gst_h264_parser_parse_pps (self->parser, nalu,
            &pps) != GST_H264_PARSER_OK) {
      GST_WARNING_OBJECT (self, "Failed to parse PPS (index=%u)", i);
      return FALSE;
    }

    gst_niquadrah264dec_store_nal (self, pps.id, GST_H264_NAL_PPS, nalu);
    gst_h264_pps_clear (&pps);
  }
  return TRUE;
}


static gboolean
gst_niquadrah264dec_parse_format (GstNiquadraDec * decoder,
    GstVideoCodecState * state)
{
  GstNiquadraH264Dec *self = GST_NIQUADRAH264DEC (decoder);
  GstStructure *s;
  const gchar *str;
  GstMapInfo map;

  gst_niquadrah264dec_clear_codec_data (self);
  self->packetized = FALSE;

  s = gst_caps_get_structure (state->caps, 0);
  str = gst_structure_get_string (s, "stream-format");
  if ((g_strcmp0 (str, "avc") == 0 || g_strcmp0 (str, "avc3") == 0) &&
      state->codec_data) {
    self->packetized = TRUE;
    self->nal_length_size = 4;
  }

  if (!self->packetized)
    return TRUE;

  if (!gst_buffer_map (state->codec_data, &map, GST_MAP_READ)) {
    GST_ERROR_OBJECT (self, "Failed to map codec data");
    return FALSE;
  }

  if (!gst_niquadrah264dec_parse_avcc (self, map.data, map.size)) {
    GST_ERROR_OBJECT (self, "Failed to parse codec data");
    gst_buffer_unmap (state->codec_data, &map);
    return FALSE;
  }

  gst_buffer_unmap (state->codec_data, &map);

  return TRUE;
}

static GstBuffer *
gst_niquadrah264dec_process_buffer (GstNiquadraDec * decoder,
    GstBuffer * buffer)
{
  GstNiquadraH264Dec *self = GST_NIQUADRAH264DEC (decoder);
  GstMapInfo map;
  GstBuffer *new_buf = NULL;
  GstBuffer *prefix_buf = NULL;
  gboolean have_sps = FALSE;
  gboolean have_pps = FALSE;
  static const guint8 start_code[] = { 0, 0, 1 };

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

  if (!gst_niquadrah264dec_parse_and_convert_nalus (self, &map, new_buf,
          start_code, sizeof (start_code), &have_sps, &have_pps)) {
    goto error;
  }

  gst_buffer_unmap (buffer, &map);
  map.data = NULL;

  if (!self->avcc_sent) {
    self->avcc_sent = TRUE;

    prefix_buf = gst_niquadrah264dec_build_prefix_buffer (self);
    if (!prefix_buf)
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
gst_niquadrah264dec_parse_and_convert_nalus (GstNiquadraH264Dec * self,
    GstMapInfo * map, GstBuffer * out_buf, const guint8 * start_code,
    gsize start_code_size, gboolean * have_sps, gboolean * have_pps)
{
  GstH264NalParser *parser = self->parser;
  GstH264NalUnit nalu = { 0 };
  GstH264ParserResult pres;

  do {
    pres = gst_h264_parser_identify_nalu_avc (parser, map->data,
        nalu.offset + nalu.size, map->size, self->nal_length_size, &nalu);

    if (pres != GST_H264_PARSER_OK && pres != GST_H264_PARSER_NO_NAL_END)
      break;

    gst_niquadrah264dec_handle_parameter_set (self, &nalu, have_sps, have_pps);

    if (!gst_niquadrah264dec_append_nalu_annexb (out_buf, &nalu,
            start_code, start_code_size)) {
      GST_ERROR_OBJECT (self, "Failed to append NAL unit");
      return FALSE;
    }
  } while (pres == GST_H264_PARSER_OK);

  return TRUE;
}

static void
gst_niquadrah264dec_handle_parameter_set (GstNiquadraH264Dec * self,
    GstH264NalUnit * nalu, gboolean * have_sps, gboolean * have_pps)
{
  GstH264ParserResult pres;

  switch (nalu->type) {
    case GST_H264_NAL_SPS:
    case GST_H264_NAL_SUBSET_SPS:{
      GstH264SPS sps;

      if (nalu->type == GST_H264_NAL_SPS)
        pres = gst_h264_parser_parse_sps (self->parser, nalu, &sps);
      else
        pres = gst_h264_parser_parse_subset_sps (self->parser, nalu, &sps);

      if (pres == GST_H264_PARSER_OK) {
        *have_sps = TRUE;
        gst_niquadrah264dec_store_nal (self, sps.id,
            (GstH264NalUnitType) nalu->type, nalu);
        gst_h264_sps_clear (&sps);
      }
      break;
    }
    case GST_H264_NAL_PPS:{
      GstH264PPS pps;

      pres = gst_h264_parser_parse_pps (self->parser, nalu, &pps);
      if (pres == GST_H264_PARSER_OK) {
        *have_pps = TRUE;
        gst_niquadrah264dec_store_nal (self, pps.id, GST_H264_NAL_PPS, nalu);
        gst_h264_pps_clear (&pps);
      }
      break;
    }
    default:
      break;
  }
}

static gboolean
gst_niquadrah264dec_append_nalu_annexb (GstBuffer * buf,
    const GstH264NalUnit * nalu, const guint8 * start_code,
    gsize start_code_size)
{
  gsize size = start_code_size + nalu->size;
  guint8 *data = g_malloc (size);

  if (G_UNLIKELY (!data))
    return FALSE;

  memcpy (data, start_code, start_code_size);
  memcpy (data + start_code_size, nalu->data + nalu->offset, nalu->size);

  GstMemory *mem = gst_memory_new_wrapped (0, data, size, 0, size,
      data, (GDestroyNotify) g_free);

  if (G_UNLIKELY (!mem)) {
    g_free (data);
    return FALSE;
  }

  gst_buffer_append_memory (buf, mem);
  return TRUE;
}

static GstBuffer *
gst_niquadrah264dec_build_prefix_buffer (GstNiquadraH264Dec * self)
{
  GstBuffer *prefix_buf = gst_buffer_new ();

  if (G_UNLIKELY (!prefix_buf)) {
    GST_ERROR_OBJECT (self, "Failed to allocate prefix buffer");
    return NULL;
  }

  for (guint i = 0; i < GST_H264_MAX_SPS_COUNT; i++) {
    if (self->sps_nals[i])
      prefix_buf = gst_buffer_append (prefix_buf,
          gst_buffer_ref (self->sps_nals[i]));
  }

  for (guint i = 0; i < GST_H264_MAX_PPS_COUNT; i++) {
    if (self->pps_nals[i])
      prefix_buf = gst_buffer_append (prefix_buf,
          gst_buffer_ref (self->pps_nals[i]));
  }

  return prefix_buf;
}

static gboolean
gst_niquadrah264dec_prepare (GstNiquadraDec * decoder)
{
  GstNiquadraH264Dec *h264dec = GST_NIQUADRAH264DEC (decoder);

  h264dec->parser = gst_h264_nal_parser_new ();
  if (!h264dec->parser) {
    GST_ERROR_OBJECT (h264dec, "Failed to create Quadra H264 parser");
    return FALSE;
  }

  gst_niquadrah264dec_clear_codec_data (h264dec);

  return TRUE;
}

static gboolean
gst_niquadrah264dec_release (GstNiquadraDec * decoder)
{
  GstNiquadraH264Dec *h264dec = GST_NIQUADRAH264DEC (decoder);

  if (h264dec->parser) {
    gst_h264_nal_parser_free (h264dec->parser);
    h264dec->parser = NULL;
  }

  gst_niquadrah264dec_clear_codec_data (h264dec);

  return TRUE;
}

static void
gst_niquadrah264dec_class_init (GstNiquadraH264DecClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstNiquadraDecClass *decoder_class;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  decoder_class = GST_NIQUADRADEC_CLASS (klass);

  gobject_class->set_property = gst_niquadradec_h264_set_property;
  gobject_class->get_property = gst_niquadradec_h264_get_property;

  decoder_class->configure = GST_DEBUG_FUNCPTR (gst_niquadrah264dec_configure);
  decoder_class->parse_format =
      GST_DEBUG_FUNCPTR (gst_niquadrah264dec_parse_format);
  decoder_class->process_buffer =
      GST_DEBUG_FUNCPTR (gst_niquadrah264dec_process_buffer);
  decoder_class->prepare = GST_DEBUG_FUNCPTR (gst_niquadrah264dec_prepare);
  decoder_class->release = GST_DEBUG_FUNCPTR (gst_niquadrah264dec_release);

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
      "NETINT Quadra H264 decoder",
      "Codec/Decoder/Video/Hardware",
      "H264 video encoder based on NetInt libxcoder SDK",
      "Simon Shi <simon.shi@netint.cn>");

  gst_element_class_add_static_pad_template (element_class, &sink_factory);
  gst_element_class_add_static_pad_template (element_class, &src_factory);
}

static void
gst_niquadrah264dec_init (GstNiquadraH264Dec * thiz)
{
  thiz->avcc_sent = FALSE;
  thiz->user_data_sei_passthru = FALSE;
  thiz->custom_sei_passthru = -1;
  thiz->low_delay = FALSE;
}

static gboolean
niquadrah264dec_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadrah264dec_debug, "niquadrah264dec", 0,
      "niquadradech264");

  return gst_element_register (plugin, "niquadrah264dec", GST_RANK_NONE,
      GST_TYPE_NIQUADRAH264DEC);
}
