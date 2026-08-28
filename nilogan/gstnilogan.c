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
 *  \file   gstnilogan.c
 *
 *  \brief  Plugin init of NetInt Logan element
 ******************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif
#include <gst/gst.h>
#include "gstniloganh264dec.h"
#include "gstniloganh265dec.h"
#include "gstniloganh264enc.h"
#include "gstniloganh265enc.h"

GST_DEBUG_CATEGORY (gst_nilogandec_debug);
GST_DEBUG_CATEGORY (gst_niloganh264dec_debug);
GST_DEBUG_CATEGORY (gst_niloganh265dec_debug);
GST_DEBUG_CATEGORY (gst_niloganh264enc_debug);
GST_DEBUG_CATEGORY (gst_niloganh265enc_debug);

static gboolean
plugin_init (GstPlugin * plugin)
{
  gboolean ret;

  GST_DEBUG_CATEGORY_INIT (gst_nilogandec_debug, "nilogandec", 0, "nilogandec");
  GST_DEBUG_CATEGORY_INIT (gst_niloganh264dec_debug, "niloganh264dec", 0,
      "niloganh264dec");
  GST_DEBUG_CATEGORY_INIT (gst_niloganh264dec_debug, "niloganh264dec", 0,
      "niloganh264dec");
  GST_DEBUG_CATEGORY_INIT (gst_niloganh264dec_debug, "niloganh264dec", 0,
      "niloganh264dec");
  GST_DEBUG_CATEGORY_INIT (gst_niloganh264dec_debug, "niloganh264dec", 0,
      "niloganh264dec");

  ret = gst_element_register (plugin, "niloganh264dec", GST_RANK_NONE,
      GST_TYPE_NILOGANH264DEC);
  ret = gst_element_register (plugin, "niloganh265dec", GST_RANK_NONE,
      GST_TYPE_NILOGANH265DEC);
  ret = gst_element_register (plugin, "niloganh264enc", GST_RANK_NONE,
      GST_TYPE_NILOGANH264ENC);
  ret = gst_element_register (plugin, "niloganh265enc", GST_RANK_NONE,
      GST_TYPE_NILOGANH265ENC);
  return ret;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    nilogan,
    "Xcoder_Logan SDK based elements",
    plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
