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
 *  \file   GstNiQuadraCrop.c
 *
 *  \brief  Implement of NetInt Quadra hardware crop filter.
 ******************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/video.h>
#include "niquadra.h"
#include "ni_device_api.h"
#include "gstniquadramemory.h"
#include "gstniquadrautils.h"
#include "gstniquadrabaseconvert.h"

GST_DEBUG_CATEGORY_STATIC (gst_niquadracrop_debug);
#define GST_CAT_DEFAULT gst_niquadracrop_debug

enum
{
  PROP_0,
  PROP_X,
  PROP_Y,
  PROP_P2P,
  PROP_AUTO_SKIP,
  PROP_LAST
};

typedef struct _GstNiQuadraCrop
{
  GstNiQuadraBaseConvert parent;

  int x;                        // x offset of the non-cropped area with respect to the input area
  int y;                        // y offset of the non-cropped area with respect to the input area

  ni_session_data_io_t api_dst_frame;
  ni_frame_config_t output_frame_config;

  uint8_t shift;
  bool is_p2p;

  gboolean auto_skip;
  gboolean skip_filter;
} GstNiQuadraCrop;

typedef struct _GstNiQuadraCropClass
{
  GstNiQuadraBaseConvertClass parent_class;
} GstNiQuadraCropClass;

#define GST_TYPE_NIQUADRACROP \
  (gst_niquadracrop_get_type())
#define GST_NIQUADRACROP(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRACROP,GstNiQuadraCrop))
#define GST_NIQUADRACROP_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRACROP,GstNiQuadraCrop))
#define GST_IS_NIQUADRACROP(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRACROP))
#define GST_IS_NIQUADRACROP_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRACROP))

static gboolean niquadracrop_element_init (GstPlugin * plugin);

#define gst_niquadracrop_parent_class parent_class

GType gst_niquadracropniquadra_get_type (void);

G_DEFINE_TYPE (GstNiQuadraCrop, gst_niquadracrop,
    GST_TYPE_NI_QUADRA_BASE_CONVERT);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadracrop, niquadracrop_element_init);

#define SUPPORTED_FORMATS \
    "{ I420, YUY2, UYVY, NV12, ARGB, RGBA, ABGR, BGRA, I420_10LE, P010_10LE, " \
    "NV16, BGRx, NV12_10LE32 }"

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY, SUPPORTED_FORMATS))
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY, SUPPORTED_FORMATS))
    );

static gboolean
gst_niquadracrop_get_explicit_fixed_par_from_caps (GstCaps * caps, gint * par_n,
    gint * par_d)
{
  const GstStructure *s;
  const GValue *par;

  if (!caps || gst_caps_get_size (caps) < 1) {
    return FALSE;
  }

  s = gst_caps_get_structure (caps, 0);
  par = gst_structure_get_value (s, "pixel-aspect-ratio");
  if (!par || !gst_value_is_fixed (par)) {
    return FALSE;
  }

  if (par_n) {
    *par_n = gst_value_get_fraction_numerator (par);
  }
  if (par_d) {
    *par_d = gst_value_get_fraction_denominator (par);
  }
  return TRUE;
}

static void
gst_niquadracrop_get_normalized_par_from_caps (GstCaps * caps, gint * par_n,
    gint * par_d)
{
  *par_n = 1;
  *par_d = 1;

  gst_niquadracrop_get_explicit_fixed_par_from_caps (caps, par_n, par_d);
}

static gboolean
gst_niquadracrop_pars_are_equal (gint par_a_n, gint par_a_d, gint par_b_n,
    gint par_b_d)
{
  return gst_util_fraction_compare (par_a_n, par_a_d, par_b_n, par_b_d) == 0;
}

static gboolean
gst_niquadracrop_othercaps_has_conflicting_fixed_par (GstCaps * caps,
    GstCaps * othercaps)
{
  gint caps_par_n, caps_par_d;
  gint other_par_n, other_par_d;

  if (!gst_niquadracrop_get_explicit_fixed_par_from_caps (caps, &caps_par_n,
          &caps_par_d)) {
    return FALSE;
  }

  if (!gst_niquadracrop_get_explicit_fixed_par_from_caps (othercaps,
          &other_par_n, &other_par_d)) {
    return FALSE;
  }

  return !gst_niquadracrop_pars_are_equal (caps_par_n, caps_par_d,
      other_par_n, other_par_d);
}

static gboolean
gst_niquadracrop_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstNiQuadraCrop *self = GST_NIQUADRACROP (trans);
  GstNiQuadraBaseConvert *parent = GST_NI_QUADRA_BASE_CONVERT (trans);
  gint in_par_n, in_par_d, out_par_n, out_par_d;

  gst_niquadracrop_get_normalized_par_from_caps (incaps, &in_par_n, &in_par_d);
  gst_niquadracrop_get_normalized_par_from_caps (outcaps, &out_par_n,
      &out_par_d);

  if (!gst_niquadracrop_pars_are_equal (in_par_n, in_par_d, out_par_n,
          out_par_d)) {
    GST_ERROR_OBJECT (trans,
        "niquadracrop requires identical input/output pixel-aspect-ratio, "
        "got %d/%d -> %d/%d", in_par_n, in_par_d, out_par_n, out_par_d);
    return FALSE;
  }

  if (!GST_BASE_TRANSFORM_CLASS (gst_niquadracrop_parent_class)->set_caps
      (trans, incaps, outcaps)) {
    return FALSE;
  }

  if (self->auto_skip && !self->is_p2p && (self->x <= 0 && self->y <= 0)
      && gst_video_info_is_equal (&parent->in_info, &parent->out_info)) {
    self->skip_filter = TRUE;
  } else {
    self->skip_filter = FALSE;
  }

  return TRUE;
}

static GstCaps *
gst_niquadracrop_fixate_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GstNiQuadraCrop *self = GST_NIQUADRACROP (base);
  GstVideoInfo info;
  GstVideoFormat in_pixfmt;
  gint caps_par_n = 0, caps_par_d = 0;
  gboolean caps_has_explicit_fixed_par;
  gboolean othercaps_has_explicit_fixed_par;

  GST_DEBUG_OBJECT (self,
      "trying to fixate othercaps %" GST_PTR_FORMAT " based on caps %"
      GST_PTR_FORMAT, othercaps, caps);

  othercaps = remove_structures_from_caps (othercaps, "video/x-raw", 1);

  if (direction == GST_PAD_SINK) {
    GstStructure *outs;
    gint w = 0, h = 0;
    gboolean is_downstream_configured = FALSE;

    if (!gst_video_info_from_caps (&info, caps)) {
      return NULL;
    }

    in_pixfmt = info.finfo->format;
    if (in_pixfmt == GST_VIDEO_FORMAT_I420
        || in_pixfmt == GST_VIDEO_FORMAT_NV12
        || in_pixfmt == GST_VIDEO_FORMAT_I420_10LE
        || in_pixfmt == GST_VIDEO_FORMAT_P010_10LE) {
      self->shift = 1;
    } else {
      self->shift = 0;
    }

    outs = gst_caps_get_structure (othercaps, 0);
    if ((gst_structure_get_int (outs, "width", &w)) ||
        (gst_structure_get_int (outs, "height", &h))) {
      is_downstream_configured = TRUE;
    }

    if (!is_downstream_configured) {
      GST_WARNING_OBJECT (self, "downstream must configure dimension");
    }
  }

  caps_has_explicit_fixed_par =
      gst_niquadracrop_get_explicit_fixed_par_from_caps (caps, &caps_par_n,
      &caps_par_d);
  othercaps_has_explicit_fixed_par =
      gst_niquadracrop_get_explicit_fixed_par_from_caps (othercaps, NULL, NULL);

  if (gst_niquadracrop_othercaps_has_conflicting_fixed_par (caps, othercaps)) {
    const GstStructure *other_s = gst_caps_get_structure (othercaps, 0);
    const GValue *other_par =
        gst_structure_get_value (other_s, "pixel-aspect-ratio");

    GST_DEBUG_OBJECT (self,
        "rejecting conflicting fixed PAR negotiation: %d/%d vs %d/%d",
        caps_par_n, caps_par_d,
        gst_value_get_fraction_numerator (other_par),
        gst_value_get_fraction_denominator (other_par));
    gst_caps_unref (othercaps);
    return gst_caps_new_empty ();
  }

  othercaps = GST_NI_QUADRA_BASE_CONVERT_CLASS
      (parent_class)->fixate_size (base, direction, caps, othercaps);
  if (gst_caps_get_size (othercaps) == 1) {
    gint i;
    const gchar *format_fields[] = { "format", "colorimetry", "chroma-site" };
    GstStructure *format_struct = gst_caps_get_structure (caps, 0);
    GstStructure *fixated_struct;

    othercaps = gst_caps_make_writable (othercaps);
    fixated_struct = gst_caps_get_structure (othercaps, 0);

    for (i = 0; i < G_N_ELEMENTS (format_fields); i++) {
      if (gst_structure_has_field (format_struct, format_fields[i])) {
        gst_structure_set (fixated_struct, format_fields[i], G_TYPE_STRING,
            gst_structure_get_string (format_struct, format_fields[i]), NULL);
      } else {
        gst_structure_remove_field (fixated_struct, format_fields[i]);
      }
    }

    if (caps_has_explicit_fixed_par) {
      gst_structure_set (fixated_struct, "pixel-aspect-ratio",
          GST_TYPE_FRACTION, caps_par_n, caps_par_d, NULL);
    } else if (!othercaps_has_explicit_fixed_par) {
      gst_structure_remove_field (fixated_struct, "pixel-aspect-ratio");
    }

    if (direction == GST_PAD_SINK) {
      if (gst_structure_has_field (fixated_struct, "format")) {
        const gchar *color;
        gchar *colorimetry_str;
        GstVideoColorimetry colorimetry;
        GstVideoFormat fmt = gst_video_format_from_string
            (gst_structure_get_string (fixated_struct, "format"));
        switch (fmt) {
          case GST_VIDEO_FORMAT_RGB:
          case GST_VIDEO_FORMAT_RGBA:
          case GST_VIDEO_FORMAT_ARGB:
          case GST_VIDEO_FORMAT_ABGR:
          case GST_VIDEO_FORMAT_GBRA:
          case GST_VIDEO_FORMAT_BGRx:
            color = gst_structure_get_string (fixated_struct, "colorimetry");
            gst_video_colorimetry_from_string (&colorimetry, color);

            colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_RGB;
            colorimetry_str = gst_video_colorimetry_to_string (&colorimetry);
            if (colorimetry_str != NULL) {
              gst_structure_set (fixated_struct, "colorimetry", G_TYPE_STRING,
                  colorimetry_str, NULL);
              g_free (colorimetry_str);
            }
            break;

          default:
            break;
        }
      }

      if (gst_structure_has_field (fixated_struct, "colorimetry")) {
        const gchar *color;
        gchar *colorimetry_str;
        GstVideoColorimetry colorimetry;
        if ((color = gst_structure_get_string (fixated_struct, "colorimetry"))) {
          gst_video_colorimetry_from_string (&colorimetry, color);
        }

        if (colorimetry.range != GST_VIDEO_COLOR_RANGE_16_235) {
          GST_DEBUG_OBJECT (self,
              "WARNING: Full color range input, limited color output\n");
          colorimetry.range = GST_VIDEO_COLOR_RANGE_16_235;
          colorimetry_str = gst_video_colorimetry_to_string (&colorimetry);
          if (colorimetry_str != NULL) {
            gst_structure_set (fixated_struct, "colorimetry", G_TYPE_STRING,
                colorimetry_str, NULL);
            g_free (colorimetry_str);
          }
        }
      }
    }
  }

  return othercaps;
}

static GstFlowReturn
gst_niquadracrop_prepare_output_buffer (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer ** outbuf)
{
  GstNiQuadraCrop *self = GST_NIQUADRACROP (trans);

  if (self->skip_filter) {
    *outbuf = inbuf;
    return GST_FLOW_OK;
  }

  return GST_BASE_TRANSFORM_CLASS
      (gst_niquadracrop_parent_class)->prepare_output_buffer (trans, inbuf,
      outbuf);
}

static GstFlowReturn
gst_niquadracrop_transform (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer * outbuf)
{
  GstNiQuadraCrop *self = GST_NIQUADRACROP (trans);
  GstNiQuadraBaseConvert *parent = GST_NI_QUADRA_BASE_CONVERT (trans);
  ni_session_data_io_t *p_session_data = &self->api_dst_frame;
  GstAllocator *alloc = NULL;
  GstMemory *in_mem = NULL;
  GstMemory *out_mem = NULL;
  niFrameSurface1_t *in_surface = NULL;
  niFrameSurface1_t *out_surface = NULL;
  gint dev_idx = -1;
  int gc620_pixfmt;
  ni_pix_fmt_t niPixFmt;
  int options = 0;
  int pool_size = DEFAULT_NI_FILTER_POOL_SIZE;
  int retval = 0;

  if (self->skip_filter) {
    return GST_FLOW_OK;
  }

  memset (p_session_data, 0, sizeof (ni_session_data_io_t));

  in_mem = gst_buffer_peek_memory (inbuf, 0);
  dev_idx = gst_deviceid_from_ni_hw_memory (in_mem);
  in_surface = gst_surface_from_ni_hw_memory (in_mem);
  if (in_surface == NULL) {
    GST_ERROR_OBJECT (self,
        "Impossible to convert between the formats supported by the filter\n");
    return GST_FLOW_ERROR;
  }

  if (!parent->initialized) {
    if (GST_VIDEO_INFO_FORMAT (&parent->out_info) == GST_VIDEO_FORMAT_BGRP) {
      GST_ERROR_OBJECT (self, "format BGRP not supported\n");
      gst_buffer_unref (inbuf);
      return GST_FLOW_ERROR;
    }

    if (self->x == -1) {
      self->x = (GST_VIDEO_INFO_WIDTH (&parent->in_info) -
          GST_VIDEO_INFO_WIDTH (&parent->out_info)) / 2;
    }
    if (self->y == -1) {
      self->y = (GST_VIDEO_INFO_HEIGHT (&parent->in_info) -
          GST_VIDEO_INFO_HEIGHT (&parent->out_info)) / 2;
    }

    ni_device_session_context_init (&parent->api_ctx);
    parent->api_ctx.session_id = NI_INVALID_SESSION_ID;
    parent->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
    parent->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;
    parent->api_ctx.hw_id = dev_idx;
    parent->api_ctx.device_type = NI_DEVICE_TYPE_SCALER;
    parent->api_ctx.scaler_operation = NI_SCALER_OPCODE_CROP;
    parent->api_ctx.keep_alive_timeout = parent->keep_alive_timeout;
    parent->api_ctx.isP2P = self->is_p2p;

    retval = ni_device_session_open (&parent->api_ctx, NI_DEVICE_TYPE_SCALER);
    if (retval < 0) {
      GST_ERROR_OBJECT (self, "Open crop device session error\n");
      ni_device_session_close (&parent->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
      ni_device_session_context_clear (&parent->api_ctx);
      return GST_FLOW_ERROR;
    } else {
      GST_DEBUG_OBJECT (self, "XCoder %s.%d (inst: %d) opened successfully\n",
          parent->api_ctx.dev_xcoder_name, parent->api_ctx.hw_id,
          parent->api_ctx.session_id);
    }

    memset (&self->api_dst_frame, 0, sizeof (ni_session_data_io_t));

    options = NI_SCALER_FLAG_IO | NI_SCALER_FLAG_PC;
    if (parent->api_ctx.isP2P) {
      options |= NI_SCALER_FLAG_P2;
    }

    if (parent->api_ctx.isP2P) {
      pool_size = 1;
    }

    niPixFmt = convertGstVideoFormatToNIPix (GST_VIDEO_INFO_FORMAT
        (&parent->out_info));
    gc620_pixfmt = convertNIPixToGC620Format (niPixFmt);

    /* Allocate a pool of frames by the scaler */
    /* *INDENT-OFF* */
    retval = ni_device_alloc_frame (&parent->api_ctx,
      NI_ALIGN (GST_VIDEO_INFO_WIDTH (&parent->out_info), 2),
      NI_ALIGN (GST_VIDEO_INFO_HEIGHT (&parent->out_info), 2),
      gc620_pixfmt,
      options,
      0,                      // rec width
      0,                      // rec height
      0,                      // rec X pos
      0,                      // rec Y pos
      pool_size,              // rgba color/pool size
      0,                      // frame index
      NI_DEVICE_TYPE_SCALER);
    /* *INDENT-ON* */
    if (retval < 0) {
      GST_ERROR_OBJECT (self, "Init frame pool error\n");
      gst_buffer_unref (inbuf);
      ni_device_session_context_clear (&parent->api_ctx);
      return GST_FLOW_ERROR;
    }

    parent->initialized = TRUE;
  }

  if (self->x < 0) {
    self->x = (GST_VIDEO_INFO_WIDTH (&parent->in_info) -
        GST_VIDEO_INFO_WIDTH (&parent->out_info)) / 2;
  }
  if (self->y < 0) {
    self->y = (GST_VIDEO_INFO_HEIGHT (&parent->in_info) -
        GST_VIDEO_INFO_HEIGHT (&parent->out_info)) / 2;
  }

  self->x &= ~((1 << self->shift) - 1);
  self->y &= ~((1 << self->shift) - 1);

  if ((unsigned) self->x + (unsigned) GST_VIDEO_INFO_WIDTH (&parent->out_info) >
      GST_VIDEO_INFO_WIDTH (&parent->in_info)) {
    self->x = GST_VIDEO_INFO_WIDTH (&parent->in_info) -
        GST_VIDEO_INFO_WIDTH (&parent->out_info);
  }
  if ((unsigned) self->y + (unsigned) GST_VIDEO_INFO_HEIGHT (&parent->out_info)
      > GST_VIDEO_INFO_HEIGHT (&parent->in_info)) {
    self->y = GST_VIDEO_INFO_HEIGHT (&parent->in_info) -
        GST_VIDEO_INFO_HEIGHT (&parent->out_info);
  }

  retval = ni_frame_buffer_alloc_hwenc (&p_session_data->data.frame,
      GST_VIDEO_INFO_WIDTH (&parent->out_info),
      GST_VIDEO_INFO_HEIGHT (&parent->out_info), 0);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (self, "Can't assign input frame %d\n", retval);
    return GST_FLOW_ERROR;
  }

  niPixFmt = convertGstVideoFormatToNIPix
      (GST_VIDEO_INFO_FORMAT (&parent->in_info));
  gc620_pixfmt = convertNIPixToGC620Format (niPixFmt);

  /*
   * Allocate device input frame. This call won't actually allocate a frame,
   * but sends the incoming hardware frame index to the scaler manager
   */
  /* *INDENT-OFF* */
  retval = ni_device_alloc_frame (&parent->api_ctx,
      NI_ALIGN (GST_VIDEO_INFO_WIDTH (&parent->in_info), 2),
      NI_ALIGN (GST_VIDEO_INFO_HEIGHT (&parent->in_info), 2),
      gc620_pixfmt,
      0,                                               // input frame
      GST_VIDEO_INFO_WIDTH (&parent->out_info),        // src rectangle width
      GST_VIDEO_INFO_HEIGHT (&parent->out_info),       // src rectangle height
      self->x,                                         // src rectangle x
      self->y,                                         // src rectangle y
      in_surface->ui32nodeAddress,
      in_surface->ui16FrameIdx, NI_DEVICE_TYPE_SCALER);
  /* *INDENT-ON* */
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (self, "Can't allocate device output frame %d\n", retval);
    gst_buffer_unref (inbuf);
    ni_frame_buffer_free (&p_session_data->data.frame);
    return GST_FLOW_ERROR;
  }

  /* Allocate hardware device destination frame. This acquires a frame from the pool */
  retval = ni_device_alloc_frame (&parent->api_ctx,
      NI_ALIGN (GST_VIDEO_INFO_WIDTH (&parent->out_info), 2),
      NI_ALIGN (GST_VIDEO_INFO_HEIGHT (&parent->out_info), 2),
      gc620_pixfmt,
      NI_SCALER_FLAG_IO, 0, 0, 0, 0, 0, -1, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (self, "Can't allocate device output frame %d\n", retval);
    gst_buffer_unref (inbuf);
    ni_frame_buffer_free (&p_session_data->data.frame);
    return GST_FLOW_ERROR;
  }

  /* Set the new frame index */
  retval = ni_device_session_read_hwdesc (&parent->api_ctx, p_session_data,
      NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (self, "Can't acquire output frame %d\n", retval);
    gst_buffer_unref (inbuf);
    ni_frame_buffer_free (&p_session_data->data.frame);
    return GST_FLOW_ERROR;
  }

  out_surface = (niFrameSurface1_t *) p_session_data->data.frame.p_data[3];
  out_surface->dma_buf_fd = 0;
  out_surface->ui32nodeAddress = 0;
  out_surface->ui16width =
      NI_ALIGN (GST_VIDEO_INFO_WIDTH (&parent->out_info), 2);
  out_surface->ui16height =
      NI_ALIGN (GST_VIDEO_INFO_HEIGHT (&parent->out_info), 2);

  gst_set_bit_depth_and_encoding_type (&out_surface->bit_depth,
      &out_surface->encoding_type, GST_VIDEO_INFO_FORMAT (&parent->out_info));

  alloc = gst_allocator_find (GST_NIQUADRA_MEMORY_TYPE_NAME);
  out_mem = gst_niquadra_allocator_alloc (alloc, &parent->api_ctx,
      out_surface, parent->api_ctx.hw_id, &parent->out_info);
  gst_buffer_append_memory (outbuf, out_mem);
  gst_object_unref (alloc);

  ni_frame_buffer_free (&p_session_data->data.frame);

  return GST_FLOW_OK;
}

static void
gst_niquadracrop_finalize (GObject * obj)
{
  GstNiQuadraCrop *self = GST_NIQUADRACROP (obj);
  GstNiQuadraBaseConvert *parent = GST_NI_QUADRA_BASE_CONVERT (obj);

  if (self->api_dst_frame.data.frame.p_buffer) {
    ni_frame_buffer_free (&self->api_dst_frame.data.frame);
  }

  if (parent->initialized) {
    if (parent->api_ctx.session_id != NI_INVALID_SESSION_ID) {
      GST_DEBUG_OBJECT (self, "libxcoder crop free context\n");
      ni_device_session_close (&parent->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
    }

    ni_session_context_t *p_ctx = &parent->api_ctx;
    if (p_ctx) {
      if (p_ctx->device_handle != NI_INVALID_DEVICE_HANDLE) {
        ni_device_close (p_ctx->device_handle);
        p_ctx->device_handle = NI_INVALID_DEVICE_HANDLE;
      }
      if (p_ctx->blk_io_handle != NI_INVALID_DEVICE_HANDLE) {
        ni_device_close (p_ctx->blk_io_handle);
        p_ctx->blk_io_handle = NI_INVALID_DEVICE_HANDLE;
      }
    }
    ni_device_session_context_clear (&parent->api_ctx);

    parent->initialized = FALSE;
  }

  G_OBJECT_CLASS (gst_niquadracrop_parent_class)->finalize (obj);
}

static void
gst_niquadracrop_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiQuadraCrop *self;

  g_return_if_fail (GST_IS_NIQUADRACROP (object));
  self = GST_NIQUADRACROP (object);

  switch (prop_id) {
    case PROP_X:
      self->x = g_value_get_int (value);
      break;
    case PROP_Y:
      self->y = g_value_get_int (value);
      break;
    case PROP_P2P:
      self->is_p2p = g_value_get_boolean (value);
      break;
    case PROP_AUTO_SKIP:
      self->auto_skip = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadracrop_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiQuadraCrop *self;

  g_return_if_fail (GST_IS_NIQUADRACROP (object));
  self = GST_NIQUADRACROP (object);

  switch (prop_id) {
    case PROP_X:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->x);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_Y:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->y);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_P2P:
      GST_OBJECT_LOCK (self);
      g_value_set_boolean (value, self->is_p2p);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_AUTO_SKIP:
      g_value_set_boolean (value, self->auto_skip);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadracrop_class_init (GstNiQuadraCropClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class->set_property = gst_niquadracrop_set_property;
  gobject_class->get_property = gst_niquadracrop_get_property;
  gobject_class->finalize = gst_niquadracrop_finalize;

  g_object_class_install_property (gobject_class, PROP_X,
      g_param_spec_int ("x", "X",
          "set the x crop area", -1, G_MAXINT,
          -1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_Y,
      g_param_spec_int ("y", "Y",
          "set the y crop area", -1, G_MAXINT,
          -1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_P2P,
      g_param_spec_boolean ("is-p2p", "Is-P2P",
          "set p2p mode of the crop", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_NAME |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_AUTO_SKIP,
      g_param_spec_boolean ("auto-skip", "Auto-Skip",
          "skip the filter when input and output of this filter are the same",
          FALSE,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (element_class, &src_factory);
  gst_element_class_add_static_pad_template (element_class, &sink_factory);

  gst_element_class_set_static_metadata (element_class,
      "NETINT Quadra CROP filter", "Filter/Effect/Video/NICrop",
      "Crop Netint Quadra", "Minglong Zhang<Minglong.Zhang@netint.cn>");

  trans_class->fixate_caps = gst_niquadracrop_fixate_caps;
  trans_class->set_caps = gst_niquadracrop_set_caps;
  trans_class->prepare_output_buffer = gst_niquadracrop_prepare_output_buffer;
  trans_class->transform = gst_niquadracrop_transform;
}

static void
gst_niquadracrop_init (GstNiQuadraCrop * filter)
{
  filter->x = -1;
  filter->y = -1;
  filter->is_p2p = FALSE;
  filter->skip_filter = FALSE;
}

static gboolean
niquadracrop_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadracrop_debug, "niquadracrop",
      0, "niquadracrop");

  return gst_element_register (plugin, "niquadracrop", GST_RANK_NONE,
      GST_TYPE_NIQUADRACROP);
}
