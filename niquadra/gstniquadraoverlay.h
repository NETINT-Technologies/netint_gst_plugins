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
 *  \file   gstniquadraoverlay.h
 *
 *  \brief  Header of NetInt Quadra overlay filter.
 ******************************************************************************/

#ifndef __GST_NIQUADRAOVERLAY_H__
#define __GST_NIQUADRAOVERLAY_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideoaggregator.h>

#include "ni_device_api.h"

G_BEGIN_DECLS

typedef enum
{
  EOF_ACTION_REPEAT,
  EOF_ACTION_ENDALL,
  EOF_ACTION_PASS
} GstEOFAction;


#define GST_TYPE_NIQUADRAOVERLAY (gst_niquadraoverlay_get_type())

G_DECLARE_FINAL_TYPE (GstNiQuadraOverlay, gst_niquadraoverlay, GST,
                      NIQUADRAOVERLAY, GstVideoAggregator)

/**
 * GstNiQuadraOverlay:
 *
 * The opaque #GstNiQuadraOverlay structure.
 */
struct _GstNiQuadraOverlay
{
  GstVideoAggregator vgg;

  gint out_width;
  gint out_height;
  GstVideoFormat out_pixfmt;
  GstVideoInterlaceMode interlace_mode;

  gint main_width, main_height;
  gint overlay_width, overlay_height;
  GstVideoFormat main_pixfmt;
  GstVideoFormat overlay_pixfmt;

  gint x;
  gint y;
  gchar *eof_action_str;
  gboolean shortest;
  gboolean repeat_last;
  gchar *alpha;
  gboolean inplace;
  gboolean is_p2p;
  guint keep_alive_timeout;

  gboolean initialized;

  GstEOFAction eof_action;
  gint alpha_format;

  GstVideoColorimetry main_colorimetry;
  GstVideoColorimetry overlay_colorimetry;

  gboolean main_has_alpha;
  gboolean overlay_has_alpha;

  GstBuffer *last_main_buff;
  GstBuffer *last_overlay_buff;

  gboolean pass_main_frame;

  ni_session_context_t *api_ctx;
  ni_session_data_io_t api_dst_frame;

  ni_session_context_t *crop_api_ctx;
  ni_session_data_io_t crop_api_dst_frame;

  uint16_t ui16CropFrameIdx;
  guint extra_frames;
  gint downstream_card;
};

#define GST_TYPE_NIQUADRAOVERLAY_PAD (gst_niquadraoverlay_pad_get_type())

G_DECLARE_FINAL_TYPE (GstNiQuadraOverlayPad, gst_niquadraoverlay_pad, GST,
                      NIQUADRAOVERLAY_PAD, GstVideoAggregatorConvertPad)

/**
 * GstNiQuadraOverlayPad:
 *
 * The opaque #GstNiQuadraOverlayPad structure.
 */
struct _GstNiQuadraOverlayPad
{
  GstVideoAggregatorConvertPad parent;
};

G_END_DECLS
#endif /* __GST_NIQUADRAOVERLAY_H__ */
