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
 *  \file   niquadra.h
 *
 *  \brief  Header of NetInt gstreamer element's common function.
 ******************************************************************************/

#ifndef _NIQUADRA_H
#define _NIQUADRA_H

#include <string.h>

#include <gst/gst.h>
#include <gst/video/video.h>

GST_ELEMENT_REGISTER_DECLARE (niquadrah264dec);
GST_ELEMENT_REGISTER_DECLARE (niquadrah265dec);
GST_ELEMENT_REGISTER_DECLARE (niquadrajpegdec);
GST_ELEMENT_REGISTER_DECLARE (niquadravp9dec);

GST_ELEMENT_REGISTER_DECLARE (niquadrah264enc);
GST_ELEMENT_REGISTER_DECLARE (niquadrah265enc);
GST_ELEMENT_REGISTER_DECLARE (niquadrajpegenc);
GST_ELEMENT_REGISTER_DECLARE (niquadraav1enc);

GST_ELEMENT_REGISTER_DECLARE (niquadrahwdownload);
GST_ELEMENT_REGISTER_DECLARE (niquadrahwupload);
GST_ELEMENT_REGISTER_DECLARE (niquadrascale);
GST_ELEMENT_REGISTER_DECLARE (niquadracrop);
GST_ELEMENT_REGISTER_DECLARE (niquadrapad);
GST_ELEMENT_REGISTER_DECLARE (niquadraoverlay);
GST_ELEMENT_REGISTER_DECLARE (niquadrastack);
GST_ELEMENT_REGISTER_DECLARE (niquadraaipre);
GST_ELEMENT_REGISTER_DECLARE (niquadrabg);
GST_ELEMENT_REGISTER_DECLARE (niquadrabgr);
GST_ELEMENT_REGISTER_DECLARE (niquadradelogo);
GST_ELEMENT_REGISTER_DECLARE (niquadraroi);
GST_ELEMENT_REGISTER_DECLARE (niquadradrawbox);
GST_ELEMENT_REGISTER_DECLARE (niquadrarotate);
GST_ELEMENT_REGISTER_DECLARE (niquadraflip);
GST_ELEMENT_REGISTER_DECLARE (niquadramerge);
GST_ELEMENT_REGISTER_DECLARE (niquadrainterleave);
GST_ELEMENT_REGISTER_DECLARE (niquadrasplit);

#endif //_NIQUADRA_H
