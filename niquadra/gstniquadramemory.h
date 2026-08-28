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
 *  \file   gstniquadramemory.h
 *
 *  \brief  Header of NetInt Quadra hardware frame.
 ******************************************************************************/

#ifndef __GST_NIQUADRA_MEMORY_H__
#define __GST_NIQUADRA_MEMORY_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include "ni_device_api.h"

G_BEGIN_DECLS

#define GST_TYPE_NIQUADRA_ALLOCATOR             (gst_niquadra_allocator_get_type())
#define GST_NIQUADRA_ALLOCATOR(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NIQUADRA_ALLOCATOR, GstNiQuadraAllocator))
#define GST_NIQUADRA_ALLOCATOR_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_NIQUADRA_ALLOCATOR, GstNiQuadraAllocatorClass))
#define GST_NIQUADRA_ALLOCATOR_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_NIQUADRA_ALLOCATOR, GstNiQuadraAllocatorClass))
#define GST_IS_NIQUADRA_ALLOCATOR(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NIQUADRA_ALLOCATOR))
#define GST_IS_NIQUADRA_ALLOCATOR_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_NIQUADRA_ALLOCATOR))

#define GST_NIQUADRA_ALLOCATOR_CAST(obj)        ((GstNiQuadraAllocator *)(obj))
#define GST_NIQUADRA_MEMORY_CAST(mem)           ((GstNiQuadraMemory *) (mem))

//Name of niquadra memory type
#define GST_NIQUADRA_MEMORY_TYPE_NAME "gst.niquadra.memory"

//Name of the caps feature for indicating the use of #GstNiQuadraMemory
#define GST_CAPS_FEATURE_MEMORY_NIQUADRA_MEMORY "memory:NiQuadraMemory"

typedef struct _GstNiQuadraAllocator GstNiQuadraAllocator;
typedef struct _GstNiQuadraAllocatorClass GstNiQuadraAllocatorClass;

struct _GstNiQuadraAllocator
{
  GstAllocator parent;
};

struct _GstNiQuadraAllocatorClass
{
  GstAllocatorClass parent_class;
};

void gst_niquadra_memory_init_once (void);

gboolean gst_is_niquadra_memory (GstMemory * mem);

GType gst_niquadra_allocator_get_type (void);

GstMemory * gst_niquadra_allocator_alloc     (GstAllocator * allocator,  
                                              ni_session_context_t * session,
                                              niFrameSurface1_t * surface,
                                              gint dev_idx,
                                              const GstVideoInfo *info);

ni_session_context_t *
gst_session_from_ni_hw_memory (GstMemory *mem);

niFrameSurface1_t *
gst_surface_from_ni_hw_memory (GstMemory *mem);

gint
gst_deviceid_from_ni_hw_memory (GstMemory *mem);

G_END_DECLS

#endif /* __GST_NIQUADRA_MEMORY_H__ */
