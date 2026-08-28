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
 *  \file   GstNiQuadraRotate.c
 *
 *  \brief  Implement of NetInt Quadra hardware rotate filter.
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

GST_DEBUG_CATEGORY_STATIC (gst_niquadrarotate_debug);
#define GST_CAT_DEFAULT gst_niquadrarotate_debug

enum
{
  PROP_0,
  PROP_P2P,
  PROP_COLOR,
  PROP_ANGLE,
  PROP_AUTO_SKIP,
  PROP_LAST
};

#define BUFFER_WIDTH_PIXEL_ALIGNMENT 16

typedef struct _GstNiQuadraRotate
{
  GstNiQuadraBaseConvert parent;

  ni_session_data_io_t api_dst_frame;
  ni_frame_config_t output_frame_config;

  int angle;
  char *fillcolor_str;
  uint8_t fillcolor[4];
  bool fillcolor_enable;
  bool is_p2p;
  guint extra_frames;
  gint downstream_card;

  gboolean auto_skip;
  gboolean skip_filter;
} GstNiQuadraRotate;

typedef struct _GstNiQuadraRotateClass
{
  GstNiQuadraBaseConvertClass parent_class;
} GstNiQuadraRotateClass;

#define GST_TYPE_NIQUADRAROTATE \
  (gst_niquadrarotate_get_type())
#define GST_NIQUADRAROTATE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRAROTATE,GstNiQuadraRotate))
#define GST_NIQUADRAROTATE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRAROTATE,GstNiQuadraRotate))
#define GST_IS_NIQUADRAROTATE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRAROTATE))
#define GST_IS_NIQUADRAROTATE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRAROTATE))

static gboolean niquadrarotate_element_init (GstPlugin * plugin);

#define gst_niquadrarotate_parent_class parent_class

GType gst_niquadrarotateniquadra_get_type (void);

G_DEFINE_TYPE (GstNiQuadraRotate, gst_niquadrarotate,
    GST_TYPE_NI_QUADRA_BASE_CONVERT);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadrarotate,
    niquadrarotate_element_init);

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

static GstCaps *
gst_niquadrarotate_fixate_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GstNiQuadraRotate *self = GST_NIQUADRAROTATE (base);
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

    {
      gint par_n, par_d;
      if (gst_structure_get_fraction (format_struct, "pixel-aspect-ratio",
              &par_n, &par_d)) {
        gst_structure_set (fixated_struct, "pixel-aspect-ratio",
            GST_TYPE_FRACTION, par_n, par_d, NULL);
        GST_DEBUG_OBJECT (self, "preserved input pixel-aspect-ratio %d/%d",
            par_n, par_d);
      } else if (gst_structure_has_field (fixated_struct, "pixel-aspect-ratio")) {
        gst_structure_remove_field (fixated_struct, "pixel-aspect-ratio");
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
    }

    GST_DEBUG_OBJECT (self, "fixated othercaps %" GST_PTR_FORMAT
        " from caps %" GST_PTR_FORMAT, othercaps, caps);
  }

  return othercaps;
}

static void
gst_niquadrarotate_query_downstream_allocation (GstNiQuadraRotate * self,
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

static gboolean
gst_niquadrarotate_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstNiQuadraRotate *self = GST_NIQUADRAROTATE (trans);
  GstNiQuadraBaseConvert *parent = GST_NI_QUADRA_BASE_CONVERT (trans);

  if (!GST_BASE_TRANSFORM_CLASS (gst_niquadrarotate_parent_class)->set_caps
      (trans, incaps, outcaps)) {
    return FALSE;
  }

  if (self->auto_skip && !self->is_p2p && (self->angle == 0)
      && gst_video_info_is_equal (&parent->in_info, &parent->out_info)) {
    self->skip_filter = TRUE;
  } else {
    self->skip_filter = FALSE;
  }

  return TRUE;
}

static GstFlowReturn
gst_niquadrarotate_prepare_output_buffer (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer ** outbuf)
{
  GstNiQuadraRotate *self = GST_NIQUADRAROTATE (trans);

  if (self->skip_filter) {
    *outbuf = inbuf;
    return GST_FLOW_OK;
  }

  return GST_BASE_TRANSFORM_CLASS
      (gst_niquadrarotate_parent_class)->prepare_output_buffer (trans, inbuf,
      outbuf);
}

static GstFlowReturn
gst_niquadrarotate_transform (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer * outbuf)
{
  GstNiQuadraRotate *self = GST_NIQUADRAROTATE (trans);
  GstNiQuadraBaseConvert *parent = GST_NI_QUADRA_BASE_CONVERT (trans);
  ni_session_data_io_t *p_session_data = &self->api_dst_frame;
  GstMemory *in_mem = NULL;
  niFrameSurface1_t *in_surface = NULL;
  niFrameSurface1_t *new_frame_surface = NULL;
  GstMemory *out_mem = NULL;
  GstAllocator *alloc = NULL;
  gint dev_idx = -1;
  int gc620_pixfmt;
  ni_pix_fmt_t niPixFmt;
  int options = 0;
  int pool_size = DEFAULT_NI_FILTER_POOL_SIZE;
  int ret = 0;
  int rgba_color = 255;         /* black opaque */
  int aligned_picture_width, rotated_picture_width, rotated_picture_height;
  ni_frame_config_t input_frame_config = { 0 };

  if (self->skip_filter) {
    return GST_FLOW_OK;
  }

  memset (p_session_data, 0, sizeof (ni_session_data_io_t));

  in_mem = gst_buffer_peek_memory (inbuf, 0);
  dev_idx = gst_deviceid_from_ni_hw_memory (in_mem);
  in_surface = gst_surface_from_ni_hw_memory (in_mem);
  if (in_surface == NULL) {
    GST_ERROR
        ("Impossible to convert between the formats supported by the filter\n");
    return GST_FLOW_ERROR;
  }

  if (!parent->initialized) {
    GstCaps *out_caps = NULL;

    if (GST_VIDEO_INFO_FORMAT (&parent->out_info) == GST_VIDEO_FORMAT_BGRP) {
      GST_ERROR ("format BGRP not supported\n");
      return GST_FLOW_ERROR;
    }

    if (!strcmp (self->fillcolor_str, "none")) {
      self->fillcolor_enable = false;
    } else if (gst_parse_color (self->fillcolor, self->fillcolor_str, -1) >= 0) {
      self->fillcolor_enable = true;
    } else {
      GST_ERROR ("Don't support the color input %s", self->fillcolor_str);
      return GST_FLOW_ERROR;
    }

    ni_device_session_context_init (&parent->api_ctx);
    parent->api_ctx.session_id = NI_INVALID_SESSION_ID;
    parent->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
    parent->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;
    parent->api_ctx.hw_id = dev_idx;
    parent->api_ctx.device_type = NI_DEVICE_TYPE_SCALER;
    parent->api_ctx.scaler_operation = NI_SCALER_OPCODE_ROTATE;
    parent->api_ctx.keep_alive_timeout = parent->keep_alive_timeout;
    parent->api_ctx.isP2P = self->is_p2p;

    ret = ni_device_session_open (&parent->api_ctx, NI_DEVICE_TYPE_SCALER);
    if (ret < 0) {
      GST_ERROR ("Open rotate device session error\n");
      ni_device_session_close (&parent->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
      ni_device_session_context_clear (&parent->api_ctx);
      return GST_FLOW_ERROR;
    } else {
      GST_DEBUG ("XCoder %s.%d (inst: %d) opened successfully\n",
          parent->api_ctx.dev_xcoder_name, parent->api_ctx.hw_id,
          parent->api_ctx.session_id);
    }

    memset (&self->api_dst_frame, 0, sizeof (ni_session_data_io_t));

    options = NI_SCALER_FLAG_IO | NI_SCALER_FLAG_PC;
    if (parent->api_ctx.isP2P) {
      options |= NI_SCALER_FLAG_P2;
    }

    out_caps = gst_video_info_to_caps (&parent->out_info);
    gst_caps_set_features_simple (out_caps,
        gst_caps_features_from_string
        (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY));
    gst_niquadrarotate_query_downstream_allocation (self, trans, out_caps);
    gst_caps_unref (out_caps);

    if (parent->api_ctx.isP2P) {
      pool_size = 1;
    } else {
      /* If the downstream element is on a different card,
       * Allocate extra frames suggested by the downstream element */
      if (parent->api_ctx.hw_id != self->downstream_card) {
        pool_size += self->extra_frames;
        GST_INFO_OBJECT (self, "Increase frame pool by %d", self->extra_frames);
      }
    }

    niPixFmt =
        convertGstVideoFormatToNIPix (GST_VIDEO_INFO_FORMAT
        (&parent->out_info));
    gc620_pixfmt = convertNIPixToGC620Format (niPixFmt);

    /* Allocate a pool of frames by the scaler */
    ret = ni_device_alloc_frame (&parent->api_ctx, NI_ALIGN (GST_VIDEO_INFO_WIDTH (&parent->out_info), 2), NI_ALIGN (GST_VIDEO_INFO_HEIGHT (&parent->out_info), 2), gc620_pixfmt, options, 0,   // rec width
        0,                      // rec height
        0,                      // rec X pos
        0,                      // rec Y pos
        pool_size,              // rgba color/pool size
        0,                      // frame index
        NI_DEVICE_TYPE_SCALER);
    if (ret < 0) {
      GST_ERROR ("Init frame pool error\n");
      ni_device_session_context_clear (&parent->api_ctx);
      return GST_FLOW_ERROR;
    }

    parent->initialized = TRUE;
  }

  ret = ni_frame_buffer_alloc_hwenc (&p_session_data->data.frame,
      GST_VIDEO_INFO_WIDTH (&parent->in_info),
      GST_VIDEO_INFO_HEIGHT (&parent->in_info), 0);
  if (ret != NI_RETCODE_SUCCESS) {
    GST_ERROR ("Can't assign input frame %d\n", ret);
    return GST_FLOW_ERROR;
  }

  niPixFmt =
      convertGstVideoFormatToNIPix (GST_VIDEO_INFO_FORMAT (&parent->in_info));
  gc620_pixfmt = convertNIPixToGC620Format (niPixFmt);

  input_frame_config.picture_format = gc620_pixfmt;
  input_frame_config.rgba_color = in_surface->ui32nodeAddress;
  input_frame_config.frame_index = in_surface->ui16FrameIdx;

  aligned_picture_width =
      NI_ALIGN (GST_VIDEO_INFO_WIDTH (&parent->in_info),
      BUFFER_WIDTH_PIXEL_ALIGNMENT);

  if (self->angle == 0) {
    input_frame_config.orientation = 0;
    input_frame_config.picture_width = GST_VIDEO_INFO_WIDTH (&parent->in_info);
    input_frame_config.picture_height =
        GST_VIDEO_INFO_HEIGHT (&parent->in_info);

    input_frame_config.rectangle_width =
        ni_min (GST_VIDEO_INFO_WIDTH (&parent->out_info),
        GST_VIDEO_INFO_WIDTH (&parent->in_info));
    input_frame_config.rectangle_height =
        ni_min (GST_VIDEO_INFO_HEIGHT (&parent->out_info),
        GST_VIDEO_INFO_HEIGHT (&parent->in_info));

    rotated_picture_width = GST_VIDEO_INFO_WIDTH (&parent->in_info);
    rotated_picture_height = GST_VIDEO_INFO_HEIGHT (&parent->in_info);
  } else if ((self->angle == -270) || (self->angle == 90)) {
    input_frame_config.orientation = 1;
    input_frame_config.picture_width = aligned_picture_width;
    input_frame_config.picture_height =
        GST_VIDEO_INFO_HEIGHT (&parent->in_info);

    input_frame_config.rectangle_width =
        ni_min (GST_VIDEO_INFO_WIDTH (&parent->out_info),
        GST_VIDEO_INFO_HEIGHT (&parent->in_info));
    input_frame_config.rectangle_height =
        ni_min (GST_VIDEO_INFO_HEIGHT (&parent->out_info),
        GST_VIDEO_INFO_WIDTH (&parent->in_info));

    rotated_picture_width = GST_VIDEO_INFO_HEIGHT (&parent->in_info);
    rotated_picture_height = aligned_picture_width;
  } else if ((self->angle == -180) || (self->angle == 180)) {
    input_frame_config.orientation = 2;
    input_frame_config.picture_width = aligned_picture_width;
    input_frame_config.picture_height =
        GST_VIDEO_INFO_HEIGHT (&parent->in_info);

    input_frame_config.rectangle_width =
        ni_min (GST_VIDEO_INFO_WIDTH (&parent->out_info),
        GST_VIDEO_INFO_WIDTH (&parent->in_info));
    input_frame_config.rectangle_height =
        ni_min (GST_VIDEO_INFO_HEIGHT (&parent->out_info),
        GST_VIDEO_INFO_HEIGHT (&parent->in_info));

    rotated_picture_width = aligned_picture_width;
    rotated_picture_height = GST_VIDEO_INFO_HEIGHT (&parent->in_info);
  } else if ((self->angle == -90) || (self->angle == 270)) {
    input_frame_config.orientation = 3;
    input_frame_config.picture_width = aligned_picture_width;
    input_frame_config.picture_height =
        GST_VIDEO_INFO_HEIGHT (&parent->in_info);

    input_frame_config.rectangle_width =
        ni_min (GST_VIDEO_INFO_WIDTH (&parent->out_info),
        GST_VIDEO_INFO_HEIGHT (&parent->in_info));
    input_frame_config.rectangle_height =
        ni_min (GST_VIDEO_INFO_HEIGHT (&parent->out_info),
        GST_VIDEO_INFO_WIDTH (&parent->in_info));

    rotated_picture_width = GST_VIDEO_INFO_HEIGHT (&parent->in_info);
    rotated_picture_height = aligned_picture_width;
  } else {
    GST_ERROR ("ni rotate filter does not support rotation of %d radians",
        self->angle);
    return GST_FLOW_ERROR;
  }

  input_frame_config.rectangle_x =
      (rotated_picture_width > input_frame_config.rectangle_width) ?
      (rotated_picture_width / 2) -
      (input_frame_config.rectangle_width / 2) : 0;
  input_frame_config.rectangle_y = (rotated_picture_height >
      input_frame_config.rectangle_height) ? (rotated_picture_height / 2) -
      (input_frame_config.rectangle_height / 2) : 0;
  if (aligned_picture_width - GST_VIDEO_INFO_WIDTH (&parent->in_info)) {
    switch (input_frame_config.orientation) {
      case 1:                  // 90°
        input_frame_config.rectangle_y =
            (GST_VIDEO_INFO_WIDTH (&parent->in_info) >
            input_frame_config.rectangle_height)
            ? (GST_VIDEO_INFO_WIDTH (&parent->in_info) / 2) -
            (input_frame_config.rectangle_height / 2) : 0;
        break;
      case 2:                  // 180°
        input_frame_config.rectangle_x =
            aligned_picture_width - GST_VIDEO_INFO_WIDTH (&parent->in_info) +
            ((GST_VIDEO_INFO_WIDTH (&parent->in_info) >
                input_frame_config.rectangle_width)
            ? (GST_VIDEO_INFO_WIDTH (&parent->in_info) / 2) -
            (input_frame_config.rectangle_width / 2) : 0);
        break;
      case 3:                  // 270°
        input_frame_config.rectangle_y =
            aligned_picture_width - GST_VIDEO_INFO_WIDTH (&parent->in_info) +
            ((GST_VIDEO_INFO_WIDTH (&parent->in_info) >
                input_frame_config.rectangle_height)
            ? (GST_VIDEO_INFO_WIDTH (&parent->in_info) / 2) -
            (input_frame_config.rectangle_height / 2) : 0);
        break;
    }
  }

  ret = ni_device_config_frame (&parent->api_ctx, &input_frame_config);
  if (ret != NI_RETCODE_SUCCESS) {
    GST_ERROR ("ni rotate filter device config input frame failed with %d",
        ret);
    return GST_FLOW_ERROR;
  }

  if (self->fillcolor_enable) {
    rgba_color = (self->fillcolor[3] << 24) |
        (self->fillcolor[0] << 16) |
        (self->fillcolor[1] << 8) | self->fillcolor[2];
  }

  ni_frame_config_t *pconfig = &self->output_frame_config;
  pconfig->picture_width =
      NI_ALIGN (GST_VIDEO_INFO_WIDTH (&parent->out_info), 2);
  pconfig->picture_height =
      NI_ALIGN (GST_VIDEO_INFO_HEIGHT (&parent->out_info), 2);
  pconfig->rectangle_width = input_frame_config.rectangle_width;
  pconfig->rectangle_height = input_frame_config.rectangle_height;
  pconfig->rectangle_x = (pconfig->picture_width > pconfig->rectangle_width) ?
      (pconfig->picture_width / 2) - (pconfig->rectangle_width / 2) : 0;
  pconfig->rectangle_y = (pconfig->picture_height > pconfig->rectangle_height) ?
      (pconfig->picture_height / 2) - (pconfig->rectangle_height / 2) : 0;
  pconfig->rgba_color = rgba_color;

  ret = ni_device_alloc_frame (&parent->api_ctx,
      pconfig->picture_width,
      pconfig->picture_height,
      gc620_pixfmt,
      NI_SCALER_FLAG_IO,
      pconfig->rectangle_width,
      pconfig->rectangle_height,
      pconfig->rectangle_x,
      pconfig->rectangle_y, pconfig->rgba_color, -1, NI_DEVICE_TYPE_SCALER);
  if (ret != NI_RETCODE_SUCCESS) {
    GST_ERROR ("ni rotate filter device alloc output frame failed with %d",
        ret);
    return GST_FLOW_ERROR;
  }

  /* Set the new frame index */
  ret = ni_device_session_read_hwdesc (&parent->api_ctx, p_session_data,
      NI_DEVICE_TYPE_SCALER);
  if (ret != NI_RETCODE_SUCCESS) {
    GST_ERROR ("Can't acquire output frame %d", ret);
    return GST_FLOW_ERROR;
  }

  new_frame_surface =
      (niFrameSurface1_t *) p_session_data->data.frame.p_data[3];
  new_frame_surface->dma_buf_fd = 0;
  new_frame_surface->ui32nodeAddress = 0;
  new_frame_surface->ui16width =
      NI_ALIGN (GST_VIDEO_INFO_WIDTH (&parent->out_info), 2);
  new_frame_surface->ui16height =
      NI_ALIGN (GST_VIDEO_INFO_HEIGHT (&parent->out_info), 2);

  GST_DEBUG_OBJECT (self,
      "output negotiated %dx%d, hw surface %ux%u (aligned)",
      GST_VIDEO_INFO_WIDTH (&parent->out_info),
      GST_VIDEO_INFO_HEIGHT (&parent->out_info),
      new_frame_surface->ui16width, new_frame_surface->ui16height);

  niPixFmt =
      convertGstVideoFormatToNIPix (GST_VIDEO_INFO_FORMAT (&parent->out_info));
  ni_set_bit_depth_and_encoding_type (&new_frame_surface->bit_depth,
      &new_frame_surface->encoding_type, niPixFmt);

  alloc = gst_allocator_find (GST_NIQUADRA_MEMORY_TYPE_NAME);
  out_mem = gst_niquadra_allocator_alloc (alloc, &parent->api_ctx,
      new_frame_surface, parent->api_ctx.hw_id, &parent->out_info);
  gst_buffer_append_memory (outbuf, out_mem);
  gst_object_unref (alloc);

  ni_frame_buffer_free (&p_session_data->data.frame);

  return GST_FLOW_OK;
}

static void
gst_niquadrarotate_finalize (GObject * obj)
{
  GstNiQuadraRotate *self = GST_NIQUADRAROTATE (obj);
  GstNiQuadraBaseConvert *parent = GST_NI_QUADRA_BASE_CONVERT (obj);

  if (self->fillcolor_str) {
    g_free (self->fillcolor_str);
  }

  if (self->api_dst_frame.data.frame.p_buffer) {
    ni_frame_buffer_free (&self->api_dst_frame.data.frame);
  }

  if (parent->initialized) {
    if (parent->api_ctx.session_id != NI_INVALID_SESSION_ID) {
      GST_DEBUG ("libxcoder rotate free context\n");
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

  G_OBJECT_CLASS (gst_niquadrarotate_parent_class)->finalize (obj);
}

static void
gst_niquadrarotate_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiQuadraRotate *self;

  g_return_if_fail (GST_IS_NIQUADRAROTATE (object));
  self = GST_NIQUADRAROTATE (object);

  switch (prop_id) {
    case PROP_P2P:
      self->is_p2p = g_value_get_boolean (value);
      break;
    case PROP_COLOR:
      g_free (self->fillcolor_str);
      self->fillcolor_str = g_strdup (g_value_get_string (value));
      break;
    case PROP_ANGLE:
      self->angle = g_value_get_int (value);
      break;
    case PROP_AUTO_SKIP:
      self->auto_skip = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadrarotate_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiQuadraRotate *self;

  g_return_if_fail (GST_IS_NIQUADRAROTATE (object));
  self = GST_NIQUADRAROTATE (object);

  switch (prop_id) {
    case PROP_P2P:
      GST_OBJECT_LOCK (self);
      g_value_set_boolean (value, self->is_p2p);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_COLOR:
      GST_OBJECT_LOCK (self);
      g_value_take_string (value, g_strdup (self->fillcolor_str));
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_ANGLE:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->angle);
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
gst_niquadrarotate_class_init (GstNiQuadraRotateClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class->set_property = gst_niquadrarotate_set_property;
  gobject_class->get_property = gst_niquadrarotate_get_property;
  gobject_class->finalize = gst_niquadrarotate_finalize;

  g_object_class_install_property (gobject_class, PROP_P2P,
      g_param_spec_boolean ("is-p2p", "Is-P2P",
          "set p2p mode of the rotate", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_NAME |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_COLOR,
      g_param_spec_string ("color", "Color",
          "Set color of the rotate", "black",
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_ANGLE,
      g_param_spec_int ("angle", "Angle", "set angle of the rotate", G_MININT,
          G_MAXINT, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING
          | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_AUTO_SKIP,
      g_param_spec_boolean ("auto-skip", "Auto-Skip",
          "skip the filter when input and output of this filter are the same",
          FALSE,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (element_class, &src_factory);
  gst_element_class_add_static_pad_template (element_class, &sink_factory);

  gst_element_class_set_static_metadata (element_class,
      "NETINT Quadra ROTATE filter", "Filter/Effect/Video/NIRotate",
      "Rotate Netint Quadra", "Minglong Zhang<Minglong.Zhang@netint.cn>");

  trans_class->fixate_caps = gst_niquadrarotate_fixate_caps;
  trans_class->set_caps = gst_niquadrarotate_set_caps;
  trans_class->prepare_output_buffer = gst_niquadrarotate_prepare_output_buffer;
  trans_class->transform = gst_niquadrarotate_transform;
}

static gboolean
niquadrarotate_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadrarotate_debug, "niquadrarotate", 0,
      "niquadrarotate");

  return gst_element_register (plugin, "niquadrarotate", GST_RANK_NONE,
      GST_TYPE_NIQUADRAROTATE);
}

static void
gst_niquadrarotate_init (GstNiQuadraRotate * filter)
{
  filter->angle = 0;
  filter->is_p2p = FALSE;
  filter->fillcolor_str = g_strdup ("black");
  filter->fillcolor_enable = FALSE;
  filter->extra_frames = 0;
  filter->downstream_card = -1;
  filter->skip_filter = FALSE;
}
