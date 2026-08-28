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
 *  \file   gstniquadracontext.c
 *
 *  \brief  Implement of NetInt Quadra context.
 ******************************************************************************/


#include "gstniquadracontext.h"
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>

#endif

GST_DEBUG_CATEGORY_STATIC (gst_debug_niquadracontext);
#define GST_CAT_DEFAULT gst_debug_niquadracontext

struct _GstNiquadraContextPrivate
{
  ni_device_context_t *rsrc_ctx;
  ni_session_context_t api_ctx;
  ni_xcoder_params_t *api_param;
  ni_session_data_io_t api_pkt;
  ni_session_data_io_t api_fme;
  GMutex mutex;
};

#define gst_niquadra_context_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstNiquadraContext, gst_niquadra_context,
    GST_TYPE_OBJECT, G_ADD_PRIVATE (GstNiquadraContext)
    GST_DEBUG_CATEGORY_INIT (gst_debug_niquadracontext, "niquadracontext", 0,
        "NIQUADRA Context"));

static void
gst_niquadra_context_init (GstNiquadraContext * context)
{
  GstNiquadraContextPrivate *priv =
      gst_niquadra_context_get_instance_private (context);

  context->priv = priv;

  g_mutex_init (&priv->mutex);

  context->priv->api_param =
      (ni_xcoder_params_t *) g_malloc0 (sizeof (ni_xcoder_params_t));

}

static void
gst_niquadra_context_finalize (GObject * obj)
{
  GST_DEBUG ("XCoder context finalize\n");
  GstNiquadraContext *context = NULL;
  GstNiquadraContextPrivate *priv;
  ni_session_context_t *p_ctx;
  context = GST_NIQUADRA_CONTEXT_CAST (obj);
  priv = context->priv;
  p_ctx = &priv->api_ctx;

  if (p_ctx) {
    ni_device_session_context_clear (p_ctx);
    if (p_ctx->device_handle != NI_INVALID_DEVICE_HANDLE) {
      ni_device_close (p_ctx->device_handle);
      p_ctx->device_handle = NI_INVALID_DEVICE_HANDLE;
    }
    if (p_ctx->blk_io_handle != NI_INVALID_DEVICE_HANDLE) {
      ni_device_close (p_ctx->blk_io_handle);
      p_ctx->blk_io_handle = NI_INVALID_DEVICE_HANDLE;
    }
    ni_rsrc_free_device_context (priv->rsrc_ctx);
    g_free (priv->api_param);
  }
  g_mutex_clear (&priv->mutex);

  G_OBJECT_CLASS (parent_class)->finalize (obj);
}

static void
gst_niquadra_context_class_init (GstNiquadraContextClass * klass)
{
  GObjectClass *const g_object_class = G_OBJECT_CLASS (klass);

  g_object_class->finalize = gst_niquadra_context_finalize;
}

GstNiquadraContext *
gst_niquadra_context_new (void)
{
  GstNiquadraContext *obj = g_object_new (GST_TYPE_NIQUADRA_CONTEXT, NULL);

  if (!obj) {
    GST_ERROR ("Error new GstNiquadraContext failure\n");
    return NULL;
  }

  if (ni_device_session_context_init (&(obj->priv->api_ctx)) < 0) {
    GST_ERROR ("Error XCoder init decoder context failure\n");
    return NULL;
  }
  memset (&(obj->priv->api_fme), 0, sizeof (ni_session_data_io_t));
  memset (&(obj->priv->api_pkt), 0, sizeof (ni_session_data_io_t));

  return obj;
}

ni_device_context_t *
gst_niquadra_context_get_dev_context (GstNiquadraContext * context)
{
  return context->priv->rsrc_ctx;
}

ni_xcoder_params_t *
gst_niquadra_context_get_xcoder_param (GstNiquadraContext * context)
{
  return context->priv->api_param;
}

ni_session_context_t *
gst_niquadra_context_get_session_context (GstNiquadraContext * context)
{
  return &(context->priv->api_ctx);
}

ni_session_data_io_t *
gst_niquadra_context_get_data_pkt (GstNiquadraContext * context)
{
  return &(context->priv->api_pkt);
}

ni_session_data_io_t *
gst_niquadra_context_get_data_frame (GstNiquadraContext * context)
{
  return &(context->priv->api_fme);
}
