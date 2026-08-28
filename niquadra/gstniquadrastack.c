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
 *  \file   gstniquadrastack.c
 *
 *  \brief  Implement of NetInt Quadra hardware stack filter.
 ******************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstniquadrastack.h"
#include "niquadra.h"
#include "gstniquadramemory.h"
#include "gstniquadrautils.h"

GST_DEBUG_CATEGORY_STATIC (gst_niquadrastack_debug);
#define GST_CAT_DEFAULT gst_niquadrastack_debug

#define SUPPORTED_FORMATS \
    "{ I420, YUY2, UYVY, NV12, ARGB, RGBA, ABGR, BGRA, I420_10LE, P010_10LE, " \
    "NV16, BGRx, NV12_10LE32 }"

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink_%u",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY, SUPPORTED_FORMATS))
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY, SUPPORTED_FORMATS))
    );

static void gst_niquadrastack_child_proxy_init (gpointer g_iface,
    gpointer iface_data);

G_DEFINE_TYPE (GstNiQuadraStackPad, gst_niquadrastack_pad,
    GST_TYPE_VIDEO_AGGREGATOR_CONVERT_PAD);

static void
gst_niquadrastack_pad_class_init (GstNiQuadraStackPadClass * klass)
{
  GstVideoAggregatorPadClass *vaggpad_class =
      GST_VIDEO_AGGREGATOR_PAD_CLASS (klass);
  vaggpad_class->prepare_frame = NULL;
  vaggpad_class->clean_frame = NULL;
}

static void
gst_niquadrastack_pad_init (GstNiQuadraStackPad * pad)
{
}

enum
{
  PROP_0,
  PROP_INPUTS,
  PROP_LAYOUT,
  PROP_SIZE,
  PROP_SHORTEST,
  PROP_FILL,
  PROP_SYNC,
  PROP_IS_P2P,
  PROP_KEEP_ALIVE_TIMEOUT,
  PROP_LAST
};

static char *
gst_strtok (char *s, const char *delim, char **saveptr)
{
  char *tok;

  if (!s && !(s = *saveptr))
    return NULL;

  /* skip leading delimiters */
  s += strspn (s, delim);

  /* s now points to the first non delimiter char, or to the end of the string */
  if (!*s) {
    *saveptr = NULL;
    return NULL;
  }
  tok = s++;

  /* skip non delimiters */
  s += strcspn (s, delim);
  if (*s) {
    *s = 0;
    *saveptr = s + 1;
  } else {
    *saveptr = NULL;
  }

  return tok;
}

static void
gst_niquadrastack_cleanup_item (stack_item * item)
{
  if (item->last_buff) {
    gst_buffer_unref (item->last_buff);
    item->last_buff = NULL;
  }

  item->got_first_buffer = FALSE;
}

static void
gst_niquadrastack_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiQuadraStack *self;

  g_return_if_fail (GST_IS_NIQUADRASTACK (object));
  self = GST_NIQUADRASTACK (object);

  switch (prop_id) {
    case PROP_INPUTS:
      g_value_set_int (value, self->nb_inputs);
      break;
    case PROP_LAYOUT:
      g_value_take_string (value, g_strdup (self->layout));
      break;
    case PROP_SIZE:
      g_value_take_string (value, g_strdup (self->size));
      break;
    case PROP_SHORTEST:
      g_value_set_boolean (value, self->shortest);
      break;
    case PROP_FILL:
      g_value_take_string (value, g_strdup (self->fillcolor_str));
      break;
    case PROP_SYNC:
      g_value_set_int (value, self->sync);
      break;
    case PROP_IS_P2P:
      g_value_set_boolean (value, self->is_p2p);
      break;
    case PROP_KEEP_ALIVE_TIMEOUT:
      g_value_set_uint (value, self->keep_alive_timeout);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
gst_niquadrastack_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiQuadraStack *self;
  gboolean support_reconfigure = FALSE;

  g_return_if_fail (GST_IS_NIQUADRASTACK (object));
  self = GST_NIQUADRASTACK (object);

  switch (prop_id) {
    case PROP_INPUTS:
      if (self->initialized) {
        self->nb_inputs_tmp = g_value_get_int (value);
      } else {
        self->nb_inputs = g_value_get_int (value);
      }
      support_reconfigure = TRUE;
      break;
    case PROP_LAYOUT:
      if (self->initialized) {
        g_free (self->layout_tmp);
        self->layout_tmp = g_value_dup_string (value);
      } else {
        g_free (self->layout);
        self->layout = g_value_dup_string (value);
      }
      support_reconfigure = TRUE;
      break;
    case PROP_SIZE:
      if (self->initialized) {
        g_free (self->size_tmp);
        self->size_tmp = g_value_dup_string (value);
      } else {
        g_free (self->size);
        self->size = g_value_dup_string (value);
      }
      support_reconfigure = TRUE;
      break;
    case PROP_SHORTEST:
      self->shortest = g_value_get_boolean (value);
      break;
    case PROP_FILL:
      g_free (self->fillcolor_str);
      self->fillcolor_str = g_value_dup_string (value);
      break;
    case PROP_SYNC:
      self->sync = g_value_get_int (value);
      break;
    case PROP_IS_P2P:
      self->is_p2p = g_value_get_boolean (value);
      break;
    case PROP_KEEP_ALIVE_TIMEOUT:
      self->keep_alive_timeout = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }

  if (self->initialized && !self->is_mark_reconfigure && support_reconfigure) {
    self->is_mark_reconfigure = TRUE;
  }
}

static gboolean niquadrastack_element_init (GstPlugin * plugin);

#define gst_niquadrastack_parent_class parent_class

G_DEFINE_TYPE_WITH_CODE (GstNiQuadraStack, gst_niquadrastack,
    GST_TYPE_VIDEO_AGGREGATOR, G_IMPLEMENT_INTERFACE (GST_TYPE_CHILD_PROXY,
        gst_niquadrastack_child_proxy_init));

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadrastack, niquadrastack_element_init);

static GstPad *
gst_niquadrastack_request_new_pad (GstElement * element, GstPadTemplate * templ,
    const gchar * req_name, const GstCaps * caps)
{
  GstPad *newpad;

  newpad = (GstPad *)
      GST_ELEMENT_CLASS (parent_class)->request_new_pad (element,
      templ, req_name, caps);

  if (newpad == NULL)
    goto could_not_create;

  gst_child_proxy_child_added (GST_CHILD_PROXY (element), G_OBJECT (newpad),
      GST_OBJECT_NAME (newpad));

  return newpad;

could_not_create:
  {
    GST_DEBUG_OBJECT (element, "could not create/add pad");
    return NULL;
  }
}

static void
gst_niquadrastack_release_pad (GstElement * element, GstPad * pad)
{
  GstNiQuadraStack *self;

  self = GST_NIQUADRASTACK (element);

  GST_DEBUG_OBJECT (self, "release pad %s:%s", GST_DEBUG_PAD_NAME (pad));

  if (self->items) {
    for (int i = 0; i < self->nb_inputs; i++) {
      if (self->items[i].pad == pad) {
        gst_niquadrastack_cleanup_item (&self->items[i]);
        break;
      }
    }
  }

  gst_child_proxy_child_removed (GST_CHILD_PROXY (self), G_OBJECT (pad),
      GST_OBJECT_NAME (pad));

  GST_ELEMENT_CLASS (parent_class)->release_pad (element, pad);
}

static GstCaps *
gst_niquadrastack_fixate_caps (GstAggregator * agg, GstCaps * caps)
{
  GstVideoAggregator *vagg = GST_VIDEO_AGGREGATOR (agg);
  GstNiQuadraStack *self = GST_NIQUADRASTACK (vagg);
  GList *l;
  int height, width;
  GstCaps *out = NULL;
  GstStructure *s;
  char *arg, *p, *saveptr = NULL;
  char *arg2, *p2, *saveptr2 = NULL;
  char *arg3, *p3, *saveptr3 = NULL;
  gint fps_n = 1, fps_d = 1;
  int inw, inh, size;
  int i;

  GST_OBJECT_LOCK (vagg);

  if (self->initialized) {
    if (self->is_mark_reconfigure) {
      self->is_mark_reconfigure = FALSE;
    }

    if (self->items) {
      for (i = 0; i < self->nb_inputs; i++) {
        gst_niquadrastack_cleanup_item (&self->items[i]);
      }
    }

    if (self->api_dst_frame.data.frame.p_buffer) {
      ni_frame_buffer_free (&self->api_dst_frame.data.frame);
    }

    if (self->api_ctx.session_id != NI_INVALID_SESSION_ID) {
      ni_device_session_close (&self->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
    }

    ni_device_session_context_clear (&self->api_ctx);

    if (self->items) {
      free (self->items);
      self->items = NULL;
    }
    if (self->in_buffers) {
      free (self->in_buffers);
      self->in_buffers = NULL;
    }

    if (self->nb_inputs_tmp > 0) {
      self->nb_inputs = self->nb_inputs_tmp;
      self->nb_inputs_tmp = -1;
    }

    if (self->layout_tmp) {
      g_free (self->layout);
      self->layout = g_strdup (self->layout_tmp);
      g_free (self->layout_tmp);
      self->layout_tmp = NULL;
    }

    if (self->size_tmp) {
      g_free (self->size);
      self->size = g_strdup (self->size_tmp);
      g_free (self->size_tmp);
      self->size_tmp = NULL;
    }

    self->initialized = FALSE;
  }

  if (strcmp (self->fillcolor_str, "none") &&
      gst_parse_color (self->fillcolor, self->fillcolor_str, -1) >= 0) {
    self->fillcolor_enable = 1;
  } else {
    self->fillcolor_enable = 0;
  }

  if (!self->layout) {
    if (self->nb_inputs == 2) {
      self->layout = g_strdup ("0_0|w0_0");
    } else {
      GST_ERROR_OBJECT (self, "No layout specified");
      return NULL;
    }
  }

  if (!self->items) {
    self->items = calloc (self->nb_inputs, sizeof (*self->items));
    if (!self->items) {
      return NULL;
    }
  }

  if (!self->in_buffers) {
    self->in_buffers = calloc (self->nb_inputs, sizeof (*self->in_buffers));
    if (!self->in_buffers) {
      return NULL;
    }
  }

  i = 0;
  for (l = GST_ELEMENT (vagg)->sinkpads; l; l = l->next) {
    GstVideoAggregatorPad *vaggpad = l->data;
    stack_item *item = &self->items[i];

    item->pad = GST_PAD (vaggpad);

    item->inw = GST_VIDEO_INFO_WIDTH (&vaggpad->info);
    item->inh = GST_VIDEO_INFO_HEIGHT (&vaggpad->info);
    item->fmt = GST_VIDEO_INFO_FORMAT (&vaggpad->info);

    if (i == self->sync) {
      fps_n = GST_VIDEO_INFO_FPS_N (&vaggpad->info);
      fps_d = GST_VIDEO_INFO_FPS_D (&vaggpad->info);
      self->out_pixfmt = item->fmt;
    }

    i++;
  }

  if (self->size == NULL) {
    i = 0;
    for (l = GST_ELEMENT (vagg)->sinkpads; l; l = l->next) {
      GstVideoAggregatorPad *vaggpad = l->data;
      stack_item *item = &self->items[i++];

      item->w = GST_VIDEO_INFO_WIDTH (&vaggpad->info);
      item->w = NI_ALIGN (item->w, 2);
      item->h = GST_VIDEO_INFO_HEIGHT (&vaggpad->info);
      item->h = NI_ALIGN (item->h, 2);
    }
  } else {
    gchar *str_size = g_strdup (self->size);
    p = str_size;
    for (i = 0; i < self->nb_inputs; i++) {
      stack_item *item = &self->items[i];

      if (!(arg = gst_strtok (p, "|", &saveptr)))
        goto EXIT;

      p = NULL;

      p2 = arg;
      inw = inh = 0;

      for (int j = 0; j < 2; j++) {
        if (!(arg2 = gst_strtok (p2, "_", &saveptr2)))
          goto EXIT;

        p2 = NULL;
        p3 = arg2;

        while ((arg3 = gst_strtok (p3, "+", &saveptr3))) {
          p3 = NULL;
          if (sscanf (arg3, "%d", &size) == 1) {
            if (size < 0)
              goto EXIT;

            if (!j)
              inw += size;
            else
              inh += size;
          } else {
            goto EXIT;
          }
        }
      }

      item->w = NI_ALIGN (inw, 2);
      item->h = NI_ALIGN (inh, 2);
    }
    g_free (str_size);
  }

  width = 0;
  height = 0;
  saveptr = NULL;
  saveptr2 = NULL;
  saveptr3 = NULL;
  gchar *str_layout = g_strdup (self->layout);
  p = str_layout;

  for (i = 0; i < self->nb_inputs; i++) {
    stack_item *item = &self->items[i];

    if (!(arg = gst_strtok (p, "|", &saveptr)))
      goto EXIT;

    p = NULL;

    p2 = arg;
    inw = inh = 0;

    for (int j = 0; j < 2; j++) {
      if (!(arg2 = gst_strtok (p2, "_", &saveptr2)))
        goto EXIT;

      p2 = NULL;
      p3 = arg2;
      while ((arg3 = gst_strtok (p3, "+", &saveptr3))) {
        p3 = NULL;
        if (sscanf (arg3, "w%d", &size) == 1) {
          if (size < 0 || size >= self->nb_inputs)
            goto EXIT;

          if (!j)
            inw += self->items[size].w;
          else
            inh += self->items[size].w;
        } else if (sscanf (arg3, "h%d", &size) == 1) {
          if (size < 0 || size >= self->nb_inputs)
            goto EXIT;

          if (!j)
            inw += self->items[size].h;
          else
            inh += self->items[size].h;
        } else if (sscanf (arg3, "%d", &size) == 1) {
          if (size < 0)
            goto EXIT;

          if (!j)
            inw += size;
          else
            inh += size;
        } else {
          goto EXIT;
        }
      }
    }

    item->x = NI_ALIGN (inw, 2);
    item->y = NI_ALIGN (inh, 2);

    width = ni_max (width, item->w + inw);
    height = ni_max (height, item->h + inh);
  }

  g_free (str_layout);

  GST_OBJECT_UNLOCK (vagg);

  self->out_width = width;
  self->out_height = height;

  out = gst_caps_make_writable (caps);

  s = gst_caps_get_structure (out, 0);
  if (gst_structure_has_field (s, "pixel-aspect-ratio")) {
    gst_structure_fixate_field_nearest_fraction (s, "pixel-aspect-ratio", 1, 1);
  }

  gst_structure_set (s, "width", G_TYPE_INT, width, NULL);
  gst_structure_set (s, "height", G_TYPE_INT, height, NULL);
  gst_structure_set (s, "format", G_TYPE_STRING,
      gst_video_format_to_string (self->out_pixfmt), NULL);

  gst_caps_set_features_simple (out,
      gst_caps_features_from_string (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY));

  GValue target = { 0 };
  g_value_init (&target, GST_TYPE_FRACTION);
  gst_value_set_fraction (&target, fps_n, fps_d);
  gst_structure_set_value (s, "framerate", &target);

  out = gst_caps_fixate (out);
  return out;

EXIT:
  GST_OBJECT_UNLOCK (vagg);
  return NULL;
}

static GstFlowReturn
gst_niquadrastack_update_src_caps (GstAggregator * agg, GstCaps * caps,
    GstCaps ** ret)
{
  GstVideoAggregator *vagg = GST_VIDEO_AGGREGATOR (agg);
  GstNiQuadraStack *self = GST_NIQUADRASTACK (agg);
  GList *l;
  guint pad_count = 0;

  GST_OBJECT_LOCK (vagg);
  for (l = GST_ELEMENT (vagg)->sinkpads; l; l = l->next) {
    GstVideoAggregatorPad *vaggpad = l->data;

    pad_count++;
    if (!vaggpad->info.finfo ||
        GST_VIDEO_INFO_FORMAT (&vaggpad->info) == GST_VIDEO_FORMAT_UNKNOWN ||
        GST_VIDEO_INFO_WIDTH (&vaggpad->info) <= 0 ||
        GST_VIDEO_INFO_HEIGHT (&vaggpad->info) <= 0) {
      GST_INFO_OBJECT (self,
          "Waiting for sink pad %s video info before updating src caps "
          "(finfo=%p format=%d width=%d height=%d)",
          GST_OBJECT_NAME (vaggpad), vaggpad->info.finfo,
          GST_VIDEO_INFO_FORMAT (&vaggpad->info),
          GST_VIDEO_INFO_WIDTH (&vaggpad->info),
          GST_VIDEO_INFO_HEIGHT (&vaggpad->info));
      GST_OBJECT_UNLOCK (vagg);
      return GST_AGGREGATOR_FLOW_NEED_DATA;
    }
  }
  GST_OBJECT_UNLOCK (vagg);

  if (pad_count != self->nb_inputs) {
    GST_DEBUG_OBJECT (self,
        "Waiting for all sink pads before updating src caps (%u/%d ready)",
        pad_count, self->nb_inputs);
    return GST_AGGREGATOR_FLOW_NEED_DATA;
  }

  return GST_AGGREGATOR_CLASS (parent_class)->update_src_caps (agg, caps, ret);
}

static gboolean
gst_niquadrastack_negotiated_caps (GstAggregator * agg, GstCaps * caps)
{
  GstVideoInfo v_info;

  GST_DEBUG_OBJECT (agg, "Negotiated caps %" GST_PTR_FORMAT, caps);

  if (!gst_video_info_from_caps (&v_info, caps))
    return FALSE;

  return GST_AGGREGATOR_CLASS (parent_class)->negotiated_src_caps (agg, caps);
}

static GstFlowReturn
gst_niquadrastack_create_output_buffer (GstVideoAggregator * vagg,
    GstBuffer ** outbuf)
{
  GstNiQuadraStack *self = GST_NIQUADRASTACK (vagg);
  GList *l;
  gboolean missing_initial_buffer = FALSE;
  int i = 0;

  GST_OBJECT_LOCK (vagg);

  for (l = GST_ELEMENT (vagg)->sinkpads; l; l = l->next, i++) {
    GstVideoAggregatorPad *vaggpad = l->data;
    GstAggregatorPad *pad = GST_AGGREGATOR_PAD (vaggpad);
    stack_item *item = (self->items && i < self->nb_inputs)
        ? &self->items[i] : NULL;

    if (gst_aggregator_pad_is_eos (pad)) {
      if (self->shortest) {
        GST_OBJECT_UNLOCK (vagg);
        return GST_FLOW_EOS;
      }

      if (!gst_video_aggregator_pad_has_current_buffer (vaggpad)
          && !(item && item->got_first_buffer)) {
        GST_WARNING_OBJECT (self,
            "sinkpad %s reached EOS without ever delivering a buffer; "
            "stopping output", GST_PAD_NAME (pad));
        GST_OBJECT_UNLOCK (vagg);
        return GST_FLOW_EOS;
      }
      continue;
    }

    if (!self->initialized
        && !gst_video_aggregator_pad_has_current_buffer (vaggpad)) {
      missing_initial_buffer = TRUE;
    }
  }

  GST_OBJECT_UNLOCK (vagg);

  if (missing_initial_buffer) {
    *outbuf = NULL;
    return GST_FLOW_OK;
  }

  *outbuf = gst_buffer_new ();
  return GST_FLOW_OK;
}

static GstFlowReturn
gst_niquadrastack_aggregate_frames (GstVideoAggregator * vagg,
    GstBuffer * outbuf)
{
  GstNiQuadraStack *self = GST_NIQUADRASTACK (vagg);
  GstAllocator *alloc = NULL;
  GstMemory *out_mem = NULL;
  GstMemory *in_mem = NULL;
  niFrameSurface1_t *in_surface, *out_surface;
  ni_session_data_io_t *p_session_data;
  int cardno = NI_INVALID_HWID, tmp_cardno = NI_INVALID_HWID;
  ni_pix_fmt_t ni_pixfmt;
  int gc620_pixfmt, out_gc620_pixfmt;
  int num_cfg_inputs = MAX_INPUTS;
  uint16_t out_frame_idx;
  GList *l;
  int i, p;
  int retval = 0;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  GstBuffer **in_buffers = self->in_buffers;
  gboolean waiting_for_input_buffers = FALSE;
  gboolean vagg_locked = FALSE;

  GST_OBJECT_LOCK (vagg);
  vagg_locked = TRUE;

  if (self->initialized && self->is_mark_reconfigure) {
    gst_pad_mark_reconfigure (GST_AGGREGATOR_SRC_PAD (self));
    self->is_mark_reconfigure = FALSE;
  }

  i = 0;
  for (l = GST_ELEMENT (vagg)->sinkpads; l; l = l->next) {
    GstBuffer *buff = NULL;
    GstVideoAggregatorPad *pad = l->data;
    GstAggregatorPad *aggpad = GST_AGGREGATOR_PAD (pad);
    stack_item *item = &self->items[i];

    buff = gst_video_aggregator_pad_get_current_buffer (pad);
    if (buff) {
      if (!item->got_first_buffer) {
        item->got_first_buffer = TRUE;
      }
      if (item->last_buff) {
        gst_buffer_unref (item->last_buff);
        item->last_buff = NULL;
      }
      item->last_buff = gst_buffer_ref (buff);
    } else {
      if (!item->got_first_buffer) {
        if (self->shortest && gst_aggregator_pad_is_eos (aggpad)) {
          flow_ret = GST_FLOW_EOS;
          goto EXIT;
        }
        waiting_for_input_buffers = TRUE;
      } else {
        if (self->shortest && gst_aggregator_pad_is_eos (aggpad)) {
          flow_ret = GST_FLOW_EOS;
          goto EXIT;
        }
        buff = item->last_buff;
      }
    }

    in_buffers[i++] = buff;
  }

  if (waiting_for_input_buffers) {
    flow_ret = GST_AGGREGATOR_FLOW_NEED_DATA;
    goto EXIT;
  }

  GST_OBJECT_UNLOCK (vagg);
  vagg_locked = FALSE;

  if (!self->initialized) {
    for (i = 0; i < self->nb_inputs; i++) {
      in_mem = gst_buffer_peek_memory (in_buffers[i], 0);
      if (in_mem == NULL) {
        GST_ERROR_OBJECT (self,
            "Impossible to convert between the formats supported by the filter");
        flow_ret = GST_FLOW_ERROR;
        goto EXIT;
      }

      if (!gst_is_niquadra_memory (in_mem)) {
        GST_ERROR_OBJECT (self,
            "Only Quadra hardware frames are supported on stack sink pads");
        flow_ret = GST_FLOW_ERROR;
        goto EXIT;
      }

      if (i == 0) {
        cardno = gst_deviceid_from_ni_hw_memory (in_mem);
      } else {
        tmp_cardno = gst_deviceid_from_ni_hw_memory (in_mem);
        if (cardno != tmp_cardno) {
          // All inputs must be on the same Quadra device
          flow_ret = GST_FLOW_ERROR;
          goto EXIT;
        }
      }
    }

    retval = ni_device_session_context_init (&self->api_ctx);
    if (retval < 0) {
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }

    self->api_ctx.hw_id = cardno;
    self->api_ctx.device_type = NI_DEVICE_TYPE_SCALER;
    self->api_ctx.scaler_operation = NI_SCALER_OPCODE_STACK;
    self->api_ctx.keep_alive_timeout = self->keep_alive_timeout;
    self->api_ctx.isP2P = self->is_p2p;

    retval = ni_device_session_open (&self->api_ctx, NI_DEVICE_TYPE_SCALER);
    if (retval < 0) {
      flow_ret = GST_FLOW_ERROR;
      ni_device_session_close (&self->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
      ni_device_session_context_clear (&self->api_ctx);
      goto EXIT;
    }

    int pool_size = DEFAULT_NI_FILTER_POOL_SIZE;
    if (self->is_p2p) {
      pool_size = 1;
    } else {
      // If the downstream element is on a different card,
      // Allocate extra frames suggested by the downstream element
      if (self->api_ctx.hw_id != self->downstream_card) {
        pool_size += self->extra_frames;
        GST_INFO_OBJECT (self, "Increase frame pool by %d", self->extra_frames);
      }
    }

    int options = NI_SCALER_FLAG_IO | NI_SCALER_FLAG_PC;
    if (self->is_p2p) {
      options |= NI_SCALER_FLAG_P2;
    }

    ni_pixfmt = convertGstVideoFormatToNIPix (self->out_pixfmt);
    gc620_pixfmt = convertNIPixToGC620Format (ni_pixfmt);

    /* Allocate a pool of frames by the scaler */
    /* *INDENT-OFF* */
    retval = ni_device_alloc_frame (&self->api_ctx,
        NI_ALIGN (self->out_width, 2),
        NI_ALIGN (self->out_height, 2),
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
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }

    if (self->nb_inputs < MAX_INPUTS) {
      self->params.nb_inputs = self->nb_inputs;
    } else {
      self->params.nb_inputs = MAX_INPUTS;
    }

    retval = ni_scaler_set_params (&self->api_ctx, &(self->params));
    if (retval < 0) {
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }

    self->initialized = TRUE;
  }

  retval = ni_frame_buffer_alloc_hwenc (&self->api_dst_frame.data.frame,
      self->out_width, self->out_height, 0);
  if (retval != NI_RETCODE_SUCCESS) {
    flow_ret = GST_FLOW_ERROR;
    goto EXIT;
  }

  /* Allocate hardware device destination frame. This acquires a frame
   * from the pool
   */
  retval = ni_device_session_read_hwdesc (&self->api_ctx,
      &self->api_dst_frame, NI_DEVICE_TYPE_SCALER);
  if (retval != NI_RETCODE_SUCCESS) {
    flow_ret = GST_FLOW_ERROR;
    goto EXIT;
  }

  p_session_data = &self->api_dst_frame;
  out_surface = (niFrameSurface1_t *) p_session_data->data.frame.p_data[3];
  out_surface->dma_buf_fd = 0;
  out_surface->ui32nodeAddress = 0;
  out_surface->ui16width = self->out_width;
  out_surface->ui16height = self->out_height;

  ni_pixfmt = convertGstVideoFormatToNIPix (self->out_pixfmt);

  ni_set_bit_depth_and_encoding_type (&out_surface->bit_depth,
      &out_surface->encoding_type, ni_pixfmt);

  out_frame_idx = out_surface->ui16FrameIdx;

  if (self->fillcolor_enable == 1) {
    self->frame_out.options = NI_SCALER_FLAG_FCE;
  }

  i = 0;
  for (p = self->nb_inputs; p > 0; p -= MAX_INPUTS) {
    int start = i;
    int end = i + MAX_INPUTS;

    if (end > self->nb_inputs) {
      num_cfg_inputs = p;
      end = self->nb_inputs;
    }

    for (; i < end; i++) {
      in_mem = gst_buffer_peek_memory (in_buffers[i], 0);
      in_surface = gst_surface_from_ni_hw_memory (in_mem);
      if (in_surface == NULL) {
        GST_ERROR_OBJECT (self,
            "Impossible to convert between the formats supported by the filter");
        flow_ret = GST_FLOW_ERROR;
        goto EXIT;
      }

      ni_pixfmt = convertGstVideoFormatToNIPix (self->items[i].fmt);
      gc620_pixfmt = convertNIPixToGC620Format (ni_pixfmt);

      self->frame_in[i].picture_width = NI_ALIGN (self->items[i].inw, 2);
      self->frame_in[i].picture_height = NI_ALIGN (self->items[i].inh, 2);
      self->frame_in[i].picture_format = gc620_pixfmt;
      self->frame_in[i].session_id = in_surface->ui16session_ID;
      self->frame_in[i].output_index = in_surface->output_idx;
      self->frame_in[i].frame_index = in_surface->ui16FrameIdx;

      // Where to place the input into the output
      self->frame_in[i].rectangle_x = self->items[i].x;
      self->frame_in[i].rectangle_y = self->items[i].y;
      self->frame_in[i].rectangle_width = self->items[i].w;
      self->frame_in[i].rectangle_height = self->items[i].h;
    }

    ni_pixfmt = convertGstVideoFormatToNIPix (self->out_pixfmt);
    out_gc620_pixfmt = convertNIPixToGC620Format (ni_pixfmt);

    self->frame_out.picture_width = NI_ALIGN (self->out_width, 2);
    self->frame_out.picture_height = NI_ALIGN (self->out_height, 2);
    self->frame_out.picture_format = out_gc620_pixfmt;
    self->frame_out.frame_index = out_frame_idx;
    self->frame_out.options |= NI_SCALER_FLAG_IO;
    if (self->frame_out.options & NI_SCALER_FLAG_FCE) {
      self->frame_out.rgba_color =
          (self->fillcolor[3] << 24) | (self->fillcolor[0] << 16) |
          (self->fillcolor[1] << 8) | self->fillcolor[2];
    } else {
      self->frame_out.rgba_color = 0;
    }

    /*
     * Config device frame parameters
     */
    retval =
        ni_device_multi_config_frame (&self->api_ctx, &self->frame_in[start],
        num_cfg_inputs, &self->frame_out);

    if (retval != NI_RETCODE_SUCCESS) {
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }

    /* Only fill the output frame once each process_frame */
    self->frame_out.options &= ~NI_SCALER_FLAG_FCE;
  }

  alloc = gst_allocator_find (GST_NIQUADRA_MEMORY_TYPE_NAME);
  out_mem = gst_niquadra_allocator_alloc (alloc, &self->api_ctx, out_surface,
      self->api_ctx.hw_id, &vagg->info);
  gst_buffer_append_memory (outbuf, out_mem);
  gst_object_unref (alloc);

EXIT:
  if (flow_ret != GST_FLOW_OK && flow_ret != GST_AGGREGATOR_FLOW_NEED_DATA) {
    GST_ERROR_OBJECT (self, "aggregate_frames exit flow=%s",
        gst_flow_get_name (flow_ret));
  }
  if (vagg_locked) {
    GST_OBJECT_UNLOCK (vagg);
  }
  return flow_ret;
}

static void
gst_niquadrastack_dispose (GObject * obj)
{
  GstNiQuadraStack *self = GST_NIQUADRASTACK (obj);

  if (self->api_dst_frame.data.frame.p_buffer) {
    ni_frame_buffer_free (&self->api_dst_frame.data.frame);
  }

  if (self->api_ctx.session_id != NI_INVALID_SESSION_ID) {
    ni_device_session_close (&self->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
  }
  ni_session_context_t *p_ctx = &self->api_ctx;
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

  ni_device_session_context_clear (&self->api_ctx);

  if (self->items) {
    for (int i = 0; i < self->nb_inputs; i++) {
      gst_niquadrastack_cleanup_item (&self->items[i]);
    }
    free (self->items);
    self->items = NULL;
  }
  if (self->in_buffers) {
    free (self->in_buffers);
    self->in_buffers = NULL;
  }
  if (self->layout) {
    g_free (self->layout);
    self->layout = NULL;
  }
  if (self->size) {
    g_free (self->size);
    self->size = NULL;
  }
  if (self->fillcolor_str) {
    g_free (self->fillcolor_str);
    self->fillcolor_str = NULL;
  }

  G_OBJECT_CLASS (parent_class)->dispose (obj);
}

static gboolean
gst_niquadrastack_stop (GstAggregator * agg)
{
  GstNiQuadraStack *self = GST_NIQUADRASTACK (agg);
  int i;

  GST_DEBUG_OBJECT (self, "niquadrastack Stop");
  if (self->items) {
    for (i = 0; i < self->nb_inputs; i++) {
      gst_niquadrastack_cleanup_item (&self->items[i]);
    }
  }

  return GST_AGGREGATOR_CLASS (parent_class)->stop (agg);
}

static gboolean
gst_niquadrastack_decide_allocation (GstAggregator * agg, GstQuery * query)
{
  gboolean gotit = FALSE;
  guint i;
  GType gtype;
  const GstStructure *params = NULL;
  GstNiQuadraStack *self = GST_NIQUADRASTACK (agg);

  /* Search for allocation metadata */
  for (i = 0; i < gst_query_get_n_allocation_metas (query); i++) {
    gtype = gst_query_parse_nth_allocation_meta (query, i, &params);
    if (gtype == GST_VIDEO_META_API_TYPE) {
      if (params
          && (strcmp (gst_structure_get_name (params),
                  NI_PREALLOCATE_STRUCTURE_NAME) == 0)) {

        gotit =
            gst_structure_get_uint (params, NI_VIDEO_META_BUFCNT,
            &self->extra_frames);
        if (gotit == FALSE)
          GST_ERROR_OBJECT (self, "Did not find buffercnt");

        gotit =
            gst_structure_get_int (params, NI_VIDEO_META_CARDNO,
            &self->downstream_card);
        if (gotit == FALSE)
          GST_ERROR_OBJECT (self, "Did not find cardno");

        break;
      }
    }
  }

  return TRUE;
}

static void
gst_niquadrastack_class_init (GstNiQuadraStackClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;
  GstVideoAggregatorClass *videoaggregator_class =
      (GstVideoAggregatorClass *) klass;
  GstAggregatorClass *agg_class = (GstAggregatorClass *) klass;

  gobject_class->get_property = gst_niquadrastack_get_property;
  gobject_class->set_property = gst_niquadrastack_set_property;

  gstelement_class->request_new_pad = gst_niquadrastack_request_new_pad;
  gstelement_class->release_pad = gst_niquadrastack_release_pad;

  agg_class->update_src_caps = gst_niquadrastack_update_src_caps;
  agg_class->fixate_src_caps = gst_niquadrastack_fixate_caps;
  agg_class->negotiated_src_caps = gst_niquadrastack_negotiated_caps;
  agg_class->stop = gst_niquadrastack_stop;
  agg_class->decide_allocation = gst_niquadrastack_decide_allocation;

  videoaggregator_class->aggregate_frames = gst_niquadrastack_aggregate_frames;
  videoaggregator_class->create_output_buffer =
      gst_niquadrastack_create_output_buffer;

  g_object_class_install_property (gobject_class, PROP_INPUTS,
      g_param_spec_int ("inputs", "Inputs",
          "number of input streams", 0, MAX_XSTACK_INPUTS, 2,
          G_PARAM_CONSTRUCT | GST_PARAM_MUTABLE_PLAYING | G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_LAYOUT,
      g_param_spec_string ("layout", "Layout",
          "set custom stack layout", NULL,
          G_PARAM_CONSTRUCT | GST_PARAM_MUTABLE_PLAYING | G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SIZE,
      g_param_spec_string ("size", "Size",
          "set custom stack size", NULL,
          G_PARAM_CONSTRUCT | GST_PARAM_MUTABLE_PLAYING | G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SHORTEST,
      g_param_spec_boolean ("shortest", "Shortest",
          "force termination when the shortest input terminates",
          FALSE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

  g_object_class_install_property (gobject_class, PROP_FILL,
      g_param_spec_string ("fill", "Fill",
          "set the color for unused pixels", "black",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT));

  g_object_class_install_property (gobject_class, PROP_SYNC,
      g_param_spec_int ("sync", "Sync",
          "input to sync to", 0, MAX_XSTACK_INPUTS - 1,
          0, G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_IS_P2P,
      g_param_spec_boolean ("is-p2p", "Is-p2p",
          "enable p2p transfer", FALSE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

  g_object_class_install_property (gobject_class, PROP_KEEP_ALIVE_TIMEOUT,
      g_param_spec_uint ("keep-alive-timeout", "Keep-alive-timeout",
          "Specify a custom session keep alive timeout in seconds",
          NI_MIN_KEEP_ALIVE_TIMEOUT, NI_MAX_KEEP_ALIVE_TIMEOUT,
          NI_DEFAULT_KEEP_ALIVE_TIMEOUT,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template_with_gtype (gstelement_class,
      &src_factory, GST_TYPE_AGGREGATOR_PAD);
  gst_element_class_add_static_pad_template_with_gtype (gstelement_class,
      &sink_factory, GST_TYPE_NIQUADRASTACK_PAD);

  gst_element_class_set_static_metadata (gstelement_class,
      "NETINT Quadra STACK filter", "Filter/Effect/Video/StackNIQuadra",
      "Hardware Netint Stack", "Leo Liu<leo.liu@netint.cn>");

  gobject_class->dispose = gst_niquadrastack_dispose;
}

static void
gst_niquadrastack_init (GstNiQuadraStack * self)
{
  self->items = NULL;
  self->in_buffers = NULL;
  self->initialized = FALSE;
  self->nb_inputs_tmp = -1;
  self->layout_tmp = NULL;
  self->size_tmp = NULL;
  self->is_mark_reconfigure = FALSE;
  self->downstream_card = NI_INVALID_HWID;
  self->extra_frames = 0;
  self->api_ctx.session_id = NI_INVALID_SESSION_ID;
  self->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
  self->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;

  /* Start aggregation from the first incoming frame running-time instead of
   * time 0, otherwise live sources with non-zero initial PTS can get stuck
   * before VideoAggregator selects any current buffer. */
  g_object_set (self, "start-time-selection",
      GST_AGGREGATOR_START_TIME_SELECTION_FIRST,
#if ENABLE_NI_GST_FRAMEWORK_PATCH
      "max-sink-pad-buffers", 1,
#endif
      NULL);
}

/* GstChildProxy implementation */
static GObject *
gst_niquadrastack_child_proxy_get_child_by_index (GstChildProxy * child_proxy,
    guint index)
{
  GstNiQuadraStack *stack = GST_NIQUADRASTACK (child_proxy);
  GObject *obj = NULL;

  GST_OBJECT_LOCK (stack);
  obj = g_list_nth_data (GST_ELEMENT_CAST (stack)->sinkpads, index);
  if (obj)
    gst_object_ref (obj);
  GST_OBJECT_UNLOCK (stack);

  return obj;
}

static guint
gst_niquadrastack_child_proxy_get_children_count (GstChildProxy * child_proxy)
{
  guint count = 0;
  GstNiQuadraStack *stack = GST_NIQUADRASTACK (child_proxy);

  GST_OBJECT_LOCK (stack);
  count = GST_ELEMENT_CAST (stack)->numsinkpads;
  GST_OBJECT_UNLOCK (stack);

  return count;
}

static void
gst_niquadrastack_child_proxy_init (gpointer g_iface, gpointer iface_data)
{
  GstChildProxyInterface *iface = g_iface;

  iface->get_child_by_index = gst_niquadrastack_child_proxy_get_child_by_index;
  iface->get_children_count = gst_niquadrastack_child_proxy_get_children_count;
}

static gboolean
niquadrastack_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadrastack_debug, "niquadrastack", 0,
      "niquadrastack");

  return gst_element_register (plugin, "niquadrastack", GST_RANK_PRIMARY + 1,
      GST_TYPE_NIQUADRASTACK);
}
