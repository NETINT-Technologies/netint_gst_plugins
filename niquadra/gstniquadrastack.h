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
 *  \file   gstniquadrastack.h
 *
 *  \brief  Header of NetInt Quadra stack filter.
 ******************************************************************************/

#ifndef __GST_NIQUADRASTACK_H__
#define __GST_NIQUADRASTACK_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideoaggregator.h>

#include "ni_device_api.h"

G_BEGIN_DECLS

#define MAX_INPUTS 8
#define MAX_XSTACK_INPUTS 50

#define GST_TYPE_NIQUADRASTACK (gst_niquadrastack_get_type())

G_DECLARE_FINAL_TYPE (GstNiQuadraStack, gst_niquadrastack, GST, NIQUADRASTACK,
                      GstVideoAggregator)

typedef struct _stack_item
{
  int x, y;
  int w, h;
  gint inw, inh;
  GstVideoFormat fmt;
  GstBuffer *last_buff;
  gboolean got_first_buffer;
  GstPad *pad;
} stack_item;

/**
 * GstNiQuadraStack:
 *
 * The opaque #GstNiQuadraStack structure.
 */
struct _GstNiQuadraStack
{
  GstVideoAggregator vgg;

  gint nb_inputs;
  gchar *layout;
  gchar *size;
  gint nb_inputs_tmp;
  gchar *layout_tmp;
  gchar *size_tmp;
  gboolean shortest;
  gchar *fillcolor_str;
  gint sync;
  gboolean is_p2p;
  guint keep_alive_timeout;

  gint out_width;
  gint out_height;
  GstVideoFormat out_pixfmt;

  uint8_t fillcolor[4];
  gint fillcolor_enable;

  gboolean is_mark_reconfigure;
  gboolean initialized;
  ni_session_context_t api_ctx;
  ni_session_data_io_t api_dst_frame;
  ni_scaler_params_t params;

  stack_item *items;
  GstBuffer **in_buffers;

  ni_frame_config_t frame_in[MAX_XSTACK_INPUTS];
  ni_frame_config_t frame_out;

  guint extra_frames;
  gint downstream_card;
};


#define GST_TYPE_NIQUADRASTACK_PAD (gst_niquadrastack_pad_get_type())

G_DECLARE_FINAL_TYPE (GstNiQuadraStackPad, gst_niquadrastack_pad, GST,
                      NIQUADRASTACK_PAD, GstVideoAggregatorConvertPad)

/**
 * GstNiQuadraStackPad:
 *
 * The opaque #GstNiQuadraStackPad structure.
 */
struct _GstNiQuadraStackPad
{
  GstVideoAggregatorConvertPad parent;
};

G_END_DECLS
#endif /* __GST_NIQUADRASTACK_H__ */
