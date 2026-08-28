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
 *  \file   gstniquadrainterleave.c
 *
 *  \brief  Implement of NetInt Quadra hardware interleave filter.
 ******************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstniquadrainterleave.h"
#include "niquadra.h"
#include "gstniquadramemory.h"
#include "gstniquadrautils.h"

GST_DEBUG_CATEGORY_STATIC (gst_niquadrainterleave_debug);
#define GST_CAT_DEFAULT gst_niquadrainterleave_debug

#if ENABLE_NI_GST_FRAMEWORK_PATCH
#define NI_QUADRA_TILED_FORMATS ", NI_QUAD_8_4L4"
#else
#define NI_QUADRA_TILED_FORMATS ""
#endif

#define SUPPORTED_FORMATS \
    "{ I420, YUY2, UYVY, NV12, ARGB, RGBA, ABGR, BGRA, I420_10LE, P010_10LE, " \
    "NV16, BGRx, NV12_10LE32" NI_QUADRA_TILED_FORMATS " }"

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink_%u",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (SUPPORTED_FORMATS))
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (SUPPORTED_FORMATS))
    );

static void gst_niquadrainterleave_child_proxy_init (gpointer g_iface,
    gpointer iface_data);

enum
{
  PROP_0,
  PROP_NB_INPUTS,
  PROP_LAST
};

static void
gst_niquadrainterleave_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNiQuadraInterleave *self;

  g_return_if_fail (GST_IS_NIQUADRAINTERLEAVE (object));
  self = GST_NIQUADRAINTERLEAVE (object);

  switch (prop_id) {
    case PROP_NB_INPUTS:
      g_value_set_uint (value, self->nb_inputs);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static void
gst_niquadrainterleave_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNiQuadraInterleave *self;

  g_return_if_fail (GST_IS_NIQUADRAINTERLEAVE (object));
  self = GST_NIQUADRAINTERLEAVE (object);

  switch (prop_id) {
    case PROP_NB_INPUTS:
      self->nb_inputs = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static gboolean niquadrainterleave_element_init (GstPlugin * plugin);

G_DEFINE_TYPE_WITH_CODE (GstNiQuadraInterleave, gst_niquadrainterleave,
    GST_TYPE_AGGREGATOR, G_IMPLEMENT_INTERFACE (GST_TYPE_CHILD_PROXY,
        gst_niquadrainterleave_child_proxy_init));

GST_ELEMENT_REGISTER_DEFINE_CUSTOM (niquadrainterleave,
    niquadrainterleave_element_init);

static GstCaps *
gst_niquadrainterleave_fixate_caps (GstAggregator * agg, GstCaps * caps)
{
  GstNiQuadraInterleave *self = GST_NIQUADRAINTERLEAVE (agg);
  GstCaps *ret = NULL;
  GList *l;
  GstStructure *s;
  gint width = 0, height = 0;
  GstVideoFormat pixfmt = GST_VIDEO_FORMAT_UNKNOWN;
  gint fps_n = 1, fps_d = 1;
  gint par_n = 1, par_d = 1;
  int i = 0;

  GST_OBJECT_LOCK (agg);

  for (l = GST_ELEMENT (agg)->sinkpads; l; l = l->next) {
    GstVideoInfo info;
    GstPad *sink_pad = l->data;
    GstCaps *sink_caps = gst_pad_get_current_caps (sink_pad);
    gst_video_info_from_caps (&info, sink_caps);
    if (i == 0) {
      width = GST_VIDEO_INFO_WIDTH (&info);
      height = GST_VIDEO_INFO_HEIGHT (&info);
      pixfmt = GST_VIDEO_INFO_FORMAT (&info);
      fps_n = GST_VIDEO_INFO_FPS_N (&info);
      fps_d = GST_VIDEO_INFO_FPS_D (&info);
    } else {
      if ((width != GST_VIDEO_INFO_WIDTH (&info)) ||
          (height != GST_VIDEO_INFO_HEIGHT (&info)) ||
          (pixfmt != GST_VIDEO_INFO_FORMAT (&info)) ||
          (fps_n != GST_VIDEO_INFO_FPS_N (&info)) ||
          (fps_d != GST_VIDEO_INFO_FPS_D (&info))) {
        GST_ERROR_OBJECT (self,
            "video caps must be identical on all sink pads for niquadrainterleave");
        GST_OBJECT_UNLOCK (agg);
        return NULL;
      }
    }

    i++;
  }

  GST_OBJECT_UNLOCK (agg);

  if (i != self->nb_inputs) {
    GST_ERROR_OBJECT (self,
        "Number of input pads (%d) isn't match nb_inputs (%d)",
        i, self->nb_inputs);
    return NULL;
  }

  ret = gst_caps_make_writable (caps);
  s = gst_caps_get_structure (ret, 0);
  gst_structure_set (s, "format", G_TYPE_STRING,
      gst_video_format_to_string (pixfmt), NULL);
  gst_structure_fixate_field_nearest_int (s, "width", width);
  gst_structure_fixate_field_nearest_int (s, "height", height);

  if (gst_structure_has_field (s, "framerate")) {
    gst_structure_fixate_field_nearest_fraction (s, "framerate", fps_n, fps_d);
  } else {
    gst_structure_set (s, "framerate", GST_TYPE_FRACTION, fps_n, fps_d, NULL);
  }

  if (gst_structure_has_field (s, "pixel-aspect-ratio")) {
    gst_structure_fixate_field_nearest_fraction (s, "pixel-aspect-ratio", 1, 1);
    gst_structure_get_fraction (s, "pixel-aspect-ratio", &par_n, &par_d);
  } else {
    par_n = par_d = 1;
  }

  return gst_caps_fixate (ret);
}

static GstFlowReturn
gst_niquadrainterleave_aggregate (GstAggregator * agg, gboolean timeout)
{
  GList *l = NULL;
  GstAggregatorPad *selected_pad = NULL;
  GstBuffer *selected_buf = NULL;
  gboolean all_eos = TRUE;
  GstClockTime min_pts = GST_CLOCK_TIME_NONE;
  GstFlowReturn ret = GST_FLOW_OK;
  GstNiQuadraInterleave *self = GST_NIQUADRAINTERLEAVE (agg);

  GST_OBJECT_LOCK (agg);

  for (l = GST_ELEMENT (agg)->sinkpads; l; l = l->next) {
    GstAggregatorPad *apad = GST_AGGREGATOR_PAD (l->data);
    if (gst_aggregator_pad_is_eos (apad)) {
      continue;
    } else {
      all_eos = FALSE;
    }

    GstBuffer *inbuf = gst_aggregator_pad_peek_buffer (apad);
    if (!inbuf) {
      continue;
    }

    GstClockTime pts = GST_BUFFER_PTS (inbuf);
    if (pts == GST_CLOCK_TIME_NONE) {
      GST_ERROR_OBJECT (self, "NOPTS value for input frame cannot be accepted");
      gst_buffer_unref (inbuf);
      if (selected_buf) {
        gst_buffer_unref (selected_buf);
      }
      GST_OBJECT_UNLOCK (agg);
      return GST_FLOW_ERROR;
    }

    if (min_pts == GST_CLOCK_TIME_NONE || pts < min_pts) {
      min_pts = pts;
      selected_pad = apad;
      if (selected_buf) {
        gst_buffer_unref (selected_buf);
      }
      selected_buf = gst_buffer_ref (inbuf);
    }

    gst_buffer_unref (inbuf);
  }

  GST_OBJECT_UNLOCK (agg);

  if (all_eos) {
    return GST_FLOW_EOS;
  }

  if (selected_pad && selected_buf) {
    ret = gst_aggregator_finish_buffer (agg, selected_buf);
    gst_aggregator_pad_drop_buffer (selected_pad);
  }

  return ret;
}

static void
gst_niquadrainterleave_class_init (GstNiQuadraInterleaveClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;
  GstAggregatorClass *agg_class = (GstAggregatorClass *) klass;

  gobject_class->get_property = gst_niquadrainterleave_get_property;
  gobject_class->set_property = gst_niquadrainterleave_set_property;

  agg_class->fixate_src_caps = gst_niquadrainterleave_fixate_caps;
  agg_class->aggregate = gst_niquadrainterleave_aggregate;

  g_object_class_install_property (gobject_class, PROP_NB_INPUTS,
      g_param_spec_uint ("nb-inputs", "Number of input",
          "Set number of input pads", 1, 4, 2,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template_with_gtype (gstelement_class,
      &src_factory, GST_TYPE_AGGREGATOR_PAD);
  gst_element_class_add_static_pad_template_with_gtype (gstelement_class,
      &sink_factory, GST_TYPE_AGGREGATOR_PAD);

  gst_element_class_set_static_metadata (gstelement_class,
      "NETINT Quadra INTERLEAVE filter",
      "Filter/Effect/Video/InterleaveNIQuadra", "Netint customized Interleave",
      "Leo Liu<leo.liu@netint.cn>");
}

static void
gst_niquadrainterleave_init (GstNiQuadraInterleave * self)
{
  self->nb_inputs = 2;
}

/* GstChildProxy implementation */
static GObject *
gst_niquadrainterleave_child_proxy_get_child_by_index (GstChildProxy *
    child_proxy, guint index)
{
  GstNiQuadraInterleave *interleave = GST_NIQUADRAINTERLEAVE (child_proxy);
  GObject *obj = NULL;

  GST_OBJECT_LOCK (interleave);
  obj = g_list_nth_data (GST_ELEMENT_CAST (interleave)->sinkpads, index);
  if (obj)
    gst_object_ref (obj);
  GST_OBJECT_UNLOCK (interleave);

  return obj;
}

static guint
gst_niquadrainterleave_child_proxy_get_children_count (GstChildProxy *
    child_proxy)
{
  guint count = 0;
  GstNiQuadraInterleave *interleave = GST_NIQUADRAINTERLEAVE (child_proxy);

  GST_OBJECT_LOCK (interleave);
  count = GST_ELEMENT_CAST (interleave)->numsinkpads;
  GST_OBJECT_UNLOCK (interleave);

  return count;
}

static void
gst_niquadrainterleave_child_proxy_init (gpointer g_iface, gpointer iface_data)
{
  GstChildProxyInterface *iface = g_iface;

  iface->get_child_by_index =
      gst_niquadrainterleave_child_proxy_get_child_by_index;
  iface->get_children_count =
      gst_niquadrainterleave_child_proxy_get_children_count;
}

static gboolean
niquadrainterleave_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_niquadrainterleave_debug, "niquadrainterleave",
      0, "niquadrainterleave");

  return gst_element_register (plugin, "niquadrainterleave",
      GST_RANK_PRIMARY + 1, GST_TYPE_NIQUADRAINTERLEAVE);
}
