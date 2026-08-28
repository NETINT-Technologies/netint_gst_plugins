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
 *  \file   GstNiQuadraDrawbox.c
 *
 *  \brief  Implement of NetInt Quadra hardware drawbox filter.
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

GST_DEBUG_CATEGORY_STATIC (gst_niquadradrawbox_debug);
#define GST_CAT_DEFAULT gst_niquadradrawbox_debug

enum
{
  PROP_0,
  PROP_FILTER_BLIT,
  GST_NIQUADRA_DEC_PROP_TIMEOUT,
  PROP_X,
  PROP_Y,
  PROP_W,
  PROP_H,
  PROP_X1,
  PROP_Y1,
  PROP_W1,
  PROP_H1,
  PROP_X2,
  PROP_Y2,
  PROP_W2,
  PROP_H2,
  PROP_X3,
  PROP_Y3,
  PROP_W3,
  PROP_H3,
  PROP_X4,
  PROP_Y4,
  PROP_W4,
  PROP_H4,
  PROP_P2P,
  PROP_COLOR,
  PROP_LAST
};

enum var_name
{
  VAR_DAR,
  VAR_IN_H, VAR_IH,
  VAR_IN_W, VAR_IW,
  VAR_SAR,
  VAR_X,
  VAR_Y,
  VAR_H,
  VAR_W,
  VAR_MAX,
  VARS_NB
};

enum
{ R, G, B, A };

typedef struct _GstNiQuadraDrawbox
{
  GstElement element;

  GstPad *sinkpad, *srcpad;

  GstVideoInfo info;
  gint width, height;
  GstVideoFormat format;

  ni_session_context_t api_ctx;
  ni_session_data_io_t api_dst_frame;
  ni_scaler_params_t params;

  int initialized;
  guint keep_alive_timeout;     /* keep alive timeout setting */
  bool is_p2p;
  /**
  * New dimensions. Special values are:
  *   0 = original width/height
  *  -1 = keep original aspect
  *  -N = try to keep aspect but make sure it is divisible by N
  */
  int box_x[NI_MAX_SUPPORT_DRAWBOX_NUM], box_y[NI_MAX_SUPPORT_DRAWBOX_NUM],
      box_w[NI_MAX_SUPPORT_DRAWBOX_NUM], box_h[NI_MAX_SUPPORT_DRAWBOX_NUM];
  unsigned char box_rgba_color[NI_MAX_SUPPORT_DRAWBOX_NUM][4];
  char *box_color_str[NI_MAX_SUPPORT_DRAWBOX_NUM];
  ni_scaler_multi_drawbox_params_t scaler_drawbox_paras;
  ni_frame_config_t frame_in;
  ni_frame_config_t frame_out;

  guint extra_frames;
  gint downstream_card;
} GstNiQuadraDrawbox;

typedef struct _GstNiQuadraDrawboxClass
{
  GstElementClass parent_class;
} GstNiQuadraDrawboxClass;

#define GST_TYPE_NIQUADRADRAWBOX \
  (gst_niquadradrawbox_get_type())
#define GST_NIQUADRADRAWBOX(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRADRAWBOX,GstNiQuadraDrawbox))
#define GST_NIQUADRADRAWBOX_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRADRAWBOX,GstNiQuadraDrawbox))
#define GST_IS_NIQUADRADRAWBOX(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRADRAWBOX))
#define GST_IS_NIQUADRADRAWBOX_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRADRAWBOX))

static gboolean niquadradrawbox_element_init (GstPlugin * plugin);

GType gst_niquadradrawboxniquadra_get_type (void);

G_DEFINE_TYPE (GstNiQuadraDrawbox, gst_niquadradrawbox, GST_TYPE_ELEMENT);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadradrawbox,
    niquadradrawbox_element_init);

#define SUPPORTED_FORMATS "{ ARGB, RGBA, ABGR, BGRA, BGRx }"

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

static GstFlowReturn gst_niquadra_drawbox_chain (GstPad * pad,
    GstObject * parent, GstBuffer * inbuf);

static gboolean
gst_niquadra_drawbox_sink_setcaps (GstPad * pad, GstObject * parent,
    GstCaps * caps)
{
  GstNiQuadraDrawbox *filter = GST_NIQUADRADRAWBOX (parent);
  GstStructure *structure = NULL;
  GstCaps *src_caps;
  gboolean ret, gotit = FALSE;
  GstVideoInfo *info = &filter->info;
  GstQuery *query;
  guint i;
  GType gtype;
  const GstStructure *params = NULL;

  structure = gst_caps_get_structure (caps, 0);
  if (!gst_structure_get_int (structure, "width", &filter->width))
    return FALSE;
  if (!gst_structure_get_int (structure, "height", &filter->height))
    return FALSE;

  if (!gst_video_info_from_caps (info, caps))
    return FALSE;

  /* Query the downstream element for proposed allocation */
  query = gst_query_new_allocation (caps, TRUE);

  if (gst_pad_peer_query (filter->srcpad, query) == TRUE) {
    /* Search for allocation metadata */
    for (i = 0; i < gst_query_get_n_allocation_metas (query); i++) {
      gtype = gst_query_parse_nth_allocation_meta (query, i, &params);
      if (gtype == GST_VIDEO_META_API_TYPE) {
        if (params
            && (strcmp (gst_structure_get_name (params),
                    NI_PREALLOCATE_STRUCTURE_NAME) == 0)) {

          gotit =
              gst_structure_get_uint (params, NI_VIDEO_META_BUFCNT,
              &filter->extra_frames);
          if (gotit == FALSE)
            GST_ERROR_OBJECT (filter, "Did not find buffercnt\n");

          gotit =
              gst_structure_get_int (params, NI_VIDEO_META_CARDNO,
              &filter->downstream_card);
          if (gotit == FALSE)
            GST_ERROR_OBJECT (filter, "Did not find cardno\n");

          break;
        }
      }
    }
  }

  if (info->colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255) {
    GST_DEBUG_OBJECT (filter,
        "WARNING: Full color range input, limited color output\n");
  }
  info->colorimetry.range = GST_VIDEO_COLOR_RANGE_16_235;

  gst_query_unref (query);

  filter->format = info->finfo->format;

  src_caps = gst_video_info_to_caps (info);
  gst_caps_set_features_simple (src_caps,
      gst_caps_features_from_string (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY));

  ret = gst_pad_set_caps (filter->srcpad, src_caps);
  gst_caps_unref (src_caps);

  return ret;
}

static gboolean
gst_niquadra_drawbox_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstNiQuadraDrawbox *filter = GST_NIQUADRADRAWBOX (parent);
  gboolean ret = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret = gst_niquadra_drawbox_sink_setcaps (pad, parent, caps);
      gst_event_unref (event);
      break;
    }
    default:
      ret = gst_pad_push_event (filter->srcpad, event);
      break;
  }

  return ret;
}

static void
gst_niquadradrawbox_init (GstNiQuadraDrawbox * filter)
{
  filter->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_event_function (filter->sinkpad, gst_niquadra_drawbox_sink_event);
  gst_pad_set_chain_function (filter->sinkpad, gst_niquadra_drawbox_chain);
  gst_element_add_pad (GST_ELEMENT (filter), filter->sinkpad);

  filter->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  gst_element_add_pad (GST_ELEMENT (filter), filter->srcpad);

  filter->params.filterblit = FALSE;
  filter->downstream_card = -1;
  filter->extra_frames = 0;
}

static void
gst_niquadradrawbox_dispose (GObject * obj)
{
  GstNiQuadraDrawbox *filter = GST_NIQUADRADRAWBOX (obj);
  if (filter->box_color_str[0])
    g_free (filter->box_color_str[0]);
  if (filter->api_dst_frame.data.frame.p_buffer) {
    ni_frame_buffer_free (&filter->api_dst_frame.data.frame);
  }
  if (filter->api_ctx.session_id != NI_INVALID_SESSION_ID) {
    GST_DEBUG_OBJECT (filter, "libxcoder drawbox free context\n");
    ni_device_session_close (&filter->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
  }
  ni_session_context_t *p_ctx = &filter->api_ctx;
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
  ni_device_session_context_clear (&filter->api_ctx);

  G_OBJECT_CLASS (gst_niquadradrawbox_parent_class)->dispose (obj);
}

static GstFlowReturn
gst_niquadra_drawbox_chain (GstPad * pad, GstObject * parent, GstBuffer * inbuf)
{
  GstNiQuadraDrawbox *filter = GST_NIQUADRADRAWBOX (parent);
  GstAllocator *alloc = NULL;
  GstMemory *in_mem = NULL;
  GstMemory *out_mem = NULL;
  GstBuffer *outbuf = NULL;
  niFrameSurface1_t *in_surface = NULL;
  gint dev_idx = -1;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  int retval = 0;
  double var_values[VARS_NB];
  niFrameSurface1_t *out_surface = NULL;
  ni_session_data_io_t *p_session_data = &filter->api_dst_frame;
  int gc620_pixfmt;
  ni_pix_fmt_t niPixFmt;
  int i, box_count = 0;

  memset (p_session_data, 0, sizeof (ni_session_data_io_t));

  in_mem = gst_buffer_peek_memory (inbuf, 0);
  dev_idx = gst_deviceid_from_ni_hw_memory (in_mem);
  in_surface = gst_surface_from_ni_hw_memory (in_mem);
  if (in_surface == NULL) {
    GST_ERROR_OBJECT (filter,
        "Impossible to convert between the formats supported by the filter\n");
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  if (!filter->initialized) {
    if (!(filter->format == GST_VIDEO_FORMAT_RGBA ||
            filter->format == GST_VIDEO_FORMAT_BGRA ||
            filter->format == GST_VIDEO_FORMAT_ARGB ||
            filter->format == GST_VIDEO_FORMAT_ABGR)) {
      GST_ERROR_OBJECT (filter,
          "nidrawbox format %d not supported\n", filter->format);
      gst_buffer_unref (inbuf);
      return GST_FLOW_ERROR;
    }
    uint8_t rgba_color[4];
    if (gst_parse_color (rgba_color, filter->box_color_str[0], -1) < 0) {
      GST_WARNING_OBJECT (filter, "invalid colorimetry, using default\n");
      // Default is black
      rgba_color[0] = rgba_color[1] = rgba_color[2] = 0;
      rgba_color[3] = 255;
    }
    filter->box_rgba_color[0][R] = rgba_color[0];
    filter->box_rgba_color[0][G] = rgba_color[1];
    filter->box_rgba_color[0][B] = rgba_color[2];
    filter->box_rgba_color[0][A] = rgba_color[3];

    ni_device_session_context_init (&filter->api_ctx);
    filter->api_ctx.session_id = NI_INVALID_SESSION_ID;
    filter->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
    filter->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;
    filter->api_ctx.hw_id = dev_idx;
    filter->api_ctx.device_type = NI_DEVICE_TYPE_SCALER;
    filter->api_ctx.scaler_operation = NI_SCALER_OPCODE_DRAWBOX;
    filter->api_ctx.keep_alive_timeout = filter->keep_alive_timeout;
    filter->api_ctx.isP2P = filter->is_p2p;

    retval = ni_device_session_open (&filter->api_ctx, NI_DEVICE_TYPE_SCALER);
    if (retval < 0) {
      GST_ERROR_OBJECT (filter,
          "Can't open device session on card %d\n", filter->api_ctx.hw_id);
      gst_buffer_unref (inbuf);
      ni_device_session_close (&filter->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
      ni_device_session_context_clear (&filter->api_ctx);
      return GST_FLOW_ERROR;
    } else {
      GST_DEBUG_OBJECT (filter,
          "XCoder %s.%d (inst: %d) opened successfully\n",
          filter->api_ctx.dev_xcoder_name, filter->api_ctx.hw_id,
          filter->api_ctx.session_id);
    }

    if (filter->params.filterblit) {
      retval = ni_scaler_set_params (&filter->api_ctx, &(filter->params));
      if (retval < 0) {
        GST_ERROR_OBJECT (filter, "Scale set filterblit fail\n");
        gst_buffer_unref (inbuf);
        return GST_FLOW_ERROR;
      }

    }
    memset (&filter->api_dst_frame, 0, sizeof (ni_session_data_io_t));

    int options;
    options = NI_SCALER_FLAG_IO | NI_SCALER_FLAG_PC;
    if (filter->api_ctx.isP2P) {
      options |= NI_SCALER_FLAG_P2;
    }
    int pool_size = 4;
    if (filter->api_ctx.isP2P) {
      pool_size = 1;
    } else {
      // If the downstream element is on a different card,
      // Allocate extra frames suggested by the downstream element
      if (filter->api_ctx.hw_id != filter->downstream_card) {
        pool_size += filter->extra_frames;
        GST_INFO_OBJECT (filter,
            "Increase frame pool by %d\n", filter->extra_frames);
      }
    }
    niPixFmt = convertGstVideoFormatToNIPix (filter->format);
    gc620_pixfmt = convertNIPixToGC620Format (niPixFmt);

    /* Allocate a pool of frames by the scaler */
    /* *INDENT-OFF* */
    retval = ni_device_alloc_frame (&filter->api_ctx,
      NI_ALIGN (filter->width, 2),
      NI_ALIGN (filter->height, 2),
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
      GST_ERROR_OBJECT (filter, "Init frame pool error\n");
      gst_buffer_unref (inbuf);
      ni_device_session_context_clear (&filter->api_ctx);
      return GST_FLOW_ERROR;
    }

    filter->initialized = 1;
  }

  niPixFmt = convertGstVideoFormatToNIPix (filter->format);
  gc620_pixfmt = convertNIPixToGC620Format (niPixFmt);

  retval = ni_frame_buffer_alloc_hwenc (&p_session_data->data.frame,
      filter->width, filter->height, 0);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "Can't assign input frame %d\n", retval);
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  var_values[VAR_IN_H] = var_values[VAR_IH] = filter->height;
  var_values[VAR_IN_W] = var_values[VAR_IW] = filter->width;
  var_values[VAR_X] = 0;
  var_values[VAR_Y] = 0;
  var_values[VAR_H] = 0;
  var_values[VAR_W] = 0;
  memset (&filter->scaler_drawbox_paras, 0,
      sizeof (filter->scaler_drawbox_paras));

  for (i = 0; i < NI_MAX_SUPPORT_DRAWBOX_NUM; i++) {
    /* evaluate expressions, fail on last iteration */
    var_values[VAR_MAX] = filter->width;
    filter->box_x[i] = var_values[VAR_X] =
        ((filter->box_x[i] <
            var_values[VAR_MAX]) ? ((filter->box_x[i] <
                0) ? 0 : filter->box_x[i]) : (var_values[VAR_MAX] - 1));


    var_values[VAR_MAX] = filter->height;
    filter->box_y[i] = var_values[VAR_Y] =
        ((filter->box_y[i] <
            var_values[VAR_MAX]) ? ((filter->box_y[i] <
                0) ? 0 : filter->box_y[i]) : (var_values[VAR_MAX] - 1));


    var_values[VAR_MAX] = filter->width - filter->box_x[i];
    filter->box_w[i] = var_values[VAR_W] =
        ((filter->box_w[i] <
            var_values[VAR_MAX]) ? filter->box_w[i] : var_values[VAR_MAX]);
    filter->box_w[i] =
        (filter->box_w[i] >= 0) ? filter->box_w[i] : var_values[VAR_MAX];


    var_values[VAR_MAX] = filter->height - filter->box_y[i];
    filter->box_h[i] = var_values[VAR_H] =
        ((filter->box_h[i] <
            var_values[VAR_MAX]) ? filter->box_h[i] : var_values[VAR_MAX]);

    filter->box_h[i] =
        (filter->box_h[i] >= 0) ? filter->box_h[i] : var_values[VAR_MAX];
    /* sanity check width and height */
    if (filter->box_w[i] < 0 || filter->box_h[i] < 0) {
      GST_ERROR_OBJECT (filter,
          "Size values less than 0 are not acceptable.\n");
      gst_buffer_unref (inbuf);
      return GST_FLOW_ERROR;
    }
    // please use drawbox->scaler_drawbox_paras to pass draw parameters
    GST_DEBUG_OBJECT (filter,
        "Can't assign input frame %d: x %d, y %d, w %d, h %d, color %x\n",
        i, filter->box_x[i], filter->box_y[i],
        filter->box_w[i], filter->box_h[i],
        filter->box_rgba_color[i][0] + (filter->box_rgba_color[i][1] << 8) +
        (filter->box_rgba_color[i][2] << 16) +
        (filter->box_rgba_color[i][3] << 24));

    if ((filter->box_w[i] > 0) && (filter->box_h[i] > 0)) {
      filter->scaler_drawbox_paras.multi_drawbox_params[box_count].start_x =
          filter->box_x[i];
      filter->scaler_drawbox_paras.multi_drawbox_params[box_count].start_y =
          filter->box_y[i];
      filter->scaler_drawbox_paras.multi_drawbox_params[box_count].end_x =
          filter->box_x[i] + filter->box_w[i] - 1;
      filter->scaler_drawbox_paras.multi_drawbox_params[box_count].end_y =
          filter->box_y[i] + filter->box_h[i] - 1;
      filter->scaler_drawbox_paras.multi_drawbox_params[box_count].rgba_c =
          filter->box_rgba_color[0][B] + (filter->box_rgba_color[0][G] << 8) +
          (filter->box_rgba_color[0][R] << 16) +
          (filter->box_rgba_color[0][A] << 24);
      if ((filter->box_w[i] > 0) && (filter->box_h[i] > 0))
        box_count++;
    }
  }

  retval = ni_scaler_set_drawbox_params (&filter->api_ctx,
      &filter->scaler_drawbox_paras.multi_drawbox_params[0]);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "Can't set drawbox params\n");
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  filter->frame_in.picture_width = NI_ALIGN (filter->width, 2);
  filter->frame_in.picture_height = NI_ALIGN (filter->height, 2);
  filter->frame_in.picture_format = gc620_pixfmt;
  filter->frame_in.session_id = in_surface->ui16session_ID;
  filter->frame_in.output_index = in_surface->output_idx;
  filter->frame_in.frame_index = in_surface->ui16FrameIdx;

  /*
   * Config device input frame parameters
   */
  retval = ni_device_config_frame (&filter->api_ctx, &filter->frame_in);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "Can't allocate device input frame %d\n", retval);
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  niPixFmt = convertGstVideoFormatToNIPix (filter->format);
  gc620_pixfmt = convertNIPixToGC620Format (niPixFmt);
  filter->frame_out.picture_width = filter->width;
  filter->frame_out.picture_height = filter->height;
  filter->frame_out.picture_format = gc620_pixfmt;

  /*
   * Allocate hardware device destination frame. This acquires a frame
   * from the pool
   */
  /* *INDENT-OFF* */
  retval = ni_device_alloc_frame (&filter->api_ctx,
      NI_ALIGN (filter->width, 2),
      NI_ALIGN (filter->height, 2),
      gc620_pixfmt,
      NI_SCALER_FLAG_IO,
      0,
      0,
      0,
      0,
      0,
      -1,
      NI_DEVICE_TYPE_SCALER);
  /* *INDENT-ON* */

  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter,
        "Can't allocate device output frame %d\n", retval);
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  /* Set the new frame index */
  retval = ni_device_session_read_hwdesc (&filter->api_ctx, p_session_data,
      NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    GST_ERROR_OBJECT (filter, "Can't acquire output frame %d\n", retval);
    gst_buffer_unref (inbuf);
    return GST_FLOW_ERROR;
  }

  out_surface = (niFrameSurface1_t *) p_session_data->data.frame.p_data[3];
  out_surface->dma_buf_fd = 0;
  out_surface->ui32nodeAddress = 0;
  out_surface->ui16width = filter->width;
  out_surface->ui16height = filter->height;

  ni_set_bit_depth_and_encoding_type (&out_surface->bit_depth,
      &out_surface->encoding_type, niPixFmt);

  outbuf = gst_buffer_new ();

  alloc = gst_allocator_find (GST_NIQUADRA_MEMORY_TYPE_NAME);
  out_mem = gst_niquadra_allocator_alloc (alloc, &filter->api_ctx, out_surface,
      filter->api_ctx.hw_id, &filter->info);
  gst_buffer_append_memory (outbuf, out_mem);
  gst_object_unref (alloc);

  ni_frame_buffer_free (&p_session_data->data.frame);

  outbuf = gst_buffer_make_writable (outbuf);
  gst_buffer_copy_into (outbuf, inbuf,
      GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_META, 0, -1);
  gst_buffer_unref (inbuf);
  flow_ret = gst_pad_push (filter->srcpad, outbuf);

  return flow_ret;
}

static void
gst_niquadradrawbox_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiQuadraDrawbox *self;

  g_return_if_fail (GST_IS_NIQUADRADRAWBOX (object));
  self = GST_NIQUADRADRAWBOX (object);

  switch (prop_id) {
    case PROP_X:
      self->box_x[0] = g_value_get_int (value);
      break;
    case PROP_Y:
      self->box_y[0] = g_value_get_int (value);
      break;
    case PROP_W:
      self->box_w[0] = g_value_get_int (value);
      break;
    case PROP_H:
      self->box_h[0] = g_value_get_int (value);
      break;
    case PROP_X1:
      self->box_x[1] = g_value_get_int (value);
      break;
    case PROP_Y1:
      self->box_y[1] = g_value_get_int (value);
      break;
    case PROP_W1:
      self->box_w[1] = g_value_get_int (value);
      break;
    case PROP_H1:
      self->box_h[1] = g_value_get_int (value);
      break;
    case PROP_X2:
      self->box_x[2] = g_value_get_int (value);
      break;
    case PROP_Y2:
      self->box_y[2] = g_value_get_int (value);
      break;
    case PROP_W2:
      self->box_w[2] = g_value_get_int (value);
      break;
    case PROP_H2:
      self->box_h[2] = g_value_get_int (value);
      break;
    case PROP_X3:
      self->box_x[3] = g_value_get_int (value);
      break;
    case PROP_Y3:
      self->box_y[3] = g_value_get_int (value);
      break;
    case PROP_W3:
      self->box_w[3] = g_value_get_int (value);
      break;
    case PROP_H3:
      self->box_h[3] = g_value_get_int (value);
      break;
    case PROP_X4:
      self->box_x[4] = g_value_get_int (value);
      break;
    case PROP_Y4:
      self->box_y[4] = g_value_get_int (value);
      break;
    case PROP_W4:
      self->box_w[4] = g_value_get_int (value);
      break;
    case PROP_H4:
      self->box_h[4] = g_value_get_int (value);
      break;
    case PROP_P2P:
      self->is_p2p = g_value_get_boolean (value);
      break;
    case PROP_COLOR:
      g_free (self->box_color_str[0]);
      self->box_color_str[0] = g_strdup (g_value_get_string (value));
      break;
    case PROP_FILTER_BLIT:
      self->params.filterblit = g_value_get_boolean (value);
      break;
    case GST_NIQUADRA_DEC_PROP_TIMEOUT:
      self->keep_alive_timeout = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadradrawbox_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiQuadraDrawbox *self;

  g_return_if_fail (GST_IS_NIQUADRADRAWBOX (object));
  self = GST_NIQUADRADRAWBOX (object);

  switch (prop_id) {
    case PROP_X:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_x[0]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_Y:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_y[0]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_W:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_w[0]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_H:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_h[0]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_X1:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_x[1]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_Y1:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_y[1]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_W1:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_w[1]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_H1:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_h[1]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_X2:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_x[2]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_Y2:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_y[2]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_W2:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_w[2]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_H2:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_h[2]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_X3:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_x[3]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_Y3:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_y[3]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_W3:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_w[3]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_H3:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_h[3]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_X4:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_x[4]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_Y4:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_y[4]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_W4:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_w[4]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_H4:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->box_h[4]);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_P2P:
      GST_OBJECT_LOCK (self);
      g_value_set_boolean (value, self->is_p2p);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_COLOR:
      GST_OBJECT_LOCK (self);
      g_value_take_string (value, g_strdup (self->box_color_str[0]));
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_FILTER_BLIT:
      GST_OBJECT_LOCK (self);
      g_value_set_boolean (value, self->params.filterblit);
      GST_OBJECT_UNLOCK (self);
      break;
    case GST_NIQUADRA_DEC_PROP_TIMEOUT:
      GST_OBJECT_LOCK (self);
      g_value_set_uint (value, self->keep_alive_timeout);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadradrawbox_class_init (GstNiQuadraDrawboxClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gobject_class->set_property = gst_niquadradrawbox_set_property;
  gobject_class->get_property = gst_niquadradrawbox_get_property;

  g_object_class_install_property (gobject_class, PROP_X,
      g_param_spec_int ("x", "X",
          "set x of the box", 0,
          G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_Y,
      g_param_spec_int ("y", "Y",
          "set y of the box", 0,
          G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_H,
      g_param_spec_int ("h", "H",
          "set height of the box",
          0, G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_W,
      g_param_spec_int ("w", "W",
          "set width of the box",
          0, G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_X1,
      g_param_spec_int ("x1", "X1",
          "set x1 of the box", 0,
          G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_Y1,
      g_param_spec_int ("y1", "Y1",
          "set y1 of the box", 0,
          G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_H1,
      g_param_spec_int ("h1", "H1",
          "set h1 of the box", 0,
          G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_W1,
      g_param_spec_int ("w1", "W1",
          "set w1 of the box", 0,
          G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_X2,
      g_param_spec_int ("x2", "X2",
          "set x2 of the box", 0,
          G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_Y2,
      g_param_spec_int ("y2", "Y2",
          "set y2 of the box", 0,
          G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_H2,
      g_param_spec_int ("h2", "H2",
          "set h2 of the box", 0,
          G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_W2,
      g_param_spec_int ("w2", "W2",
          "set w2 of the box", 0,
          G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_X3,
      g_param_spec_int ("x3", "X3",
          "set x3 of the box", 0,
          G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_Y3,
      g_param_spec_int ("y3", "Y3",
          "set y3 of the box", 0,
          G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_H3,
      g_param_spec_int ("h3", "H3",
          "set h3 of the box", 0,
          G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_W3,
      g_param_spec_int ("w3", "W3",
          "set w3 of the box", 0,
          G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_X4,
      g_param_spec_int ("x4", "X4",
          "set x4 of the box", 0,
          G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_Y4,
      g_param_spec_int ("y4", "Y4",
          "set y4 of the box", 0,
          G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_H4,
      g_param_spec_int ("h4", "H4",
          "set h4 of the box", 0,
          G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_W4,
      g_param_spec_int ("w4", "W4",
          "set w4 of the box", 0,
          G_MAXINT, 0,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_P2P,
      g_param_spec_boolean ("is-p2p", "Is-P2P",
          "set p2p mode of the drawbox",
          FALSE,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_NAME |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class, PROP_COLOR,
      g_param_spec_string ("color", "Color",
          "Set color of the box",
          "black",
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_FILTER_BLIT,
      g_param_spec_boolean ("filterblit",
          "Filter_blit",
          "Whether to use the higher quality scaling",
          FALSE,
          G_PARAM_READWRITE |
          G_PARAM_STATIC_NAME |
          GST_PARAM_MUTABLE_PLAYING | GST_PARAM_CONTROLLABLE));
  g_object_class_install_property (gobject_class,
      GST_NIQUADRA_DEC_PROP_TIMEOUT,
      g_param_spec_uint ("keep-alive-timeout",
          "TIMEOUT",
          "Specify a custom session keep alive timeout in seconds. Defult is 3.",
          NI_MIN_KEEP_ALIVE_TIMEOUT, NI_MAX_KEEP_ALIVE_TIMEOUT,
          NI_DEFAULT_KEEP_ALIVE_TIMEOUT,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (element_class, &src_factory);
  gst_element_class_add_static_pad_template (element_class, &sink_factory);

  gst_element_class_set_static_metadata (element_class,
      "NETINT Quadra DRAWBOX filter",
      "Filter/Effect/Video/NIDrawbox",
      "Drawbox Netint Quadra", "Minglong Zhang<Minglong.Zhang@netint.cn>");

  gobject_class->dispose = gst_niquadradrawbox_dispose;
}

static gboolean
niquadradrawbox_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadradrawbox_debug, "niquadradrawbox", 0,
      "niquadradrawbox");

  return gst_element_register (plugin, "niquadradrawbox", GST_RANK_NONE,
      GST_TYPE_NIQUADRADRAWBOX);
}
