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
 *  \file   gstniquadramemory.c
 *
 *  \brief  Implement of NetInt Quadra hardware frame.
 ******************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstniquadramemory.h"
#include "gstniquadrautils.h"

GST_DEBUG_CATEGORY_STATIC (gst_niquadramemory_debug);
#define GST_CAT_DEFAULT gst_niquadramemory_debug

typedef struct _GstNiQuadraHWFrame GstNiQuadraHWFrame;
typedef struct _GstNiQuadraMemory GstNiQuadraMemory;

//representing one HWFrame
struct _GstNiQuadraHWFrame
{
  ni_session_context_t *p_session;
  niFrameSurface1_t *ni_surface;
  gint dev_idx;

  //indicate whether it is mapped or not
  gboolean is_mapped;
  uint8_t *data;
  GMutex lock;
};

struct _GstNiQuadraMemory
{
  GstMemory mem;

  GstVideoInfo info;
  GstNiQuadraHWFrame *hw_frame;
};

G_DEFINE_TYPE (GstNiQuadraAllocator, gst_niquadra_allocator,
    GST_TYPE_ALLOCATOR);

static GstAllocator *_gst_niquadra_allocator = NULL;

static gboolean
_download_niquadra_frame (uint8_t * dst, GstNiQuadraHWFrame * ni_frame,
    GstVideoInfo * info)
{
  niFrameSurface1_t *surface = ni_frame->ni_surface;
  ni_session_context_t *p_session = ni_frame->p_session;
  ni_pix_fmt_t pix_fmt;
  ni_session_data_io_t *p_session_data = NULL;
  int retval = 0;

  pix_fmt = convertGstVideoFormatToNIPix (info->finfo->format);
  p_session_data =
      (ni_session_data_io_t *) g_malloc0 (sizeof (ni_session_data_io_t));
  if (!p_session_data) {
    GST_ERROR ("[%s]: Failed to allocate memory for p_session_data",
        __FUNCTION__);
    return FALSE;
  }

  retval = ni_frame_buffer_alloc_dl (&(p_session_data->data.frame),
      info->width, info->height, pix_fmt);
  if (retval != 0) {
    g_free (p_session_data);
    return FALSE;
  }

  p_session->is_auto_dl = false;
  retval = ni_device_session_hwdl (p_session, p_session_data, surface);
  if (retval <= 0) {
    g_free (p_session_data);
    ni_frame_buffer_free (&p_session_data->data.frame);
    return FALSE;
  }

  copy_ni_to_gst_memory (&p_session_data->data.frame, dst, info);
  ni_frame_buffer_free (&p_session_data->data.frame);
  g_free (p_session_data);

  return TRUE;
}

static gpointer
gst_niquadra_mem_map (GstMemory * mem, gsize maxsize, GstMapFlags flags)
{
  GstNiQuadraMemory *nimem = GST_NIQUADRA_MEMORY_CAST (mem);
  gpointer ret = NULL;

  g_mutex_lock (&nimem->hw_frame->lock);

  if (nimem->hw_frame->is_mapped) {
    ret = nimem->hw_frame->data;
  } else {
    uint8_t *data = (uint8_t *) g_malloc0 (nimem->info.size);
    if (!data) {
      GST_ERROR ("[%s]: Failed to allocate memory", __FUNCTION__);
      return NULL;
    }
    nimem->hw_frame->data = data;

    _download_niquadra_frame (nimem->hw_frame->data, nimem->hw_frame,
        &nimem->info);
    nimem->hw_frame->is_mapped = TRUE;
    ret = nimem->hw_frame->data;
  }

  g_mutex_unlock (&nimem->hw_frame->lock);

  return ret;
}

static void
gst_niquadra_mem_unmap (GstMemory * mem)
{

}

static GstMemory *
gst_niquadra_mem_share (GstMemory * mem, gssize offset, gssize size)
{
  GstNiQuadraMemory *nimem = GST_NIQUADRA_MEMORY_CAST (mem);
  GstNiQuadraMemory *sub;
  GstMemory *parent;

  if ((parent = nimem->mem.parent) == NULL) {
    parent = (GstMemory *) nimem;
  }

  sub = g_slice_new0 (GstNiQuadraMemory);
  if (!sub) {
    return NULL;
  }

  gst_memory_init (GST_MEMORY_CAST (sub), 0, nimem->mem.allocator, parent,
      nimem->mem.maxsize, nimem->mem.align, nimem->mem.offset + offset, size);

  sub->hw_frame = nimem->hw_frame;

  return GST_MEMORY_CAST (sub);
}

static GstMemory *
gst_niquadra_mem_copy (GstMemory * mem, gssize offset, gssize size)
{
  return FALSE;
}

static gboolean
gst_niquadra_mem_is_span (GstMemory * mem1, GstMemory * mem2, gsize * offset)
{
  return FALSE;
}

static GstMemory *
_niquadra_alloc (GstAllocator * allocator, gsize size,
    GstAllocationParams * params)
{
  GST_WARNING
      ("use gst_niquadra_allocator_alloc () to allocate from this "
      "GstNiQuadraAllocator allocator");

  return NULL;
}

static void
_niquadra_free (GstAllocator * allocator, GstMemory * mem)
{
  GstNiQuadraMemory *nimem = GST_NIQUADRA_MEMORY_CAST (mem);

  if (nimem->hw_frame && mem->parent == NULL) {
    GstNiQuadraHWFrame *frame = nimem->hw_frame;
    niFrameSurface1_t *surface = frame->ni_surface;
    if (surface) {
      ni_hwframe_buffer_recycle2 (surface);
    }

    if (frame->is_mapped && frame->data) {
      g_free (frame->data);
      frame->data = NULL;
      frame->is_mapped = FALSE;
    }

    g_slice_free (niFrameSurface1_t, surface);
    g_mutex_clear (&frame->lock);
    g_slice_free (GstNiQuadraHWFrame, frame);
  }

  g_slice_free (GstNiQuadraMemory, nimem);
}

static void
gst_niquadra_allocator_class_init (GstNiQuadraAllocatorClass * klass)
{
  GstAllocatorClass *allocator_class = GST_ALLOCATOR_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (gst_niquadramemory_debug, "niquadramemory", 0,
      "niquadramemory");

  allocator_class->alloc = _niquadra_alloc;
  allocator_class->free = _niquadra_free;
}

static void
gst_niquadra_allocator_init (GstNiQuadraAllocator * allocator)
{
  GstAllocator *alloc = GST_ALLOCATOR_CAST (allocator);

  alloc->mem_type = GST_NIQUADRA_MEMORY_TYPE_NAME;

  alloc->mem_map = gst_niquadra_mem_map;
  alloc->mem_unmap = gst_niquadra_mem_unmap;
  alloc->mem_copy = gst_niquadra_mem_copy;
  alloc->mem_share = gst_niquadra_mem_share;
  alloc->mem_is_span = gst_niquadra_mem_is_span;

  GST_OBJECT_FLAG_SET (alloc, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

void
gst_niquadra_memory_init_once (void)
{
  static gsize _init = 0;

  if (g_once_init_enter (&_init)) {
    _gst_niquadra_allocator =
        (GstAllocator *) g_object_new (GST_TYPE_NIQUADRA_ALLOCATOR, NULL);
    gst_object_ref_sink (_gst_niquadra_allocator);

    gst_allocator_register (GST_NIQUADRA_MEMORY_TYPE_NAME,
        _gst_niquadra_allocator);
    g_once_init_leave (&_init, 1);
  }
}

gboolean
gst_is_niquadra_memory (GstMemory * mem)
{
  return mem != NULL && mem->allocator != NULL &&
      GST_IS_NIQUADRA_ALLOCATOR (mem->allocator);
}

GstMemory *
gst_niquadra_allocator_alloc (GstAllocator * allocator,
    ni_session_context_t * session, niFrameSurface1_t * surface,
    gint dev_idx, const GstVideoInfo * info)
{
  GstNiQuadraMemory *nimem = g_slice_new0 (GstNiQuadraMemory);
  if (!nimem) {
    GST_ERROR ("[%s]: Failed to allocate memory", __FUNCTION__);
    return NULL;
  }

  nimem->info = *info;

  gst_memory_init (GST_MEMORY_CAST (nimem), 0, allocator,
      NULL, info->size, 0, 0, info->size);

  nimem->hw_frame = g_slice_new0 (GstNiQuadraHWFrame);
  if (!nimem->hw_frame) {
    g_slice_free (GstNiQuadraMemory, nimem);
    return NULL;
  }
  nimem->hw_frame->p_session = session;
  nimem->hw_frame->dev_idx = dev_idx;

  nimem->hw_frame->ni_surface = g_slice_new0 (niFrameSurface1_t);
  if (!nimem->hw_frame->ni_surface) {
    g_slice_free (GstNiQuadraMemory, nimem);
    g_slice_free (GstNiQuadraHWFrame, nimem->hw_frame);
    return NULL;
  }
  memcpy (nimem->hw_frame->ni_surface, surface, sizeof (niFrameSurface1_t));

  g_mutex_init (&nimem->hw_frame->lock);

  return (GstMemory *) nimem;
}

ni_session_context_t *
gst_session_from_ni_hw_memory (GstMemory * mem)
{
  if (!mem) {
    GST_ERROR ("[%s]: Invalid input GstMemory pointer", __FUNCTION__);
    return NULL;
  }

  GstNiQuadraMemory *nimem = GST_NIQUADRA_MEMORY_CAST (mem);
  return nimem->hw_frame->p_session;
}

niFrameSurface1_t *
gst_surface_from_ni_hw_memory (GstMemory * mem)
{
  if (!mem) {
    GST_ERROR ("[%s]: Invalid input GstMemory pointer", __FUNCTION__);
    return NULL;
  }

  GstNiQuadraMemory *nimem = GST_NIQUADRA_MEMORY_CAST (mem);
  return nimem->hw_frame->ni_surface;
}

gint
gst_deviceid_from_ni_hw_memory (GstMemory * mem)
{
  if (!mem) {
    GST_ERROR ("[%s]: Invalid input GstMemory pointer", __FUNCTION__);
    return NI_INVALID_HWID;
  }

  GstNiQuadraMemory *nimem = GST_NIQUADRA_MEMORY_CAST (mem);
  return nimem->hw_frame->dev_idx;
}
