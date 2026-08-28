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
 *  \file   gstniquadramerge.c
 *
 *  \brief  Implement of NetInt Quadra hardware merge filter.
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

GST_DEBUG_CATEGORY_STATIC (gst_niquadramerge_debug);
#define GST_CAT_DEFAULT gst_niquadramerge_debug

enum
{
  PROP_0,
  PROP_FLITERBLIT,
  PROP_LAST
};

typedef struct _GstNIQuadraMerge
{
  GstNiQuadraBaseConvert parent;

  ni_session_data_io_t api_dst_frame;
  ni_scaler_params_t params;
  guint extra_frames;
  gint downstream_card;
} GstNIQuadraMerge;

typedef struct _GstNIQuadraMergeClass
{
  GstNiQuadraBaseConvertClass parent_class;
} GstNIQuadraMergeClass;

#define GST_TYPE_NIQUADRAMERGE \
  (gst_niquadramerge_get_type())
#define GST_NIQUADRAMERGE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRAMERGE,GstNIQuadraMerge))
#define GST_NIQUADRAMERGE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRAMERGE,GstNIQuadraMerge))
#define GST_IS_NIQUADRAMERGE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRAMERGE))
#define GST_IS_NIQUADRAMERGE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRAMERGE))

static gboolean niquadramerge_element_init (GstPlugin * plugin);

#define gst_niquadramerge_parent_class parent_class

GType gst_niquadramerge_get_type (void);

G_DEFINE_TYPE (GstNIQuadraMerge, gst_niquadramerge,
    GST_TYPE_NI_QUADRA_BASE_CONVERT);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadramerge, niquadramerge_element_init);

#define SUPPORTED_FORMATS \
    "{ I420, NV12, ARGB, RGBA, ABGR, BGRA, I420_10LE, NV16, BGRx, NV12_10LE32 }"

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

static GstCaps *
gst_niquadramerge_fixate_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GstNIQuadraMerge *self = GST_NIQUADRAMERGE (base);
  GstVideoInfo info;

  GST_DEBUG_OBJECT (base,
      "trying to fixate othercaps %" GST_PTR_FORMAT " based on caps %"
      GST_PTR_FORMAT, othercaps, caps);

  if (direction == GST_PAD_SINK) {
    GstStructure *outs;
    gint w = 0, h = 0;
    gboolean is_downstream_configured = FALSE;

    if (!gst_video_info_from_caps (&info, caps)) {
      return FALSE;
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
      const gchar *field = format_fields[i];
      const gchar *new_value;
      const gchar *old_value = NULL;

      if (!gst_structure_has_field (format_struct, field)) {
        gst_structure_remove_field (fixated_struct, field);
        continue;
      }

      new_value = gst_structure_get_string (format_struct, field);

      if (gst_structure_has_field (fixated_struct, field)) {
        gst_structure_fixate_field_string (fixated_struct, field, new_value);
        old_value = gst_structure_get_string (fixated_struct, field);
      }

      if (gst_structure_has_field (fixated_struct, field) &&
          g_strcmp0 (old_value, new_value) != 0 &&
          g_strcmp0 (field, "format") == 0) {
        GST_ERROR_OBJECT (self, "%s mismatch: %s vs %s",
            field, new_value, GST_STR_NULL (old_value));
        gst_caps_unref (othercaps);
        return gst_caps_new_empty ();
      } else {
        gst_structure_set (fixated_struct, field,
            G_TYPE_STRING, new_value, NULL);
      }
    }

    if (direction == GST_PAD_SINK) {
      if (gst_structure_has_field (fixated_struct, "format")) {
        const gchar *color_str;
        gchar *color;
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
            color_str =
                gst_structure_get_string (fixated_struct, "colorimetry");
            gst_video_colorimetry_from_string (&colorimetry, color_str);

            colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_RGB;
            color = gst_video_colorimetry_to_string (&colorimetry);
            if (color != NULL) {
              gst_structure_set (fixated_struct, "colorimetry", G_TYPE_STRING,
                  color, NULL);
              g_free (color);
            }
            break;

          default:
            break;
        }
      }

      if (gst_structure_has_field (fixated_struct, "colorimetry")) {
        const gchar *color_str;
        gchar *color;
        GstVideoColorimetry colorimetry;
        if ((color_str =
                gst_structure_get_string (fixated_struct, "colorimetry"))) {
          gst_video_colorimetry_from_string (&colorimetry, color_str);
        }

        if (colorimetry.range != GST_VIDEO_COLOR_RANGE_16_235) {
          GST_DEBUG ("WARNING: Full color range input, limited color output\n");
          colorimetry.range = GST_VIDEO_COLOR_RANGE_16_235;
          color = gst_video_colorimetry_to_string (&colorimetry);
          if (color != NULL) {
            gst_structure_set (fixated_struct, "colorimetry", G_TYPE_STRING,
                color, NULL);
            g_free (color);
          }
        }
      }
    }
  }

  return othercaps;
}

static void
gst_niquadramerge_query_downstream_allocation (GstNIQuadraMerge * self,
    GstBaseTransform * trans, GstCaps * caps)
{
  GstQuery *query;
  GstPad *srcpad;
  guint i;
  GType gtype;
  const GstStructure *params = NULL;
  gboolean gotit;

  self->extra_frames = 0;
  self->downstream_card = -1;

  srcpad = gst_element_get_static_pad (GST_ELEMENT (trans), "src");
  if (!srcpad) {
    return;
  }

  query = gst_query_new_allocation (caps, TRUE);

  if (gst_pad_peer_query (srcpad, query) == TRUE) {
    /* Search for allocation metadata */
    for (i = 0; i < gst_query_get_n_allocation_metas (query); i++) {
      gtype = gst_query_parse_nth_allocation_meta (query, i, &params);
      if (gtype == GST_VIDEO_META_API_TYPE) {
        if (params && (strcmp (gst_structure_get_name (params),
                    NI_PREALLOCATE_STRUCTURE_NAME) == 0)) {

          gotit = gst_structure_get_uint (params, NI_VIDEO_META_BUFCNT,
              &self->extra_frames);
          if (gotit == FALSE)
            GST_ERROR_OBJECT (self, "Did not find buffercnt\n");

          gotit = gst_structure_get_int (params, NI_VIDEO_META_CARDNO,
              &self->downstream_card);
          if (gotit == FALSE)
            GST_ERROR_OBJECT (self, "Did not find cardno\n");

          break;
        }
      }
    }
  }

  gst_query_unref (query);
  gst_object_unref (srcpad);
}

static GstFlowReturn
gst_niquadramerge_transform (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer * outbuf)
{
  GstNIQuadraMerge *self = GST_NIQUADRAMERGE (trans);
  GstNiQuadraBaseConvert *parent = GST_NI_QUADRA_BASE_CONVERT (trans);
  GstBuffer *tmpbuf = NULL;
  GstMemory *in_mem0 = NULL;
  GstMemory *in_mem1 = NULL;
  GstMemory *out_mem = NULL;
  niFrameSurface1_t *in_surface0 = NULL;
  niFrameSurface1_t *in_surface1 = NULL;
  int cardno = NI_INVALID_HWID;
  ni_session_data_io_t *p_session_data;
  int gc620_pixfmt;
  int flags;
  ni_pix_fmt_t niPixFmt;
  int retval = 0;
  GstCaps *out_caps = NULL;

  in_mem0 = gst_buffer_peek_memory (inbuf, 0);
  in_surface0 = gst_surface_from_ni_hw_memory (in_mem0);
  cardno = gst_deviceid_from_ni_hw_memory (in_mem0);

  in_mem1 = gst_buffer_peek_memory (inbuf, 1);
  in_surface1 = gst_surface_from_ni_hw_memory (in_mem1);

  if (!in_surface0 || !in_surface1) {
    GST_ERROR_OBJECT (self, "gstniquadramerge frame_surface is null");
    return GST_FLOW_ERROR;
  }

  niPixFmt =
      convertGstVideoFormatToNIPix (GST_VIDEO_INFO_FORMAT (&parent->in_info));
  gc620_pixfmt = convertNIPixToGC620Format (niPixFmt);

  if (!parent->initialized) {
    retval = ni_device_session_context_init (&parent->api_ctx);
    if (retval < 0) {
      return GST_FLOW_ERROR;
    }

    parent->api_ctx.session_id = NI_INVALID_SESSION_ID;
    parent->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
    parent->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;
    parent->api_ctx.hw_id = cardno;
    parent->api_ctx.device_type = NI_DEVICE_TYPE_SCALER;
    parent->api_ctx.scaler_operation = NI_SCALER_OPCODE_MERGE;
    parent->api_ctx.keep_alive_timeout = parent->keep_alive_timeout;
    parent->api_ctx.isP2P = 0;

    retval = ni_device_session_open (&parent->api_ctx, NI_DEVICE_TYPE_SCALER);
    if (retval < 0) {
      if (self->api_dst_frame.data.frame.p_buffer) {
        ni_frame_buffer_free (&self->api_dst_frame.data.frame);
      }
      ni_device_session_close (&parent->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
      ni_device_session_context_clear (&parent->api_ctx);
      return GST_FLOW_ERROR;
    }

    if (self->params.filterblit) {
      retval = ni_scaler_set_params (&parent->api_ctx, &(self->params));
      if (retval < 0) {
        return GST_FLOW_ERROR;
      }
    }

    out_caps = gst_video_info_to_caps (&parent->out_info);
    gst_caps_set_features_simple (out_caps,
        gst_caps_features_from_string
        (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY));
    gst_niquadramerge_query_downstream_allocation (self, trans, out_caps);
    gst_caps_unref (out_caps);

    int pool_size = DEFAULT_NI_FILTER_POOL_SIZE;
    // If the downstream element is on a different card,
    // Allocate extra frames suggested by the downstream element
    if (parent->api_ctx.hw_id != self->downstream_card) {
      pool_size += self->extra_frames;
      GST_INFO_OBJECT (self, "Increase frame pool by %d", self->extra_frames);
    }

    int options = NI_SCALER_FLAG_IO | NI_SCALER_FLAG_PC;

    /* Allocate a pool of frames by the scaler */
    /* *INDENT-OFF* */
    retval = ni_device_alloc_frame (&parent->api_ctx,
      NI_ALIGN (GST_VIDEO_INFO_WIDTH (&parent->out_info), 2),
      NI_ALIGN (GST_VIDEO_INFO_HEIGHT (&parent->out_info), 2),
      gc620_pixfmt, options,
      0,                      // rec width
      0,                      // rec height
      0,                      // rec X pos
      0,                      // rec Y pos
      pool_size,              // rgba color/pool size
      0,                      // frame index
      NI_DEVICE_TYPE_SCALER);
    /* *INDENT-ON* */
    if (retval < 0) {
      return GST_FLOW_ERROR;
    }

    parent->initialized = TRUE;
  }

  retval = ni_frame_buffer_alloc_hwenc (&self->api_dst_frame.data.frame,
      GST_VIDEO_INFO_WIDTH (&parent->out_info),
      GST_VIDEO_INFO_HEIGHT (&parent->out_info), 0);
  if (retval != NI_RETCODE_SUCCESS) {
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
      (in_surface0 && in_surface1->encoding_type == 2) ? NI_SCALER_FLAG_CMP : 0,
      NI_ALIGN (GST_VIDEO_INFO_WIDTH (&parent->in_info), 2),  // src rectangle width
      NI_ALIGN (GST_VIDEO_INFO_HEIGHT (&parent->in_info), 2),  // src rectangle height
      0,                        // src rectangle x = 0
      0,                        // src rectangle y = 0
      in_surface0->ui32nodeAddress,
      in_surface0->ui16FrameIdx, NI_DEVICE_TYPE_SCALER);
  /* *INDENT-ON* */
  if (retval != NI_RETCODE_SUCCESS) {
    return GST_FLOW_ERROR;
  }

  flags = NI_SCALER_FLAG_IO;
  flags |= (in_surface1->encoding_type == 2) ? NI_SCALER_FLAG_CMP : 0;
  /* Allocate device destination frame. This will acquire a frame from the pool */
  /* *INDENT-OFF* */
  retval = ni_device_alloc_frame (&parent->api_ctx,
      NI_ALIGN (GST_VIDEO_INFO_WIDTH (&parent->out_info), 2),
      NI_ALIGN (GST_VIDEO_INFO_HEIGHT (&parent->out_info), 2),
      gc620_pixfmt,
      flags,    // output frame
      NI_ALIGN (GST_VIDEO_INFO_WIDTH (&parent->out_info), 2),  // dst rectangle width
      NI_ALIGN (GST_VIDEO_INFO_HEIGHT (&parent->out_info), 2), // dst rectangle height
      0,                        // dst rectangle x
      0,                        // dst rectangle y
      in_surface1->ui32nodeAddress, // rgba color
      in_surface1->ui16FrameIdx, NI_DEVICE_TYPE_SCALER);
  /* *INDENT-ON* */
  if (retval != NI_RETCODE_SUCCESS) {
    return GST_FLOW_ERROR;
  }

  /* Set the new frame index */
  retval = ni_device_session_read_hwdesc (&parent->api_ctx,
      &self->api_dst_frame, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    return GST_FLOW_ERROR;
  }

  p_session_data = &self->api_dst_frame;
  ni_frame_buffer_free (&p_session_data->data.frame);

  in_surface1->ui16width = GST_VIDEO_INFO_WIDTH (&parent->out_info);
  in_surface1->ui16height = GST_VIDEO_INFO_HEIGHT (&parent->out_info);

  ni_set_bit_depth_and_encoding_type (&in_surface1->bit_depth,
      &in_surface1->encoding_type, niPixFmt);

  tmpbuf = gst_buffer_make_writable (outbuf);
  out_mem = gst_memory_ref (in_mem1);
  gst_buffer_append_memory (tmpbuf, out_mem);

  gst_buffer_copy_into (tmpbuf, inbuf,
      GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_META, 0, -1);

  return GST_FLOW_OK;
}

static void
gst_niquadramerge_finalize (GObject * obj)
{
  GstNIQuadraMerge *self = GST_NIQUADRAMERGE (obj);
  GstNiQuadraBaseConvert *parent = GST_NI_QUADRA_BASE_CONVERT (obj);

  if (self->api_dst_frame.data.frame.p_buffer) {
    ni_frame_buffer_free (&self->api_dst_frame.data.frame);
  }

  if (parent->initialized) {
    if (parent->api_ctx.session_id != NI_INVALID_SESSION_ID) {
      GST_DEBUG_OBJECT (self, "libxcoder scale free context");
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

  G_OBJECT_CLASS (gst_niquadramerge_parent_class)->finalize (obj);
}

static void
gst_niquadramerge_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNIQuadraMerge *self;

  g_return_if_fail (GST_IS_NIQUADRAMERGE (object));
  self = GST_NIQUADRAMERGE (object);

  switch (prop_id) {
    case PROP_FLITERBLIT:
      self->params.filterblit = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadramerge_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNIQuadraMerge *self;

  g_return_if_fail (GST_IS_NIQUADRAMERGE (object));
  self = GST_NIQUADRAMERGE (object);

  switch (prop_id) {
    case PROP_FLITERBLIT:
      g_value_set_int (value, self->params.filterblit);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadramerge_class_init (GstNIQuadraMergeClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class->set_property = gst_niquadramerge_set_property;
  gobject_class->get_property = gst_niquadramerge_get_property;
  gobject_class->finalize = gst_niquadramerge_finalize;

  g_object_class_install_property (gobject_class, PROP_FLITERBLIT,
      g_param_spec_int ("filterblit", "FILTERBLIT",
          "set filterblit", 0, 2, 0,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (element_class, &src_factory);
  gst_element_class_add_static_pad_template (element_class, &sink_factory);

  gst_element_class_set_static_metadata (element_class,
      "NETINT Quadra MERGE filter", "Filter/Effect/Video/NIMerge",
      "MERGE Netint Quadra", "Minglong Zhang<minglong.zhang@netint.cn>");

  trans_class->fixate_caps = gst_niquadramerge_fixate_caps;
  trans_class->transform = gst_niquadramerge_transform;
}

static gboolean
niquadramerge_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadramerge_debug, "niquadramerge",
      0, "niquadramerge");

  return gst_element_register (plugin, "niquadramerge", GST_RANK_NONE,
      GST_TYPE_NIQUADRAMERGE);
}

static void
gst_niquadramerge_init (GstNIQuadraMerge * filter)
{
  filter->params.filterblit = 0;
  filter->downstream_card = -1;
  filter->extra_frames = 0;
}
