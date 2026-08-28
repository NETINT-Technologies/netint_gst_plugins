/*******************************************************************************
 *
 * Copyright (C) 2025 NETINT Technologies
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
 *  \file   gstniquadrasplit.c
 *
 *  \brief  Implement of NetInt Quadra split filter.
 ******************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include <gst/gst.h>
#include <gst/video/video.h>

#include "niquadra.h"
#include "gstniquadramemory.h"
#include "gstniquadrautils.h"

GST_DEBUG_CATEGORY_STATIC (gst_niquadrasplit_debug);
#define GST_CAT_DEFAULT gst_niquadrasplit_debug

GType gst_niquadrasplit_pad_get_type (void);

#define GST_TYPE_NIQUADRASPLIT_PAD            (gst_niquadrasplit_pad_get_type())
#define GST_NIQUADRASPLIT_PAD(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRASPLIT_PAD,GstNiQuadraSplitPad))
#define GST_NIQUADRASPLIT_PAD_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRASPLIT_PAD,GstNiQuadraSplitPadClass))
#define GST_IS_NIQUADRASPLIT_PAD(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRASPLIT_PAD))
#define GST_IS_NIQUADRASPLIT_PAD_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRASPLIT_PAD))
#define GST_NIQUADRASPLIT_PAD_CAST(obj)       ((GstNiQuadraSplitPad *)(obj))

typedef struct _GstNiQuadraSplitPad GstNiQuadraSplitPad;
typedef struct _GstNiQuadraSplitPadClass GstNiQuadraSplitPadClass;

struct _GstNiQuadraSplitPad
{
  GstPad parent;

  guint index;
  guint pad_id;
};

struct _GstNiQuadraSplitPadClass
{
  GstPadClass parent;
};

G_DEFINE_TYPE (GstNiQuadraSplitPad, gst_niquadrasplit_pad, GST_TYPE_PAD);

static void
gst_niquadrasplit_pad_class_init (GstNiQuadraSplitPadClass * klass)
{
}

static void
gst_niquadrasplit_pad_init (GstNiQuadraSplitPad * pad)
{
}


enum
{
  PROP_0,
  PROP_OUTPUT,
  PROP_LAST
};

typedef struct _GstNiQuadraSplit GstNiQuadraSplit;
typedef struct _GstNiQuadraSplitClass GstNiQuadraSplitClass;

struct _GstNiQuadraSplit
{
  GstElement element;

  GstPad *sinkpad;
  GHashTable *pad_indexes;
  guint next_pad_index;
  guint srcpad_num;

  gboolean enable_out0;
  gint output0_split_num;
  gboolean enable_out1;
  gint output1_split_num;
  gboolean enable_out2;
  gint output2_split_num;
  gchar *output_str;
};

struct _GstNiQuadraSplitClass
{
  GstElementClass parent_class;
};

#define GST_TYPE_NIQUADRASPLIT            (gst_niquadrasplit_get_type())
#define GST_NIQUADRASPLIT(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRASPLIT,GstNiQuadraSplit))
#define GST_NIQUADRASPLIT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NIQUADRASPLIT,GstNiQuadraSplitClass))
#define GST_IS_NIQUADRASPLIT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRASPLIT))
#define GST_IS_NIQUADRASPLIT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NIQUADRASPLIT))
#define GST_NIQUADRASPLIT_CAST(obj)       ((GstNiQuadraSplit *)(obj))

static gboolean niquadrasplit_element_init (GstPlugin * plugin);

GType gst_niquadrasplit_get_type (void);

G_DEFINE_TYPE (GstNiQuadraSplit, gst_niquadrasplit, GST_TYPE_ELEMENT);

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadrasplit, niquadrasplit_element_init);

#define SUPPORTED_FORMATS \
    "{ I420, YUY2, UYVY, NV12, ARGB, RGBA, ABGR, BGRA, I420_10LE, P010_10LE, " \
    "NV16, BGRx, NV12_10LE32 }"

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY, SUPPORTED_FORMATS))
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src_%u",
    GST_PAD_SRC,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY, SUPPORTED_FORMATS))
    );

#define MAX_SPLIT_NUM 	128

static gboolean
parse_output_configure (GstNiQuadraSplit * thiz, const gchar * output)
{
  if (!thiz) {
    return FALSE;
  }

  if ((output == NULL) || (output[0] == '\0')) {
    thiz->enable_out0 = TRUE;
    thiz->output0_split_num = 1;
    return TRUE;
  }

  char *output_str = strdup (output);
  if (!output_str) {
    return FALSE;
  }

  char *token = strtok (output_str, ":");
  int index = 0;

  while (token != NULL && index < 3) {
    int value = atoi (token);
    if (value < 0 || value > MAX_SPLIT_NUM) {
      free (output_str);
      return FALSE;
    }

    if (index == 0) {
      thiz->enable_out0 = TRUE;
      thiz->output0_split_num = value;
      if (value < 1) {
        thiz->output0_split_num = 1;
      }
    } else if (index == 1) {
      thiz->enable_out1 = TRUE;
      thiz->output1_split_num = value;
    } else if (index == 2) {
      thiz->enable_out2 = TRUE;
      thiz->output2_split_num = value;
    }

    index++;
    token = strtok (NULL, ":");
  }

  free (output_str);
  return TRUE;
}

static void
gst_niquadrasplit_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiQuadraSplit *thiz;

  g_return_if_fail (GST_IS_NIQUADRASPLIT (object));
  thiz = GST_NIQUADRASPLIT (object);

  switch (prop_id) {
    case PROP_OUTPUT:
      g_free (thiz->output_str);
      thiz->output_str = g_value_dup_string (value);
      parse_output_configure (thiz, thiz->output_str);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (thiz, prop_id, pspec);
      break;
  }
}

static void
gst_niquadrasplit_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiQuadraSplit *thiz;

  g_return_if_fail (GST_IS_NIQUADRASPLIT (object));
  thiz = GST_NIQUADRASPLIT (object);

  switch (prop_id) {
    case PROP_OUTPUT:
      g_value_take_string (value, g_strdup (thiz->output_str));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (thiz, prop_id, pspec);
      break;
  }
}

static gboolean
gst_niquadrasplit_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  gboolean res = TRUE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_SCHEDULING:
      gst_query_add_scheduling_mode (query, GST_PAD_MODE_PUSH);
      res = TRUE;
      break;
    default:
      res = gst_pad_query_default (pad, parent, query);
      break;
  }

  return res;
}

static gboolean
gst_niquadrasplit_src_activate_mode (GstPad * pad, GstObject * parent,
    GstPadMode mode, gboolean active)
{
  gboolean res = TRUE;

  switch (mode) {
    case GST_PAD_MODE_PULL:
      res = FALSE;
      break;
    default:
      res = TRUE;
      break;
  }

  return res;
}

static gboolean
forward_sticky_events (GstPad * pad, GstEvent ** event, gpointer user_data)
{
  GstPad *srcpad = GST_PAD_CAST (user_data);
  GstFlowReturn flow_ret = GST_FLOW_OK;

  flow_ret = gst_pad_store_sticky_event (srcpad, *event);
  if (flow_ret != GST_FLOW_OK) {
    GST_DEBUG_OBJECT (srcpad, "storing sticky event %p (%s) failed: %s", *event,
        GST_EVENT_TYPE_NAME (*event), gst_flow_get_name (flow_ret));
  }

  return TRUE;
}

static GstPad *
gst_niquadrasplit_request_new_pad (GstElement * element, GstPadTemplate * templ,
    const gchar * name_templ, const GstCaps * caps)
{
  guint index = 0;
  GstNiQuadraSplit *thiz;
  gchar *pad_name;
  GstPad *srcpad;

  thiz = GST_NIQUADRASPLIT (element);

  GST_OBJECT_LOCK (thiz);

  gint src_num_config = thiz->output0_split_num;
  if (thiz->enable_out1) {
    src_num_config += thiz->output1_split_num;
  }
  if (thiz->enable_out2) {
    src_num_config += thiz->output2_split_num;
  }

  if (thiz->srcpad_num + 1 > src_num_config) {
    GST_ERROR_OBJECT (element,
        "src pad num (%d) will exceed the configured num (%d)",
        thiz->srcpad_num + 1, src_num_config);
    GST_OBJECT_UNLOCK (thiz);
    return NULL;
  }

  if (name_templ && sscanf (name_templ, "src_%u", &index) == 1) {
    GST_LOG_OBJECT (element, "name: %s (index %d)", name_templ, index);
    if (g_hash_table_contains (thiz->pad_indexes, GUINT_TO_POINTER (index))) {
      GST_ERROR_OBJECT (element, "pad name %s is not unique", name_templ);
      GST_OBJECT_UNLOCK (thiz);
      return NULL;
    }

    if (index >= thiz->next_pad_index) {
      thiz->next_pad_index = index + 1;
    }
  } else {
    index = thiz->next_pad_index;

    while (g_hash_table_contains (thiz->pad_indexes, GUINT_TO_POINTER (index))) {
      index++;
    }

    thiz->next_pad_index = index + 1;
  }

  g_hash_table_insert (thiz->pad_indexes, GUINT_TO_POINTER (index), NULL);

  pad_name = g_strdup_printf ("src_%u", index);
  srcpad = GST_PAD_CAST (g_object_new (GST_TYPE_NIQUADRASPLIT_PAD,
          "name", pad_name, "direction", templ->direction, "template", templ,
          NULL));
  GST_NIQUADRASPLIT_PAD_CAST (srcpad)->index = index;
  GST_NIQUADRASPLIT_PAD_CAST (srcpad)->pad_id = thiz->srcpad_num;
  thiz->srcpad_num += 1;
  g_free (pad_name);

  GST_OBJECT_UNLOCK (thiz);

  if (!gst_pad_activate_mode (srcpad, GST_PAD_MODE_PUSH, TRUE)) {
    gst_object_unref (srcpad);
    return NULL;
  }

  gst_pad_set_activatemode_function (srcpad,
      GST_DEBUG_FUNCPTR (gst_niquadrasplit_src_activate_mode));
  gst_pad_set_query_function (srcpad,
      GST_DEBUG_FUNCPTR (gst_niquadrasplit_src_query));
  GST_OBJECT_FLAG_SET (srcpad, GST_PAD_FLAG_PROXY_CAPS);
  gst_pad_sticky_events_foreach (thiz->sinkpad, forward_sticky_events, srcpad);
  gst_element_add_pad (element, srcpad);

  return srcpad;
}

static void
gst_niquadrasplit_release_pad (GstElement * element, GstPad * pad)
{
  GstNiQuadraSplit *thiz;
  guint index;

  thiz = GST_NIQUADRASPLIT (element);

  gst_pad_set_active (pad, FALSE);
  gst_element_remove_pad (element, pad);

  index = GST_NIQUADRASPLIT_PAD_CAST (pad)->index;

  GST_OBJECT_LOCK (thiz);
  g_hash_table_remove (thiz->pad_indexes, GUINT_TO_POINTER (index));
  GST_OBJECT_UNLOCK (thiz);
}

static gboolean
gst_niquadrasplit_sink_setcaps (GstPad * pad, GstObject * parent,
    GstCaps * caps)
{
  GstNiQuadraSplit *thiz = GST_NIQUADRASPLIT (parent);
  GstStructure *structure;
  GstCapsFeatures *features;
  GstVideoInfo info;
  gboolean enable_out1 = FALSE;
  gboolean enable_out2 = FALSE;
  gint out0_width, out0_height;
  gint out1_width, out1_height;
  gint out2_width, out2_height;
  gboolean ret = TRUE;

  gint src_num_config = thiz->output0_split_num;
  if (thiz->enable_out1) {
    src_num_config += thiz->output1_split_num;
  }
  if (thiz->enable_out2) {
    src_num_config += thiz->output2_split_num;
  }

  if (thiz->srcpad_num != src_num_config) {
    GST_ERROR_OBJECT (thiz,
        "src pad num (%d) is mismatch with the configured num (%d)",
        thiz->srcpad_num, src_num_config);
    return FALSE;
  }

  features = gst_caps_get_features (caps, 0);
  if (!gst_caps_features_contains (features,
          GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY)) {
    GST_ERROR_OBJECT (thiz, "It's not supported for non HW frame");
    return FALSE;
  }

  structure = gst_caps_get_structure (caps, 0);
  if (gst_structure_has_field (structure, "enable_out1")) {
    enable_out1 = TRUE;
    gst_structure_get_int (structure, "out1_width", &out1_width);
    gst_structure_get_int (structure, "out1_height", &out1_height);
  }

  if (gst_structure_has_field (structure, "enable_out2")) {
    enable_out2 = TRUE;
    gst_structure_get_int (structure, "out2_width", &out2_width);
    gst_structure_get_int (structure, "out2_height", &out2_height);
  }

  if (!gst_video_info_from_caps (&info, caps)) {
    return FALSE;
  }

  out0_width = info.width;
  out0_height = info.height;

  GList *pads = GST_ELEMENT_CAST (thiz)->srcpads;
  for (; pads; pads = g_list_next (pads)) {
    GstNiQuadraSplitPad *srcpad = (GstNiQuadraSplitPad *) pads->data;

    if (srcpad->pad_id < thiz->output0_split_num) {
      info.width = out0_width;
      info.height = out0_height;
    } else if (enable_out1 && srcpad->pad_id < thiz->output0_split_num +
        thiz->output1_split_num) {
      info.width = out1_width;
      info.height = out1_height;
    } else if (enable_out2 && srcpad->pad_id < thiz->output0_split_num +
        thiz->output1_split_num + thiz->output2_split_num) {
      info.width = out2_width;
      info.height = out2_height;
    }

    GstCaps *src_caps = gst_video_info_to_caps (&info);
    gst_caps_set_features_simple (src_caps,
        gst_caps_features_from_string
        (GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY));
    ret = gst_pad_set_caps (GST_PAD (srcpad), src_caps);
    gst_caps_unref (src_caps);
    if (!ret) {
      break;
    }
  }

  return ret;
}

static gboolean
gst_niquadrasplit_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  gboolean ret = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret = gst_niquadrasplit_sink_setcaps (pad, parent, caps);
      gst_event_unref (event);
      break;
    }
    default:
      ret = gst_pad_event_default (pad, parent, event);
      break;
  }

  return ret;
}

static gboolean
gst_niquadrasplit_sink_query (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  gboolean res;

  switch (GST_QUERY_TYPE (query)) {
    default:
      res = gst_pad_query_default (pad, parent, query);
      break;
  }

  return res;
}

static gboolean
gst_niquadrasplit_sink_activate_mode (GstPad * pad, GstObject * parent,
    GstPadMode mode, gboolean active)
{
  gboolean res;

  switch (mode) {
    case GST_PAD_MODE_PUSH:
      res = TRUE;
      break;
    default:
      res = FALSE;
      break;
  }

  return res;
}

static GstFlowReturn
gst_niquadrasplit_chain (GstPad * pad, GstObject * parent, GstBuffer * inbuf)
{
  GstNiQuadraSplit *thiz = GST_NIQUADRASPLIT (parent);
  GstMemory *in_mem0 = NULL;
  GstMemory *in_mem1 = NULL;
  GstMemory *in_mem2 = NULL;
  GstMemory *out_mem = NULL;
  GList *pads;
  GstFlowReturn flow_ret = GST_FLOW_OK;

  in_mem0 = gst_buffer_peek_memory (inbuf, 0);
  in_mem1 = gst_buffer_peek_memory (inbuf, 1);
  in_mem2 = gst_buffer_peek_memory (inbuf, 2);

  for (pads = GST_ELEMENT_CAST (thiz)->srcpads; pads; pads = g_list_next (pads)) {
    GstNiQuadraSplitPad *srcpad = (GstNiQuadraSplitPad *) pads->data;
    GstBuffer *outbuf = gst_buffer_new ();
    if (!outbuf) {
      flow_ret = GST_FLOW_ERROR;
      goto EXIT;
    }

    if (srcpad->pad_id < thiz->output0_split_num) {
      out_mem = gst_memory_ref (in_mem0);
      gst_buffer_append_memory (outbuf, out_mem);
    } else if (thiz->enable_out1 && srcpad->pad_id < thiz->output0_split_num +
        thiz->output1_split_num) {
      out_mem = gst_memory_ref (in_mem1);
      gst_buffer_append_memory (outbuf, out_mem);
    } else if (thiz->enable_out2 && srcpad->pad_id < thiz->output0_split_num +
        thiz->output1_split_num + thiz->output2_split_num) {
      out_mem = gst_memory_ref (in_mem2);
      gst_buffer_append_memory (outbuf, out_mem);
    }

    gst_buffer_copy_into (outbuf, inbuf, GST_BUFFER_COPY_TIMESTAMPS, 0, -1);
    flow_ret = gst_pad_push ((GstPad *) srcpad, outbuf);
    if (flow_ret != GST_FLOW_OK) {
      gst_buffer_unref (outbuf);
      break;
    }
  }

EXIT:
  gst_buffer_unref (inbuf);
  return flow_ret;
}

static void
gst_niquadrasplit_init (GstNiQuadraSplit * thiz)
{
  thiz->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_event_function (thiz->sinkpad, gst_niquadrasplit_sink_event);
  gst_pad_set_query_function (thiz->sinkpad, gst_niquadrasplit_sink_query);
  gst_pad_set_activatemode_function (thiz->sinkpad,
      gst_niquadrasplit_sink_activate_mode);
  gst_pad_set_chain_function (thiz->sinkpad, gst_niquadrasplit_chain);
  GST_OBJECT_FLAG_SET (thiz->sinkpad, GST_PAD_FLAG_PROXY_CAPS);
  gst_element_add_pad (GST_ELEMENT (thiz), thiz->sinkpad);

  thiz->pad_indexes = g_hash_table_new (NULL, NULL);
  thiz->next_pad_index = 0;
  thiz->srcpad_num = 0;

  thiz->enable_out0 = TRUE;
  thiz->output0_split_num = 1;
  thiz->enable_out1 = FALSE;
  thiz->output1_split_num = 0;
  thiz->enable_out2 = FALSE;
  thiz->output2_split_num = 0;
}

static void
gst_niquadrasplit_dispose (GObject * object)
{
  GList *item;

restart:
  for (item = GST_ELEMENT_PADS (object); item; item = g_list_next (item)) {
    GstPad *pad = GST_PAD (item->data);
    if (GST_PAD_IS_SRC (pad)) {
      gst_element_release_request_pad (GST_ELEMENT (object), pad);
      goto restart;
    }
  }

  G_OBJECT_CLASS (gst_niquadrasplit_parent_class)->dispose (object);
}

static void
gst_niquadrasplit_finalize (GObject * object)
{
  GstNiQuadraSplit *thiz = GST_NIQUADRASPLIT (object);

  g_hash_table_unref (thiz->pad_indexes);
  thiz->pad_indexes = NULL;

  if (thiz->output_str) {
    g_free (thiz->output_str);
    thiz->output_str = NULL;
  }

  G_OBJECT_CLASS (gst_niquadrasplit_parent_class)->finalize (object);
}

static void
gst_niquadrasplit_class_init (GstNiQuadraSplitClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gobject_class->set_property = gst_niquadrasplit_set_property;
  gobject_class->get_property = gst_niquadrasplit_get_property;

  g_object_class_install_property (gobject_class, PROP_OUTPUT,
      g_param_spec_string ("output", "Output",
          "the expression representing copies of output0/output1/output2", "",
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (element_class, &sink_factory);
  gst_element_class_add_static_pad_template (element_class, &src_factory);

  gst_element_class_set_static_metadata (element_class,
      "NETINT Quadra split filter", "Filter/Effect/Video/NISplit",
      "Netint Quadra SPLIT", "Leo Liu<leo.liu@netint.cn>");

  element_class->request_new_pad =
      GST_DEBUG_FUNCPTR (gst_niquadrasplit_request_new_pad);

  element_class->release_pad =
      GST_DEBUG_FUNCPTR (gst_niquadrasplit_release_pad);

  gobject_class->finalize = gst_niquadrasplit_finalize;

  gobject_class->dispose = gst_niquadrasplit_dispose;
}

static gboolean
niquadrasplit_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadrasplit_debug, "niquadrasplit",
      0, "niquadrasplit");

  return gst_element_register (plugin, "niquadrasplit", GST_RANK_NONE,
      GST_TYPE_NIQUADRASPLIT);
}
