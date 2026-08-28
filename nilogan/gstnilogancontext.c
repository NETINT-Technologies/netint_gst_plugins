/*******************************************************************************
 *
 * Copyright (C) 2024 NETINT Technologies
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
 *  \file   gstnilogancontext.c
 *
 *  \brief  Implement of NetInt Logan context.
 ******************************************************************************/


#include "gstnilogancontext.h"
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>

#endif

GST_DEBUG_CATEGORY_STATIC (gst_debug_nilogancontext);
#define GST_CAT_DEFAULT gst_debug_nilogancontext

struct _GstNiloganContextPrivate
{
  ni_logan_device_context_t *rsrc_ctx;
  ni_logan_session_context_t api_ctx;
  ni_logan_decoder_params_t *api_dec_param;
  ni_logan_encoder_params_t *api_enc_param;
  ni_logan_session_data_io_t api_pkt;
  ni_logan_session_data_io_t api_fme;
  GMutex mutex;
};

#define gst_nilogan_context_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstNiloganContext, gst_nilogan_context,
    GST_TYPE_OBJECT, G_ADD_PRIVATE (GstNiloganContext)
    GST_DEBUG_CATEGORY_INIT (gst_debug_nilogancontext, "nilogancontext", 0,
        "NILOGAN Context"));

static void
gst_nilogan_context_init (GstNiloganContext * context)
{
  GstNiloganContextPrivate *priv =
      gst_nilogan_context_get_instance_private (context);

  context->priv = priv;

  g_mutex_init (&priv->mutex);
}

static void
gst_nilogan_context_finalize (GObject * obj)
{
  GST_DEBUG ("XCoder context finalize\n");
  GstNiloganContext *context = NULL;
  GstNiloganContextPrivate *priv;
  ni_logan_session_context_t *p_ctx;
  context = GST_NILOGAN_CONTEXT_CAST (obj);
  priv = context->priv;
  p_ctx = &priv->api_ctx;

  if (p_ctx) {
    ni_logan_device_session_context_clear (p_ctx);
    ni_logan_rsrc_free_device_context (priv->rsrc_ctx);
    p_ctx->device_handle = NI_INVALID_DEVICE_HANDLE;
    p_ctx->blk_io_handle = NI_INVALID_DEVICE_HANDLE;
    g_free (priv->api_dec_param);
    g_free (priv->api_enc_param);
  }
  g_mutex_clear (&priv->mutex);

  G_OBJECT_CLASS (parent_class)->finalize (obj);
}

static void
gst_nilogan_context_class_init (GstNiloganContextClass * klass)
{
  GObjectClass *const g_object_class = G_OBJECT_CLASS (klass);

  g_object_class->finalize = gst_nilogan_context_finalize;
}

GstNiloganContext *
gst_nilogan_context_new (void)
{
  GstNiloganContext *obj = g_object_new (GST_TYPE_NILOGAN_CONTEXT, NULL);
  ni_logan_device_session_context_init (&(obj->priv->api_ctx));
  obj->priv->api_dec_param = (ni_logan_decoder_params_t *)
      g_malloc0 (sizeof (ni_logan_decoder_params_t));
  obj->priv->api_enc_param = (ni_logan_encoder_params_t *)
      g_malloc0 (sizeof (ni_logan_encoder_params_t));
  memset (&(obj->priv->api_fme), 0, sizeof (ni_logan_session_data_io_t));
  memset (&(obj->priv->api_pkt), 0, sizeof (ni_logan_session_data_io_t));

  return obj;
}

ni_logan_device_context_t *
gst_nilogan_context_get_dev_context (GstNiloganContext * context)
{
  return context->priv->rsrc_ctx;
}

ni_logan_decoder_params_t *
gst_nilogan_context_get_xcoder_dec_param (GstNiloganContext * context)
{
  return context->priv->api_dec_param;
}

ni_logan_encoder_params_t *
gst_nilogan_context_get_xcoder_enc_param (GstNiloganContext * context)
{
  return context->priv->api_enc_param;
}

ni_logan_session_context_t *
gst_nilogan_context_get_session_context (GstNiloganContext * context)
{
  return &(context->priv->api_ctx);
}

ni_logan_session_data_io_t *
gst_nilogan_context_get_data_pkt (GstNiloganContext * context)
{
  return &(context->priv->api_pkt);
}

ni_logan_session_data_io_t *
gst_nilogan_context_get_data_frame (GstNiloganContext * context)
{
  return &(context->priv->api_fme);
}
