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
 *  \file   gstniloganutils.c
 *
 *  \brief  Implement of NetInt Logan common function.
 ******************************************************************************/

#include <string.h>
#include <glib.h>

#include "gstniloganutils.h"
#include "gst/gst.h"

typedef struct ni_logan_err_rc_txt_entry
{
  ni_logan_retcode_t rc;
  const char *txt;
} ni_logan_err_rc_txt_entry_t;

static const ni_logan_err_rc_txt_entry_t ni_logan_err_rc_description[] = {
  {NI_LOGAN_RETCODE_SUCCESS, "SUCCESS"},
  {NI_LOGAN_RETCODE_FAILURE, "FAILURE"},
  {NI_LOGAN_RETCODE_INVALID_PARAM, "INVALID_PARAM"},
  {NI_LOGAN_RETCODE_ERROR_MEM_ALOC, "ERROR_MEM_ALOC"},
  {NI_LOGAN_RETCODE_ERROR_NVME_CMD_FAILED, "ERROR_NVME_CMD_FAILED"},
  {NI_LOGAN_RETCODE_ERROR_INVALID_SESSION, "ERROR_INVALID_SESSION"},
  {NI_LOGAN_RETCODE_ERROR_RESOURCE_UNAVAILABLE, "ERROR_RESOURCE_UNAVAILABLE"},
  {NI_LOGAN_RETCODE_PARAM_INVALID_NAME, "PARAM_INVALID_NAME"},
  {NI_LOGAN_RETCODE_PARAM_INVALID_VALUE, "PARAM_INVALID_VALUE"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_FRATE, "PARAM_ERROR_FRATE"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_BRATE, "PARAM_ERROR_BRATE"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_TRATE, "PARAM_ERROR_TRATE"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_RC_INIT_DELAY, "PARAM_ERROR_RC_INIT_DELAY"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_INTRA_PERIOD, "PARAM_ERROR_INTRA_PERIOD"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_INTRA_QP, "PARAM_ERROR_INTRA_QP"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_GOP_PRESET, "PARAM_ERROR_GOP_PRESET"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_CU_SIZE_MODE, "PARAM_ERROR_CU_SIZE_MODE"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_MX_NUM_MERGE, "PARAM_ERROR_MX_NUM_MERGE"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_DY_MERGE_8X8_EN, "PARAM_ERROR_DY_MERGE_8X8_EN"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_DY_MERGE_16X16_EN,
      "PARAM_ERROR_DY_MERGE_16X16_EN"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_DY_MERGE_32X32_EN,
      "PARAM_ERROR_DY_MERGE_32X32_EN"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_CU_LVL_RC_EN, "PARAM_ERROR_CU_LVL_RC_EN"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_HVS_QP_EN, "PARAM_ERROR_HVS_QP_EN"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_HVS_QP_SCL, "PARAM_ERROR_HVS_QP_SCL"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_MN_QP, "PARAM_ERROR_MN_QP"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_MX_QP, "PARAM_ERROR_MX_QP"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_MX_DELTA_QP, "PARAM_ERROR_MX_DELTA_QP"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_CONF_WIN_TOP, "PARAM_ERROR_CONF_WIN_TOP"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_CONF_WIN_BOT, "PARAM_ERROR_CONF_WIN_BOT"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_CONF_WIN_L, "PARAM_ERROR_CONF_WIN_L"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_CONF_WIN_R, "PARAM_ERROR_CONF_WIN_R"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_USR_RMD_ENC_PARAM,
      "PARAM_ERROR_USR_RMD_ENC_PARAM"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_BRATE_LT_TRATE, "PARAM_ERROR_BRATE_LT_TRATE"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_RCINITDELAY, "PARAM_ERROR_RCINITDELAY"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_RCENABLE, "PARAM_ERROR_RCENABLE"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_MAXNUMMERGE, "PARAM_ERROR_MAXNUMMERGE"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_CUSTOM_GOP, "PARAM_ERROR_CUSTOM_GOP"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_PIC_WIDTH, "PARAM_ERROR_PIC_WIDTH"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_PIC_HEIGHT, "PARAM_ERROR_PIC_HEIGHT"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_DECODING_REFRESH_TYPE,
      "PARAM_ERROR_DECODING_REFRESH_TYPE"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_CUSIZE_MODE_8X8_EN,
      "PARAM_ERROR_CUSIZE_MODE_8X8_EN"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_CUSIZE_MODE_16X16_EN,
      "PARAM_ERROR_CUSIZE_MODE_16X16_EN"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_CUSIZE_MODE_32X32_EN,
      "PARAM_ERROR_CUSIZE_MODE_32X32_EN"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_TOO_BIG, "PARAM_ERROR_TOO_BIG"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_TOO_SMALL, "PARAM_ERROR_TOO_SMALL"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_ZERO, "PARAM_ERROR_ZERO"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_OOR, "PARAM_ERROR_OOR"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_WIDTH_TOO_BIG, "PARAM_ERROR_WIDTH_TOO_BIG"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_WIDTH_TOO_SMALL, "PARAM_ERROR_WIDTH_TOO_SMALL"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_HEIGHT_TOO_BIG, "PARAM_ERROR_HEIGHT_TOO_BIG"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_HEIGHT_TOO_SMALL,
      "PARAM_ERROR_HEIGHT_TOO_SMALL"},
  {NI_LOGAN_RETCODE_PARAM_ERROR_AREA_TOO_BIG, "PARAM_ERROR_AREA_TOO_BIG"},
  {NI_LOGAN_RETCODE_ERROR_EXCEED_MAX_NUM_SESSIONS,
      "ERROR_EXCEED_MAX_NUM_SESSIONS"},
  {NI_LOGAN_RETCODE_ERROR_GET_DEVICE_POOL, "ERROR_GET_DEVICE_POOL"},
  {NI_LOGAN_RETCODE_ERROR_LOCK_DOWN_DEVICE, "ERROR_LOCK_DOWN_DEVICE"},
  {NI_LOGAN_RETCODE_ERROR_UNLOCK_DEVICE, "ERROR_UNLOCK_DEVICE"},
  {NI_LOGAN_RETCODE_ERROR_OPEN_DEVICE, "ERROR_OPEN_DEVICE"},
  {NI_LOGAN_RETCODE_ERROR_INVALID_HANDLE, "ERROR_INVALID_HANDLE"},
  {NI_LOGAN_RETCODE_ERROR_INVALID_ALLOCATION_METHOD,
      "ERROR_INVALID_ALLOCATION_METHOD"},
  {NI_LOGAN_RETCODE_ERROR_VPU_RECOVERY, "ERROR_VPU_RECOVERY"},
  {NI_LOGAN_RETCODE_PARAM_GOP_INTRA_INCOMPATIBLE,
      "PARAM_GOP_INTRA_INCOMPATIBLE"},

  {NI_LOGAN_RETCODE_NVME_SC_WRITE_BUFFER_FULL, "NVME_SC_WRITE_BUFFER_FULL"},
  {NI_LOGAN_RETCODE_NVME_SC_RESOURCE_UNAVAILABLE,
      "NVME_SC_RESOURCE_UNAVAILABLE"},
  {NI_LOGAN_RETCODE_NVME_SC_RESOURCE_IS_EMPTY, "NVME_SC_RESOURCE_IS_EMPTY"},
  {NI_LOGAN_RETCODE_NVME_SC_RESOURCE_NOT_FOUND, "NVME_SC_RESOURCE_NOT_FOUND"},
  {NI_LOGAN_RETCODE_NVME_SC_REQUEST_NOT_COMPLETED,
      "NVME_SC_REQUEST_NOT_COMPLETED"},
  {NI_LOGAN_RETCODE_NVME_SC_REQUEST_IN_PROGRESS, "NVME_SC_REQUEST_IN_PROGRESS"},
  {NI_LOGAN_RETCODE_NVME_SC_INVALID_PARAMETER, "NVME_SC_INVALID_PARAMETER"},
  {NI_LOGAN_RETCODE_NVME_SC_VPU_RECOVERY, "NVME_SC_VPU_RECOVERY"},
  {NI_LOGAN_RETCODE_NVME_SC_VPU_RSRC_INSUFFICIENT,
      "NVME_SC_VPU_RSRC_INSUFFICIENT"},
  {NI_LOGAN_RETCODE_NVME_SC_VPU_GENERAL_ERROR, "NVME_SC_VPU_GENERAL_ERROR"},

  {NI_LOGAN_RETCODE_DEFAULT_SESSION_ERR_NO, "DEFAULT_SESSION_ERR_NO"},
};

static const char *
ni_logan_get_rc_txt (ni_logan_retcode_t rc)
{
  int i;
  for (i = 0;
      i <
      sizeof (ni_logan_err_rc_description) /
      sizeof (ni_logan_err_rc_txt_entry_t); i++) {
    if (rc == ni_logan_err_rc_description[i].rc) {
      return ni_logan_err_rc_description[i].txt;
    }
  }
  return "rc not supported";
}

static int
get_key_value (char *p_str, char *key, char *value)
{
  if (!p_str || !key || !value) {
    return 1;
  }

  char *p = strchr (p_str, '=');
  if (!p) {
    return 1;
  } else {
    *p = '\0';
    key[0] = '\0';
    value[0] = '\0';
    strncat (key, p_str, strlen (p_str));
    strncat (value, p + 1, strlen (p + 1));
    return 0;
  }
}

gint
gst_ni_retrieve_xcoder_params (ni_logan_session_context_t * ctx,
    const char xcoderParams[], ni_logan_encoder_params_t * params)
{
  char key[64], value[64];
  char *xcoder_opts;
  char *curr, *colon_pos;
  int ret = 0;

  if (!xcoderParams) {
    return 0;
  }

  xcoder_opts = g_strdup (xcoderParams);
  curr = xcoder_opts;

  while (*curr) {
    colon_pos = strchr (curr, ':');

    if (colon_pos) {
      *colon_pos = '\0';
    }

    if (strlen (curr) > sizeof (key) + sizeof (value) - 1 ||
        get_key_value (curr, key, value)) {
      GST_WARNING ("Error: xcoder-params p_config key/value not "
          "retrieved: %s\n", curr);
      ret = -1;
      break;
    }
    ret = ni_logan_encoder_params_set_value (params, key, value, ctx);
    switch (ret) {
      case NI_LOGAN_RETCODE_PARAM_INVALID_NAME:
        GST_WARNING ("Error: unknown option: %s.\n", key);
        break;
      case NI_LOGAN_RETCODE_PARAM_INVALID_VALUE:
        GST_WARNING ("Error: invalid value for %s: %s.\n", key, value);
        break;
      default:
        break;
    }

    if (NI_LOGAN_RETCODE_SUCCESS != ret) {
      GST_WARNING ("Error: config parsing failed %d: %s\n", ret,
          ni_logan_get_rc_txt (ret));
      break;
    }

    if (colon_pos) {
      curr = colon_pos + 1;
    } else {
      curr += strlen (curr);
    }
  }
  g_free (xcoder_opts);
  return ret;
}

gint
gst_ni_retrieve_gop_params (const char gopParams[],
    ni_logan_encoder_params_t * params)
{
  char key[64], value[64];
  char *xcoder_gop;
  char *curr, *colon_pos;
  int ret = 0;

  if (!gopParams) {
    return 0;
  }

  xcoder_gop = g_strdup (gopParams);
  curr = xcoder_gop;

  while (*curr) {
    colon_pos = strchr (curr, ':');

    if (colon_pos) {
      *colon_pos = '\0';
    }

    if (strlen (curr) > sizeof (key) + sizeof (value) - 1 ||
        get_key_value (curr, key, value)) {
      GST_ERROR ("Error: xcoder-params p_config key/value not "
          "retrieved: %s\n", curr);
      ret = -1;
      break;
    }
    ret = ni_logan_encoder_gop_params_set_value (params, key, value);
    switch (ret) {
      case NI_LOGAN_RETCODE_PARAM_INVALID_NAME:
        GST_ERROR ("Unknown option: %s.\n", key);
        ret = NI_EINVAL;
        goto done;
      case NI_LOGAN_RETCODE_PARAM_ERROR_TOO_BIG:
        GST_ERROR ("Invalid custom GOP parameters: %s too big\n", key);
        ret = NI_EINVAL;
        goto done;
      case NI_LOGAN_RETCODE_PARAM_ERROR_TOO_SMALL:
        GST_ERROR ("Invalid custom GOP parameters: %s too small\n", key);
        ret = NI_EINVAL;
        goto done;
      case NI_LOGAN_RETCODE_PARAM_ERROR_OOR:
        GST_ERROR ("Invalid custom GOP parameters: %s out of range \n", key);
        ret = NI_EINVAL;
        goto done;
      case NI_LOGAN_RETCODE_PARAM_ERROR_ZERO:
        GST_ERROR
            ("Invalid custom GOP paramaters: Error setting option %s to value 0\n",
            key);
        ret = NI_EINVAL;
        goto done;
      case NI_LOGAN_RETCODE_PARAM_INVALID_VALUE:
        GST_ERROR ("Invalid value for GOP param %s: %s.\n", key, value);
        ret = NI_EINVAL;
        goto done;
      case NI_LOGAN_RETCODE_FAILURE:
        GST_ERROR ("Generic failure during xcoder-params setting for %s\n",
            key);
        ret = NI_EINVAL;
        goto done;
      default:
        break;
    }

    if (colon_pos) {
      curr = colon_pos + 1;
    } else {
      curr += strlen (curr);
    }
  }
done:
  g_free (xcoder_gop);
  return ret;
}

//int
//ni_build_frame_pool(ni_logan_session_context_t *ctx,int width, int height,
//                    GstVideoFormat out_format, int pool_size)
//{
//  int rc;
//  int scaler_format;
//  int options;
//
//  scaler_format = convertGstVideoFormatToGC620Format(out_format);
//  options = NI_SCALER_FLAG_IO |  NI_SCALER_FLAG_PC;
//  if (ctx->isP2P) {
//    options |= NI_SCALER_FLAG_P2;
//  }
//
//  /* Allocate a pool of frames by the scaler */
//  rc = ni_logan_device_alloc_frame(ctx,
//                             NI_ALIGN(width,2),
//                             NI_ALIGN(height,2),
//                             scaler_format,
//                             options,
//                             0, // rec width
//                             0, // rec height
//                             0, // rec X pos
//                             0, // rec Y pos
//                             pool_size, // rgba color/pool size
//                             0, // frame index
//                             NI_DEVICE_TYPE_SCALER);
//
//  return rc;
//}

GstVideoFormat
convertNIPixToGstVideoFormat (ni_logan_pix_fmt_t ni_pix_fmt)
{
  GstVideoFormat gstVideoFormat = GST_VIDEO_FORMAT_UNKNOWN;
  switch (ni_pix_fmt) {
    case NI_LOGAN_PIX_FMT_YUV420P:
      gstVideoFormat = GST_VIDEO_FORMAT_I420;
      break;
    case NI_LOGAN_PIX_FMT_YUV420P10LE:
      gstVideoFormat = GST_VIDEO_FORMAT_I420_10LE;
      break;
    default:
      GST_ERROR ("Unsupported ni_pix_fmt [%d]\n", ni_pix_fmt);
      break;
  }
  return gstVideoFormat;
}

ni_logan_pix_fmt_t
convertGstVideoFormatToNIPix (GstVideoFormat gstVideoFormat)
{
  ni_logan_pix_fmt_t niPixFmt = NI_LOGAN_PIX_FMT_YUV420P;
  switch (gstVideoFormat) {
    case GST_VIDEO_FORMAT_I420:
      niPixFmt = NI_LOGAN_PIX_FMT_YUV420P;
      break;
    case GST_VIDEO_FORMAT_I420_10LE:
      niPixFmt = NI_LOGAN_PIX_FMT_YUV420P10LE;
      break;
    default:
      GST_ERROR ("Unsupported gst video format [%d]\n", gstVideoFormat);
      break;
  }

  return niPixFmt;
}

int
copy_ni_to_gst_frame (const ni_logan_frame_t * src, GstVideoFrame * dst,
    ni_logan_pix_fmt_t niPixFmt)
{
  int src_linesize[4], src_height[4];
  int i, h, nb_planes;
  uint8_t *src_line, *dst_line;

  nb_planes = dst->info.finfo->n_planes;

  switch (niPixFmt) {
    case NI_LOGAN_PIX_FMT_YUV420P:
      src_linesize[0] = NI_ALIGN (dst->info.width, 128);
      src_linesize[1] = NI_ALIGN (dst->info.width / 2, 128);
      src_linesize[2] = src_linesize[1];
      src_linesize[3] = 0;

      src_height[0] = dst->info.height;
      src_height[1] = NI_ALIGN (dst->info.height, 2) / 2;
      src_height[2] = src_height[1];
      src_height[3] = 0;
      break;
    case NI_LOGAN_PIX_FMT_YUV420P10LE:
      src_linesize[0] = NI_ALIGN (dst->info.width * 2, 128);
      src_linesize[1] = NI_ALIGN (dst->info.width, 128);
      src_linesize[2] = src_linesize[1];
      src_linesize[3] = 0;

      src_height[0] = dst->info.height;
      src_height[1] = NI_ALIGN (dst->info.height, 2) / 2;
      src_height[2] = src_height[1];
      src_height[3] = 0;
      break;
    default:
      GST_ERROR ("Unsupported fmt:%d\n", niPixFmt);
      return -1;
  }

  for (i = 0; i < nb_planes; i++) {
    dst_line = dst->data[i];
    src_line = src->p_data[i];

    for (h = 0; h < src_height[i]; h++) {
      memcpy (dst_line, src_line,
          gst_ni_min (src_linesize[i], GST_VIDEO_FRAME_PLANE_STRIDE (dst, i)));
      dst_line += GST_VIDEO_FRAME_PLANE_STRIDE (dst, i);
      src_line += src_linesize[i];
    }
  }

  return 0;
}

int
copy_gst_to_ni_frame (const int *dst_stride, ni_logan_frame_t * dst,
    GstVideoFrame * frame)
{
  int src_height[4], hpad[4], vpad[4];
  int i, j, h, nb_planes;
  uint8_t *src_line, *dst_line, YUVsample, *sample, *dest;
  uint16_t lastidx;
  bool tenBit;
  ni_logan_pix_fmt_t niPixFmt = NI_LOGAN_PIX_FMT_YUV420P;

  nb_planes = frame->info.finfo->n_planes;
  switch (niPixFmt) {
    case NI_LOGAN_PIX_FMT_YUV420P:
      hpad[0] =
          gst_ni_max (dst_stride[0] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0),
          0);
      hpad[1] =
          gst_ni_max (dst_stride[1] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 1),
          0);
      hpad[2] =
          gst_ni_max (dst_stride[2] - GST_VIDEO_FRAME_PLANE_STRIDE (frame, 2),
          0);
      hpad[3] = 0;

      src_height[0] = frame->info.height;
      src_height[1] = NI_ALIGN (frame->info.height, 2) / 2;
      src_height[2] = NI_ALIGN (frame->info.height, 2) / 2;
      src_height[3] = 0;

      vpad[0] = NI_ALIGN (src_height[0], 2) - src_height[0];
      vpad[1] = NI_ALIGN (src_height[1], 2) - src_height[1];
      vpad[2] = NI_ALIGN (src_height[2], 2) - src_height[2];
      vpad[3] = 0;

      tenBit = false;
      break;
  }
  for (i = 0; i < nb_planes; i++) {
    dst_line = dst->p_data[i];
    src_line = GST_VIDEO_FRAME_PLANE_DATA (frame, i);

    for (h = 0; h < src_height[i]; h++) {
      memcpy (dst_line, src_line,
          gst_ni_min (GST_VIDEO_FRAME_PLANE_STRIDE (frame, i), dst_stride[i]));
      if (hpad[i]) {
        lastidx = GST_VIDEO_FRAME_PLANE_STRIDE (frame, i);

        if (tenBit) {
          sample = &src_line[lastidx - 2];
          dest = &dst_line[lastidx];

          /* two bytes per sample */
          for (j = 0; j < hpad[i] / 2; j++) {
            memcpy (dest, sample, 2);
            dest += 2;
          }
        } else {
          YUVsample = dst_line[lastidx - 1];
          memset (&dst_line[lastidx], YUVsample, hpad[i]);
        }
      }

      src_line += GST_VIDEO_FRAME_PLANE_STRIDE (frame, i);
      dst_line += dst_stride[i];
    }

    /* Extend the height by cloning the last line */
    src_line = dst_line - dst_stride[i];
    for (h = 0; h < vpad[i]; h++) {
      memcpy (dst_line, src_line, dst_stride[i]);
      dst_line += dst_stride[i];
    }
  }

  return 0;
}

void
ni_set_bit_depth_and_encoding_type (int8_t * p_bit_depth,
    int8_t * p_enc_type, ni_logan_pix_fmt_t pix_fmt)
{
  // bit depth is 1 for 8-bit format, 2 for 10-bit format
  // encoding type should be 1 for planar or packed, 0 for semi-planar

  switch (pix_fmt) {
    case NI_LOGAN_PIX_FMT_YUV420P:
      *p_bit_depth = 1;         // 8-bits per component
      *p_enc_type = 1;          // planar
      break;

    case NI_LOGAN_PIX_FMT_YUV420P10LE:
      *p_bit_depth = 2;         // 10-bits per component
      *p_enc_type = 1;          // planar
      break;

    default:
      // use default values if we've supported a new pixel format
      *p_bit_depth = 1;         // 8-bits per component
      *p_enc_type = 1;          // planar or packed
      break;
  }
}

gint32
calculateSwFrameSize (int width, int height, ni_logan_pix_fmt_t pix_fmt)
{
  int32_t frameSize = 0;
  switch (pix_fmt) {
    case NI_LOGAN_PIX_FMT_YUV420P:
      frameSize = width * height * 3 / 2;
      break;
    case NI_LOGAN_PIX_FMT_YUV420P10LE:
      frameSize = width * height * 3;
      break;
  }
  return frameSize;
}

/* For color hdr metadata all the UNKNOWN case should make it to 2
 * which means UNSPECIFIED on libxcoder & ffmpeg
 * */

int
map_gst_color_primaries (GstVideoColorPrimaries primaries)
{
  switch (primaries) {
    case GST_VIDEO_COLOR_PRIMARIES_UNKNOWN:
      return 2;
    case GST_VIDEO_COLOR_PRIMARIES_BT709:
      return 1;
    case GST_VIDEO_COLOR_PRIMARIES_BT470M:
      return 4;
    case GST_VIDEO_COLOR_PRIMARIES_BT470BG:
      return 5;
    case GST_VIDEO_COLOR_PRIMARIES_SMPTE170M:
      return 6;
    case GST_VIDEO_COLOR_PRIMARIES_SMPTE240M:
      return 7;
    case GST_VIDEO_COLOR_PRIMARIES_FILM:
      return 8;
    case GST_VIDEO_COLOR_PRIMARIES_BT2020:
      return 9;
    case GST_VIDEO_COLOR_PRIMARIES_ADOBERGB:
      return 13;
    case GST_VIDEO_COLOR_PRIMARIES_SMPTEST428:
      return 10;
    case GST_VIDEO_COLOR_PRIMARIES_SMPTERP431:
      return 11;
    case GST_VIDEO_COLOR_PRIMARIES_SMPTEEG432:
      return 12;
    case GST_VIDEO_COLOR_PRIMARIES_EBU3213:
      return 22;
    default:
      return 2;
  }
}

int
map_gst_color_trc (GstVideoTransferFunction transferFunction)
{
  switch (transferFunction) {
    case GST_VIDEO_TRANSFER_UNKNOWN:
      return 2;
    case GST_VIDEO_TRANSFER_GAMMA10:
    case GST_VIDEO_TRANSFER_GAMMA18:
    case GST_VIDEO_TRANSFER_GAMMA20:
    case GST_VIDEO_TRANSFER_GAMMA22:
      return 4;
    case GST_VIDEO_TRANSFER_BT709:
      return 1;
    case GST_VIDEO_TRANSFER_SMPTE240M:
      return 7;
    case GST_VIDEO_TRANSFER_LOG100:
      return 9;
    case GST_VIDEO_TRANSFER_LOG316:
      return 10;
    case GST_VIDEO_TRANSFER_BT2020_12:
      return 15;
    case GST_VIDEO_TRANSFER_BT2020_10:
      return 14;
    case GST_VIDEO_TRANSFER_SMPTE2084:
      return 16;
    case GST_VIDEO_TRANSFER_ARIB_STD_B67:
      return 18;
    default:
      return 2;
  }
}

int
map_gst_color_space (GstVideoColorMatrix matrix)
{
  switch (matrix) {
    case GST_VIDEO_COLOR_MATRIX_UNKNOWN:
      return 2;
    case GST_VIDEO_COLOR_MATRIX_FCC:
      return 4;
    case GST_VIDEO_COLOR_MATRIX_BT709:
      return 1;
    case GST_VIDEO_COLOR_MATRIX_SMPTE240M:
      return 7;
    case GST_VIDEO_COLOR_MATRIX_BT2020:
      return 9;
    default:
      return 2;
  }
}
