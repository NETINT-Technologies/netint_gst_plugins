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
 *  \file   gstniquadracontext.h
 *
 *  \brief  Header of NetInt Quadra context.
 ******************************************************************************/


#ifndef _GST_NIQUADRA_CONTEXT_H
#define _GST_NIQUADRA_CONTEXT_H

#include "niquadra.h"
#include <ni_rsrc_api.h>
#include <ni_device_api.h>

G_BEGIN_DECLS
#define GST_TYPE_NIQUADRA_CONTEXT \
  (gst_niquadra_context_get_type ())
#define GST_NIQUADRA_CONTEXT(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_NIQUADRA_CONTEXT, \
      GstNiquadraContext))
#define GST_NIQUADRA_CONTEXT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_NIQUADRA_CONTEXT, \
      GstNiquadraContextClass))
#define GST_IS_NIQUADRA_CONTEXT(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_NIQUADRA_CONTEXT))
#define GST_IS_NIQUADRA_CONTEXT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_NIQUADRA_CONTEXT))
#define GST_NIQUADRA_CONTEXT_CAST(obj) ((GstNiquadraContext*)(obj))
typedef struct _GstNiquadraContext GstNiquadraContext;
typedef struct _GstNiquadraContextClass GstNiquadraContextClass;
typedef struct _GstNiquadraContextPrivate GstNiquadraContextPrivate;

/*
 * GstNiquadraContext:
 */
struct _GstNiquadraContext
{
  GstObject parent_instance;

  GstNiquadraContextPrivate *priv;
};

/*
 * GstNiquadraContextClass:
 */
struct _GstNiquadraContextClass
{
  GstObjectClass parent_class;
};

GType gst_niquadra_context_get_type (void);

GstNiquadraContext *gst_niquadra_context_new (void);

ni_device_context_t *gst_niquadra_context_get_dev_context (GstNiquadraContext *context);

ni_xcoder_params_t *gst_niquadra_context_get_xcoder_param (GstNiquadraContext *context);

ni_session_context_t *gst_niquadra_context_get_session_context (GstNiquadraContext *context);

ni_session_data_io_t *gst_niquadra_context_get_data_pkt (GstNiquadraContext *context);

ni_session_data_io_t *gst_niquadra_context_get_data_frame (GstNiquadraContext *context);

G_END_DECLS
#endif //_GST_NIQUADRA_CONTEXT_H
