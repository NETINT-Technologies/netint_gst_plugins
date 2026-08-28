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
 *  \file   gstnilogancontext.h
 *
 *  \brief  Header of NetInt Logan context.
 ******************************************************************************/


#ifndef _GST_NILOGAN_CONTEXT_H
#define _GST_NILOGAN_CONTEXT_H
#include <string.h>

#include <gst/gst.h>
#include <ni_rsrc_api_logan.h>
#include <ni_device_api_logan.h>

G_BEGIN_DECLS
#define GST_TYPE_NILOGAN_CONTEXT \
  (gst_nilogan_context_get_type ())
#define GST_NILOGAN_CONTEXT(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_NILOGAN_CONTEXT, \
      GstNiloganContext))
#define GST_NILOGAN_CONTEXT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_NILOGAN_CONTEXT, \
      GstNiloganContextClass))
#define GST_IS_NILOGAN_CONTEXT(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_NILOGAN_CONTEXT))
#define GST_IS_NILOGAN_CONTEXT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_NILOGAN_CONTEXT))
#define GST_NILOGAN_CONTEXT_CAST(obj) ((GstNiloganContext*)(obj))
typedef struct _GstNiloganContext GstNiloganContext;
typedef struct _GstNiloganContextClass GstNiloganContextClass;
typedef struct _GstNiloganContextPrivate GstNiloganContextPrivate;

/*
 * GstNiloganContext:
 */
struct _GstNiloganContext
{
  GstObject parent_instance;

  GstNiloganContextPrivate *priv;
};

/*
 * GstNiloganContextClass:
 */
struct _GstNiloganContextClass
{
  GstObjectClass parent_class;
};

GType gst_nilogan_context_get_type (void);

GstNiloganContext *gst_nilogan_context_new (void);
ni_logan_device_context_t
    * gst_nilogan_context_get_dev_context (GstNiloganContext * context);
ni_logan_decoder_params_t
    * gst_nilogan_context_get_xcoder_dec_param (GstNiloganContext * context);
ni_logan_encoder_params_t
    * gst_nilogan_context_get_xcoder_enc_param (GstNiloganContext * context);
ni_logan_session_context_t
    * gst_nilogan_context_get_session_context (GstNiloganContext * context);
ni_logan_session_data_io_t *gst_nilogan_context_get_data_pkt (GstNiloganContext
    * context);
ni_logan_session_data_io_t
    * gst_nilogan_context_get_data_frame (GstNiloganContext * context);

G_END_DECLS
#endif //_GST_NILOGAN_CONTEXT_H
