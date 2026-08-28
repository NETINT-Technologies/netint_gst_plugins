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
 *  \file   gstniquadra.c
 *
 *  \brief  Plugin init of NetInt Quadra element
 ******************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif
#include <gst/gst.h>

#include "gstniquadramemory.h"
#include "niquadra.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
  gboolean ret = TRUE;

  gst_niquadra_memory_init_once ();

  //register decoders
  ret &= GST_ELEMENT_REGISTER (niquadrah264dec, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadrah265dec, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadravp9dec, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadrajpegdec, plugin);

  //register encoders
  ret &= GST_ELEMENT_REGISTER (niquadrah264enc, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadrah265enc, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadrajpegenc, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadraav1enc, plugin);

  //register filters
  ret &= GST_ELEMENT_REGISTER (niquadrahwdownload, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadrahwupload, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadrascale, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadracrop, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadrapad, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadraoverlay, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadrastack, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadraaipre, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadrabg, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadrabgr, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadradelogo, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadraroi, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadradrawbox, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadrarotate, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadraflip, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadramerge, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadrainterleave, plugin);
  ret &= GST_ELEMENT_REGISTER (niquadrasplit, plugin);

  return ret;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    niquadra,
    "Xcoder SDK based elements",
    plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
