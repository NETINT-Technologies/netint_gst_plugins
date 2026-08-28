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
 *  \file   GstNiQuadraScale.c
 *
 *  \brief  Implement of NetInt Quadra hardware scale filter.
 ******************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/base/gstbasetransform.h>

#include "niquadra.h"
#include "ni_device_api.h"
#include "gstniquadramemory.h"
#include "gstniquadrautils.h"
#include "gstniquadrabaseconvert.h"

GST_DEBUG_CATEGORY_STATIC (gst_niquadrascale_debug);
#define GST_CAT_DEFAULT gst_niquadrascale_debug

enum
{
  PROP_0,
  PROP_P2P,
  PROP_FILTER_BLIT,
  PROP_AUTO_SELECT,
  PROP_IN_COLOR_MATRIX,
  PROP_OUT_COLOR_MATRIX,
  PROP_AUTO_SKIP,
  PROP_HWFRAME_POOL_SIZE,
  PROP_LAST
};

typedef struct _GstNiQuadraScale
{
  GstNiQuadraBaseConvert parent;

  gboolean is_p2p;
  gboolean auto_select;
  gchar *in_color_matrix;
  gchar *out_color_matrix;

  gboolean auto_skip;
  gint hwframe_pool_size;
  gboolean is_skip;

  ni_session_data_io_t api_dst_frame;
  ni_session_data_io_t api_src_frame;
  ni_scaler_params_t params;
} GstNiQuadraScale;

typedef struct _GstNiQuadraScaleClass
{
  GstNiQuadraBaseConvertClass parent_class;
} GstNiQuadraScaleClass;

#define GST_TYPE_NIQUADRASCALE \
  (gst_niquadrascale_get_type())
#define GST_NIQUADRASCALE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRASCALE,GstNiQuadraScale))
#define GST_NIQUADRASCALE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRASCALE,GstNiQuadraScale))
#define GST_IS_NIQUADRASCALE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRASCALE))
#define GST_IS_NIQUADRASCALE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRASCALE))

static gboolean niquadrascale_element_init (GstPlugin * plugin);

#define gst_niquadrascale_parent_class parent_class

GType gst_niquadrascaleniquadra_get_type (void);

G_DEFINE_TYPE (GstNiQuadraScale, gst_niquadrascale,
    GST_TYPE_NI_QUADRA_BASE_CONVERT);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadrascale, niquadrascale_element_init);

#if ENABLE_NI_GST_FRAMEWORK_PATCH
#define NI_QUADRA_TILED_FORMATS ", NI_QUAD_8_4L4"
#else
#define NI_QUADRA_TILED_FORMATS ""
#endif

#define SUPPORTED_FORMATS \
    "{ I420, YUY2, UYVY, NV12, ARGB, RGBA, ABGR, BGRA, I420_10LE, P010_10LE, " \
    "NV16, BGRx, NV12_10LE32" NI_QUADRA_TILED_FORMATS " }"

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

static void
gst_niquadrascale_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  g_return_if_fail (GST_IS_NIQUADRASCALE (object));
  GstNiQuadraScale *self = GST_NIQUADRASCALE (object);

  switch (prop_id) {
    case PROP_P2P:
      g_value_set_boolean (value, self->is_p2p);
      break;
    case PROP_FILTER_BLIT:
      g_value_set_int (value, self->params.filterblit);
      break;
    case PROP_AUTO_SELECT:
      g_value_set_boolean (value, self->auto_select);
      break;
    case PROP_IN_COLOR_MATRIX:
      g_value_take_string (value, g_strdup (self->in_color_matrix));
      break;
    case PROP_OUT_COLOR_MATRIX:
      g_value_take_string (value, g_strdup (self->out_color_matrix));
      break;
    case PROP_AUTO_SKIP:
      g_value_set_boolean (value, self->auto_skip);
      break;
    case PROP_HWFRAME_POOL_SIZE:
      g_value_set_int (value, self->hwframe_pool_size);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadrascale_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiQuadraScale *self;

  g_return_if_fail (GST_IS_NIQUADRASCALE (object));
  self = GST_NIQUADRASCALE (object);

  switch (prop_id) {
    case PROP_P2P:
      self->is_p2p = g_value_get_boolean (value);
      break;
    case PROP_FILTER_BLIT:
      self->params.filterblit = g_value_get_int (value);
      break;
    case PROP_AUTO_SELECT:
      self->auto_select = g_value_get_boolean (value);
      break;
    case PROP_IN_COLOR_MATRIX:
      g_free (self->in_color_matrix);
      self->in_color_matrix = g_strdup (g_value_get_string (value));
      break;
    case PROP_OUT_COLOR_MATRIX:
      g_free (self->out_color_matrix);
      self->out_color_matrix = g_strdup (g_value_get_string (value));
      break;
    case PROP_AUTO_SKIP:
      self->auto_skip = g_value_get_boolean (value);
      break;
    case PROP_HWFRAME_POOL_SIZE:
      self->hwframe_pool_size = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadrascale_finalize (GObject * obj)
{
  GstNiQuadraScale *self = GST_NIQUADRASCALE (obj);
  GstBaseTransform *base = GST_BASE_TRANSFORM (self);
  GstNiQuadraBaseConvert *parent = GST_NI_QUADRA_BASE_CONVERT (obj);

  if (self->api_dst_frame.data.frame.p_buffer) {
    ni_frame_buffer_free (&self->api_dst_frame.data.frame);
  }

  if (parent->initialized) {
    if (parent->api_ctx.session_id != NI_INVALID_SESSION_ID) {
      GST_DEBUG_OBJECT (base, "libxcoder scale free context");
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

  if (self->in_color_matrix) {
    g_free (self->in_color_matrix);
    self->in_color_matrix = NULL;
  }

  if (self->out_color_matrix) {
    g_free (self->out_color_matrix);
    self->out_color_matrix = NULL;
  }

  G_OBJECT_CLASS (gst_niquadrascale_parent_class)->finalize (obj);
}

typedef struct _GstForamtNameMap
{
  const gchar *format_str;
  const gchar *format_alias;
} GstForamtNameMap;

static GstCaps *
gst_niquadrascale_fixate_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GstNiQuadraScale *self = GST_NIQUADRASCALE (base);
  GstNiQuadraBaseConvert *parent = GST_NI_QUADRA_BASE_CONVERT (base);
  GstCaps *format;

  GST_DEBUG_OBJECT (base,
      "trying to fixate othercaps %" GST_PTR_FORMAT " based on caps %"
      GST_PTR_FORMAT, othercaps, caps);

  if (self->api_dst_frame.data.frame.p_buffer) {
    ni_frame_buffer_free (&self->api_dst_frame.data.frame);
  }

  if (parent->initialized) {
    if (parent->api_ctx.session_id != NI_INVALID_SESSION_ID) {
      GST_DEBUG_OBJECT (base, "libxcoder scale free context");
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

  othercaps = remove_structures_from_caps (othercaps, "video/x-raw", 1);

  if (direction == GST_PAD_SINK) {
    GstStructure *outs;
    const gchar *out_format;
    gint w = 0, h = 0;
    gboolean is_downstream_configured = FALSE;

    outs = gst_caps_get_structure (othercaps, 0);
    if ((gst_structure_get_int (outs, "width", &w)) ||
        (gst_structure_get_int (outs, "height", &h)) ||
        (out_format = gst_structure_get_string (outs, "format"))) {
      is_downstream_configured = TRUE;
    }

    if (!is_downstream_configured) {
      GST_WARNING_OBJECT (self, "downstream must configure dimension");
    }
  }

  format = GST_NI_QUADRA_BASE_CONVERT_CLASS
      (parent_class)->get_fixed_format (base, direction, caps, othercaps);
  if (gst_caps_is_empty (format)) {
    GST_ERROR_OBJECT (base, "Could not convert formats");
    return format;
  }

  othercaps = GST_NI_QUADRA_BASE_CONVERT_CLASS
      (parent_class)->fixate_size (base, direction, caps, othercaps);
  if (gst_caps_get_size (othercaps) == 1) {
    gint i;
    const gchar *format_fields[] = { "format", "colorimetry", "chroma-site" };
    GstStructure *format_struct = gst_caps_get_structure (format, 0);
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

    if (direction == GST_PAD_SINK) {
      if (gst_structure_has_field (fixated_struct, "format")) {
        const gchar *color;
        gchar *colorimetry_str;
        GstVideoColorimetry colorimetry;
        GstVideoFormat fmt =
            gst_video_format_from_string (gst_structure_get_string
            (fixated_struct, "format"));
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
          GST_DEBUG_OBJECT (base,
              "WARNING: Full color range input, limited color output");
          colorimetry.range = GST_VIDEO_COLOR_RANGE_16_235;
          colorimetry_str = gst_video_colorimetry_to_string (&colorimetry);
          if (colorimetry_str != NULL) {
            gst_structure_set (fixated_struct, "colorimetry", G_TYPE_STRING,
                colorimetry_str, NULL);
            g_free (colorimetry_str);
          }
        }
      }

      if (self->out_color_matrix) {
        GstVideoColorimetry colorimetry;
        if (!gst_video_colorimetry_from_string (&colorimetry,
                self->out_color_matrix)) {
          GST_ERROR_OBJECT (base, "gst_video_colorimetry_from_string failed");
        } else {
          gst_structure_set (fixated_struct, "colorimetry", G_TYPE_STRING,
              self->out_color_matrix, NULL);
        }
      }
    }
  }
  gst_caps_unref (format);

  return othercaps;
}

static GstFlowReturn
gst_niquadrascale_transform (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer * outbuf)
{
  GstNiQuadraScale *self = GST_NIQUADRASCALE (trans);
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
  int pool_size = self->hwframe_pool_size;
  int retval = 0;

  if (self->is_skip) {
    return GST_FLOW_OK;
  }

  memset (p_session_data, 0, sizeof (ni_session_data_io_t));

  in_mem = gst_buffer_peek_memory (inbuf, 0);
  dev_idx = gst_deviceid_from_ni_hw_memory (in_mem);
  in_surface = gst_surface_from_ni_hw_memory (in_mem);
  if (in_surface == NULL) {
    GST_ERROR_OBJECT (trans,
        "Impossible to convert between the formats supported by the filter");
    return GST_FLOW_ERROR;
  }

  if (!parent->initialized) {
    ni_device_session_context_init (&parent->api_ctx);
    parent->api_ctx.session_id = NI_INVALID_SESSION_ID;
    parent->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
    parent->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;
    parent->api_ctx.hw_id = dev_idx;
    parent->api_ctx.device_type = NI_DEVICE_TYPE_SCALER;
    parent->api_ctx.scaler_operation = NI_SCALER_OPCODE_SCALE;
    parent->api_ctx.keep_alive_timeout = parent->keep_alive_timeout;
    parent->api_ctx.isP2P = self->is_p2p;

    retval = ni_device_session_open (&parent->api_ctx, NI_DEVICE_TYPE_SCALER);
    if (retval < 0) {
      GST_ERROR_OBJECT (trans, "Open scale session error");
      ni_device_session_close (&parent->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
      ni_device_session_context_clear (&parent->api_ctx);
      return GST_FLOW_ERROR;
    } else {
      GST_DEBUG_OBJECT (trans, "XCoder %s.%d (inst: %d) opened successfully",
          parent->api_ctx.dev_xcoder_name, parent->api_ctx.hw_id,
          parent->api_ctx.session_id);
    }

    if (self->auto_select) {
      if (GST_VIDEO_INFO_WIDTH (&parent->out_info) <= 540
          || GST_VIDEO_INFO_HEIGHT (&parent->out_info) <= 540) {
        self->params.filterblit = 1;
      } else {
        self->params.filterblit = 2;
      }
    }

    if (self->params.filterblit) {
      retval = ni_scaler_set_params (&parent->api_ctx, &(self->params));
      if (retval < 0) {
        GST_ERROR_OBJECT (trans, "Scale set filterblit fail");
        return GST_FLOW_ERROR;
      }
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
      GST_ERROR_OBJECT (trans, "Init frame pool error");
      ni_device_session_context_clear (&parent->api_ctx);
      return GST_FLOW_ERROR;
    }

    parent->initialized = TRUE;
  }

  retval = ni_frame_buffer_alloc_hwenc (&p_session_data->data.frame,
      GST_VIDEO_INFO_WIDTH (&parent->out_info),
      GST_VIDEO_INFO_HEIGHT (&parent->out_info), 0);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (trans, "Can't assign input frame %d", retval);
    return GST_FLOW_ERROR;
  }

  niPixFmt = convertGstVideoFormatToNIPix
      (GST_VIDEO_INFO_FORMAT (&parent->in_info));
  gc620_pixfmt = convertNIPixToGC620Format (niPixFmt);

  options = 0;
  options |= (in_surface->encoding_type == 2) ? NI_SCALER_FLAG_CMP : 0;
  if (self->in_color_matrix && (strcmp (self->in_color_matrix, "bt2020") == 0)) {
    GST_ERROR_OBJECT (trans, "bt2020 in color matrix not supported");
    return GST_FLOW_ERROR;
  }

  /*
   * Allocate device input frame. This call won't actually allocate a frame,
   * but sends the incoming hardware frame index to the scaler manager
   */
  /* *INDENT-OFF* */
  retval = ni_device_alloc_frame (&parent->api_ctx,
      NI_ALIGN (GST_VIDEO_INFO_WIDTH (&parent->in_info), 2),
      NI_ALIGN (GST_VIDEO_INFO_HEIGHT (&parent->in_info), 2),
      gc620_pixfmt,
      options,
      0,
      0,
      0,
      0,
      0,
      in_surface->ui16FrameIdx,
      NI_DEVICE_TYPE_SCALER);
  /* *INDENT-ON* */
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (trans, "Can't allocate device output frame %d", retval);
    return GST_FLOW_ERROR;
  }

  niPixFmt = convertGstVideoFormatToNIPix
      (GST_VIDEO_INFO_FORMAT (&parent->out_info));
  gc620_pixfmt = convertNIPixToGC620Format (niPixFmt);
  options = NI_SCALER_FLAG_IO;
#if ENABLE_NI_GST_FRAMEWORK_PATCH
  options |= (GST_VIDEO_INFO_FORMAT (&parent->out_info) ==
      GST_VIDEO_FORMAT_NI_QUAD_8_4L4) ? NI_SCALER_FLAG_CMP : 0;
#endif
  if (self->out_color_matrix && strcmp (self->out_color_matrix, "bt2020") == 0) {
    options |= NI_SCALER_FLAG_CS;
  }

  /* Allocate hardware device destination frame. This acquires a frame from the pool */
  retval = ni_device_alloc_frame (&parent->api_ctx,
      NI_ALIGN (GST_VIDEO_INFO_WIDTH (&parent->out_info), 2),
      NI_ALIGN (GST_VIDEO_INFO_HEIGHT (&parent->out_info), 2),
      gc620_pixfmt, options, 0, 0, 0, 0, 0, -1, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (trans, "Can't allocate device output frame %d", retval);
    return GST_FLOW_ERROR;
  }

  /* Set the new frame index */
  retval = ni_device_session_read_hwdesc (&parent->api_ctx, p_session_data,
      NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (trans, "Can't acquire output frame %d", retval);
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

static gboolean
gst_niquadrascale_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstNiQuadraScale *self = GST_NIQUADRASCALE (trans);
  GstNiQuadraBaseConvert *parent = GST_NI_QUADRA_BASE_CONVERT (trans);

  if (!GST_BASE_TRANSFORM_CLASS (parent_class)->set_caps (trans, incaps,
          outcaps)) {
    return FALSE;
  }

  if (self->auto_skip
      && gst_video_info_is_equal (&parent->in_info, &parent->out_info)) {
    self->is_skip = TRUE;
  } else {
    self->is_skip = FALSE;
  }

  return TRUE;
}

static GstFlowReturn
gst_niquadrascale_prepare_output_buffer (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer ** outbuf)
{
  GstNiQuadraScale *self = GST_NIQUADRASCALE (trans);

  if (self->is_skip) {
    *outbuf = inbuf;
    return GST_FLOW_OK;
  }

  return GST_BASE_TRANSFORM_CLASS (parent_class)->prepare_output_buffer (trans,
      inbuf, outbuf);
}

static void
gst_niquadrascale_class_init (GstNiQuadraScaleClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class->set_property = gst_niquadrascale_set_property;
  gobject_class->get_property = gst_niquadrascale_get_property;
  gobject_class->finalize = gst_niquadrascale_finalize;

  g_object_class_install_property (gobject_class, PROP_P2P,
      g_param_spec_boolean ("is-p2p", "Is-p2p",
          "enable p2p transfer",
          FALSE,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_FILTER_BLIT,
      g_param_spec_int ("filterblit", "Filter_blit",
          "filterblit enable", 0, 2, 0,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_AUTO_SELECT,
      g_param_spec_boolean ("autoselect", "AutoSelect",
          "auto select filterblit mode according to resolution",
          FALSE,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_IN_COLOR_MATRIX,
      g_param_spec_string ("in-color-matrix",
          "In-Color-Matrix",
          "set input YCbCr type (bt709)",
          NULL,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
      PROP_OUT_COLOR_MATRIX,
      g_param_spec_string ("out-color-matrix",
          "Out-Color-Matrix",
          "set output YCbCr type (bt709, bt2020)",
          NULL,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_AUTO_SKIP,
      g_param_spec_boolean ("auto-skip", "Auto-Skip",
          "skip the filter when input and output of this filter are the same",
          FALSE,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_HWFRAME_POOL_SIZE,
      g_param_spec_int ("hwframe-pool-size", "hwframe-pool-size",
          "Frame number of a allocated pool on Quadra transcoding card",
          0, G_MAXINT, DEFAULT_NI_FILTER_POOL_SIZE,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (element_class, &src_factory);
  gst_element_class_add_static_pad_template (element_class, &sink_factory);

  gst_element_class_set_static_metadata (element_class,
      "NETINT Quadra SCALE filter", "Filter/Effect/Video/NIScale",
      "Scale Netint Quadra", "Minglong Zhang<Minglong.Zhang@netint.cn>");

  trans_class->fixate_caps = gst_niquadrascale_fixate_caps;
  trans_class->transform = gst_niquadrascale_transform;
  trans_class->set_caps = gst_niquadrascale_set_caps;
  trans_class->prepare_output_buffer = gst_niquadrascale_prepare_output_buffer;
}

static void
gst_niquadrascale_init (GstNiQuadraScale * self)
{
  self->is_p2p = FALSE;
  self->auto_select = FALSE;
  self->params.filterblit = 0;
  self->is_skip = FALSE;
}

static gboolean
niquadrascale_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadrascale_debug, "niquadrascale",
      0, "niquadrascale");

  return gst_element_register (plugin, "niquadrascale", GST_RANK_NONE,
      GST_TYPE_NIQUADRASCALE);
}
