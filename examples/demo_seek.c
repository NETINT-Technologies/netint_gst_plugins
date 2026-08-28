/*******************************************************************************
 *
 * Copyright (C) 2025 NETINT Technologies
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gst/gst.h>
#include <glib.h>
#include <getopt.h>

#define LOG_INFO(msg) g_print("INFO: %s\n", msg)
#define LOG_DEBUG(msg) g_print("DEBUG: %s\n", msg)
#define LOG_WARN(msg) g_print("WARN: %s\n", msg)
#define LOG_ERROR(msg) g_printerr("ERROR: %s\n", msg)

typedef struct
{
  const gchar **allowed_video_decoders; // NULL-terminated array
  const gchar *video_encoder;
  const gchar *input_file;
  const gchar *output_file;
  gint64 clip_start_seconds;
  gint64 clip_duration_seconds;
} Parameters;

typedef struct
{
  gboolean terminate;
  gboolean playing;
  GstClockTime duration;
} PipelineState;

typedef struct
{
  guint32 counter;
  GMutex mutex;
} BlockingCounter;

typedef struct
{
  GstElement *mp4mux;
  const gchar *video_encoder;
  gboolean *prerolled;
  BlockingCounter *blocking_counter;
  GstPipeline *pipeline;
} CallbackData;

typedef struct
{
  GstPipeline *pipeline;
  gboolean *prerolled;
  BlockingCounter *blocking_counter;
} ProbeData;

static void decodebin_pad_added_cb (GstElement * decodebin, GstPad * pad,
    gpointer user_data);
static void decodebin_no_more_pads_cb (GstElement * decodebin,
    gpointer user_data);
static GstPadProbeReturn pad_blocked_cb (GstPad * pad, GstPadProbeInfo * info,
    gpointer user_data);
static gboolean seek_pipeline (GstPipeline * pipeline, gint64 start_seconds,
    gint64 duration_seconds);
static void set_factory_ranks (const Parameters * params);
static gboolean handle_message (PipelineState * state, GstMessage * msg,
    GstPipeline * pipeline, gint64 clip_start_seconds,
    gint64 clip_duration_seconds);
static const gchar *get_parser_for_encoder (const gchar * encoder_name);

static void
print_usage (const gchar * program_name)
{
  printf ("Usage: %s [OPTIONS]\n", program_name);
  printf ("Options:\n");
  printf ("  -i, --input FILE        Input file path (required)\n");
  printf ("  -o, --output FILE       Output file path (required)\n");
  printf ("  -e, --encoder ENCODER  "
      " Video encoder to use (default: niquadrah265enc)\n");
  printf ("  -d, --decoders DECODERS "
      "Comma-separated list of allowed decoders"
      "  (default: niquadrah264dec,niquadrah265dec)\n");
  printf ("  -s, --start SECONDS    "
      " Clip start time in seconds (default: 600)\n");
  printf ("  -l, --length SECONDS    Clip duration in seconds (default: 20)\n");
  printf ("  -h, --help              Show this help message\n");
}

static gchar **
parse_decoder_list (const gchar * decoder_str)
{
  if (!decoder_str || strlen (decoder_str) == 0) {
    return NULL;
  }

  gchar *str_copy = g_strdup (decoder_str);
  if (!str_copy) {
    return NULL;
  }

  gint count = 1;
  for (const gchar * p = decoder_str; *p; p++) {
    if (*p == ',')
      count++;
  }

  gchar **decoders = (gchar **) g_malloc0 ((count + 1) * sizeof (gchar *));
  if (!decoders) {
    g_free (str_copy);
    return NULL;
  }

  gchar *token = strtok (str_copy, ",");
  gint i = 0;
  while (token && i < count) {
    while (*token == ' ')
      token++;
    gchar *end = token + strlen (token) - 1;
    while (end > token && *end == ' ')
      *end-- = '\0';

    decoders[i] = g_strdup (token);
    if (!decoders[i]) {
      for (gint j = 0; j < i; j++) {
        g_free (decoders[j]);
      }
      g_free (decoders);
      g_free (str_copy);
      return NULL;
    }
    i++;
    token = strtok (NULL, ",");
  }

  g_free (str_copy);
  return decoders;
}

static void
free_decoder_list (gchar ** decoders)
{
  if (!decoders)
    return;

  for (gint i = 0; decoders[i] != NULL; i++) {
    g_free (decoders[i]);
  }
  g_free (decoders);
}

static const gchar *
get_parser_for_encoder (const gchar * encoder_name)
{
  if (strstr (encoder_name, "264")) {
    return "h264parse";
  } else if (strstr (encoder_name, "265") || strstr (encoder_name, "hevc")) {
    return "h265parse";
  } else {
    LOG_WARN ("Unknown encoder type, defaulting to h265parse");
    return "h265parse";
  }
}

static BlockingCounter *
blocking_counter_new (void)
{
  BlockingCounter *counter = g_new0 (BlockingCounter, 1);
  counter->counter = 1;
  g_mutex_init (&counter->mutex);
  return counter;
}

static void
blocking_counter_free (BlockingCounter * counter)
{
  if (counter) {
    g_mutex_clear (&counter->mutex);
    g_free (counter);
  }
}

static void
blocking_counter_inc (BlockingCounter * counter)
{
  g_mutex_lock (&counter->mutex);
  counter->counter++;
  g_mutex_unlock (&counter->mutex);
}

static void
blocking_counter_dec (BlockingCounter * counter, GstPipeline * pipeline,
    gboolean * prerolled)
{
  g_mutex_lock (&counter->mutex);

  if (*prerolled) {
    LOG_WARN ("BlockingCounter::dec called when pipeline is prerolled");
    g_mutex_unlock (&counter->mutex);
    return;
  }

  counter->counter--;
  if (counter->counter == 0) {
    LOG_INFO ("Pipeline is prerolled");
    *prerolled = TRUE;

    GstBus *bus = gst_pipeline_get_bus (pipeline);
    GstStructure *structure = gst_structure_new_empty ("ExPrerolled");
    GstMessage *msg = gst_message_new_application (GST_OBJECT (pipeline),
        structure);
    gst_bus_post (bus, msg);
    gst_object_unref (bus);
  }

  g_mutex_unlock (&counter->mutex);
}

gboolean
run (const Parameters * params)
{
  BlockingCounter *blocking_counter = blocking_counter_new ();
  gboolean prerolled = FALSE;

  set_factory_ranks (params);

  GstElement *filesrc = gst_element_factory_make ("filesrc", "filesrc");
  g_object_set (G_OBJECT (filesrc), "location", params->input_file, NULL);

  GstElement *decodebin = gst_element_factory_make ("decodebin", "decodebin");
  GstElement *mp4mux = gst_element_factory_make ("mp4mux", "mp4mux");

  GstElement *filesink = gst_element_factory_make ("filesink", "filesink");
  g_object_set (G_OBJECT (filesink), "location", params->output_file, NULL);

  if (!filesrc || !decodebin || !mp4mux || !filesink) {
    LOG_ERROR ("Failed to create elements");
    blocking_counter_free (blocking_counter);
    return FALSE;
  }

  GstPipeline *pipeline = GST_PIPELINE (gst_pipeline_new
      ("transcoding-pipeline"));

  gst_bin_add_many (GST_BIN (pipeline), filesrc, decodebin, mp4mux, filesink,
      NULL);

  if (!gst_element_link (filesrc, decodebin)) {
    LOG_ERROR ("Failed to link filesrc and decodebin");
    blocking_counter_free (blocking_counter);
    return FALSE;
  }

  if (!gst_element_link (mp4mux, filesink)) {
    LOG_ERROR ("Failed to link mp4mux and filesink");
    blocking_counter_free (blocking_counter);
    return FALSE;
  }

  CallbackData *callback_data = g_new0 (CallbackData, 1);
  callback_data->mp4mux = mp4mux;
  callback_data->video_encoder = params->video_encoder;
  callback_data->prerolled = &prerolled;
  callback_data->blocking_counter = blocking_counter;
  callback_data->pipeline = pipeline;

  g_signal_connect (decodebin, "pad-added", G_CALLBACK (decodebin_pad_added_cb),
      callback_data);
  g_signal_connect (decodebin, "no-more-pads",
      G_CALLBACK (decodebin_no_more_pads_cb), callback_data);

  PipelineState state = { FALSE, FALSE, GST_CLOCK_TIME_NONE };
  GstBus *bus = gst_pipeline_get_bus (pipeline);

  LOG_INFO ("Starting the pipeline");
  gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_PAUSED);

  while (!state.terminate) {
    GstMessage *msg = gst_bus_timed_pop (bus, 500 * GST_MSECOND);

    if (msg) {
      if (!handle_message (&state, msg, pipeline, params->clip_start_seconds,
              params->clip_duration_seconds)) {
        gst_message_unref (msg);
        break;
      }
      gst_message_unref (msg);
    } else if (state.playing) {
      gint64 pos;
      if (gst_element_query_position (GST_ELEMENT (pipeline), GST_FORMAT_TIME,
              &pos)) {
        if (state.duration == GST_CLOCK_TIME_NONE) {
          gst_element_query_duration (GST_ELEMENT (pipeline), GST_FORMAT_TIME,
              (gint64 *) & state.duration);
        }

        gchar info[256];
        g_snprintf (info, sizeof (info),
            "Position %" GST_TIME_FORMAT " / %" GST_TIME_FORMAT,
            GST_TIME_ARGS (pos), GST_TIME_ARGS (state.duration));
        LOG_INFO (info);
      }
    }
  }

  LOG_INFO ("Ending the pipeline");
  gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_NULL);

  gst_object_unref (bus);
  gst_object_unref (pipeline);
  g_free (callback_data);
  blocking_counter_free (blocking_counter);

  return TRUE;
}

static void
set_factory_ranks (const Parameters * params)
{
  GList *factories =
      gst_element_factory_list_get_elements (GST_ELEMENT_FACTORY_TYPE_DECODER |
      GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO, GST_RANK_NONE);

  for (GList * l = factories; l != NULL; l = l->next) {
    GstElementFactory *factory = GST_ELEMENT_FACTORY (l->data);
    const gchar *factory_name =
        gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory));

    gboolean allowed = FALSE;
    if (params->allowed_video_decoders) {
      for (gint i = 0; params->allowed_video_decoders[i] != NULL; i++) {
        if (strcmp (params->allowed_video_decoders[i], factory_name) == 0) {
          allowed = TRUE;
          break;
        }
      }
    }

    if (allowed) {
      gst_plugin_feature_set_rank (GST_PLUGIN_FEATURE (factory),
          GST_RANK_PRIMARY);
      LOG_DEBUG ("Setting factory rank to PRIMARY");
    } else {
      gst_plugin_feature_set_rank (GST_PLUGIN_FEATURE (factory), GST_RANK_NONE);
    }
  }

  gst_plugin_feature_list_free (factories);
}

static gboolean
handle_message (PipelineState * state, GstMessage * msg, GstPipeline * pipeline,
    gint64 clip_start_seconds, gint64 clip_duration_seconds)
{
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ERROR:{
      GError *err;
      gchar *debug_info;
      gst_message_parse_error (msg, &err, &debug_info);

      gchar error_msg[512];
      g_snprintf (error_msg, sizeof (error_msg), "Error from %s: %s",
          GST_OBJECT_NAME (msg->src), err->message);
      LOG_ERROR (error_msg);

      g_clear_error (&err);
      g_free (debug_info);
      state->terminate = TRUE;
      return FALSE;
    }

    case GST_MESSAGE_EOS:
      LOG_INFO ("End-Of-Stream reached");
      state->terminate = TRUE;
      break;

    case GST_MESSAGE_DURATION_CHANGED:
      state->duration = GST_CLOCK_TIME_NONE;
      break;

    case GST_MESSAGE_STATE_CHANGED:{
      if (GST_MESSAGE_SRC (msg) == GST_OBJECT (pipeline)) {
        GstState old_state, new_state, pending_state;
        gst_message_parse_state_changed (msg, &old_state, &new_state,
            &pending_state);

        gchar state_msg[256];
        g_snprintf (state_msg, sizeof (state_msg),
            "Pipeline state changed from %s to %s",
            gst_element_state_get_name (old_state),
            gst_element_state_get_name (new_state));
        LOG_INFO (state_msg);

        state->playing = (new_state == GST_STATE_PLAYING);
      }
      break;
    }

    case GST_MESSAGE_APPLICATION:{
      const GstStructure *structure = gst_message_get_structure (msg);
      if (gst_structure_has_name (structure, "ExPrerolled")) {
        if (!seek_pipeline (pipeline, clip_start_seconds,
                clip_duration_seconds)) {
          return FALSE;
        }

        LOG_INFO ("Setting pipeline to PLAYING");
        gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_PLAYING);
      }
      break;
    }

    default:
      break;
  }

  return TRUE;
}

static void
decodebin_pad_added_cb (GstElement * decodebin, GstPad * pad,
    gpointer user_data)
{
  CallbackData *data = (CallbackData *) user_data;

  gchar pad_info[256];
  g_snprintf (pad_info, sizeof (pad_info), "Received new pad %s from %s",
      GST_PAD_NAME (pad), GST_ELEMENT_NAME (decodebin));
  LOG_DEBUG (pad_info);

  if (*data->prerolled) {
    LOG_WARN ("pad_added called when pipeline is prerolled");
    return;
  }

  blocking_counter_inc (data->blocking_counter);

  ProbeData *probe_data = g_new0 (ProbeData, 1);
  probe_data->pipeline = data->pipeline;
  probe_data->prerolled = data->prerolled;
  probe_data->blocking_counter = data->blocking_counter;

  gst_pad_add_probe (pad, (GstPadProbeType) (GST_PAD_PROBE_TYPE_BLOCK |
          GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
      pad_blocked_cb, probe_data, g_free);

  GstCaps *caps = gst_pad_get_current_caps (pad);
  if (!caps)
    return;

  GstStructure *structure = gst_caps_get_structure (caps, 0);
  const gchar *media_type = gst_structure_get_name (structure);

  if (g_str_has_prefix (media_type, "video/x-raw")) {
    gchar *pad_name = gst_pad_get_name (pad);

    GstElement *input_queue = gst_element_factory_make ("queue",
        g_strdup_printf ("%s_input_queue", pad_name));
    GstElement *videoconvert = gst_element_factory_make ("videoconvert",
        g_strdup_printf ("%s_videoconvert", pad_name));
    GstElement *encoder = gst_element_factory_make (data->video_encoder,
        g_strdup_printf ("%s_%s", pad_name, data->video_encoder));

    const gchar *parser_name = get_parser_for_encoder (data->video_encoder);
    GstElement *parser = gst_element_factory_make (parser_name,
        g_strdup_printf ("%s_%s", pad_name, parser_name));

    GstElement *output_queue = gst_element_factory_make ("queue",
        g_strdup_printf ("%s_output_queue", pad_name));

    if (!input_queue || !videoconvert || !encoder || !parser || !output_queue) {
      LOG_ERROR ("Failed to create video processing elements");
      g_free (pad_name);
      gst_caps_unref (caps);
      return;
    }

    gst_bin_add_many (GST_BIN (data->pipeline), input_queue, videoconvert,
        encoder, parser, output_queue, NULL);

    if (!gst_element_link_many (input_queue, videoconvert, encoder, parser,
            output_queue, NULL)) {
      LOG_ERROR ("Failed to link video processing elements");
      g_free (pad_name);
      gst_caps_unref (caps);
      return;
    }

    gst_element_sync_state_with_parent (input_queue);
    gst_element_sync_state_with_parent (videoconvert);
    gst_element_sync_state_with_parent (encoder);
    gst_element_sync_state_with_parent (parser);
    gst_element_sync_state_with_parent (output_queue);

    GstPad *sinkpad = gst_element_get_static_pad (input_queue, "sink");
    gst_pad_link (pad, sinkpad);
    gst_object_unref (sinkpad);

    GstPad *srcpad = gst_element_get_static_pad (output_queue, "src");
    GstPad *mux_pad = gst_element_request_pad_simple (data->mp4mux, "video_%u");
    gst_pad_link (srcpad, mux_pad);
    gst_object_unref (srcpad);
    gst_object_unref (mux_pad);

    g_free (pad_name);
  } else {
    gchar warn_msg[256];
    g_snprintf (warn_msg, sizeof (warn_msg), "Unsupported pad type: %s",
        media_type);
    LOG_WARN (warn_msg);
  }

  gst_caps_unref (caps);
}

static void
decodebin_no_more_pads_cb (GstElement * decodebin, gpointer user_data)
{
  LOG_DEBUG ("decodebin: no more pads");

  CallbackData *data = (CallbackData *) user_data;

  if (*data->prerolled) {
    LOG_WARN ("no_more_pads called when pipeline is prerolled");
    return;
  }

  blocking_counter_dec (data->blocking_counter, data->pipeline,
      data->prerolled);
}

static GstPadProbeReturn
pad_blocked_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  ProbeData *data = (ProbeData *) user_data;

  if (*data->prerolled) {
    LOG_DEBUG ("pad_blocked_cb: removing blocking probe");
    return GST_PAD_PROBE_REMOVE;
  }

  blocking_counter_dec (data->blocking_counter, data->pipeline,
      data->prerolled);
  return GST_PAD_PROBE_OK;
}

static gboolean
seek_pipeline (GstPipeline * pipeline, gint64 start_seconds,
    gint64 duration_seconds)
{
  gchar msg[256];

  GstState current, pending;
  gst_element_get_state (GST_ELEMENT (pipeline), &current, &pending, 0);
  g_snprintf (msg, sizeof (msg), "Before seek - State: %s, Pending: %s",
      gst_element_state_get_name (current),
      pending ==
      GST_STATE_VOID_PENDING ? "NONE" : gst_element_state_get_name (pending));
  LOG_INFO (msg);

  g_snprintf (msg, sizeof (msg), "Seeking: start=%lds, duration=%lds",
      start_seconds, duration_seconds);
  LOG_INFO (msg);

  gint64 start_ns = start_seconds * GST_SECOND;
  gint64 stop_ns = (start_seconds + duration_seconds) * GST_SECOND;

  gboolean seek_result = gst_element_seek (GST_ELEMENT (pipeline),
      1.0,
      GST_FORMAT_TIME,
      (GstSeekFlags) (GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
      GST_SEEK_TYPE_SET, start_ns,
      GST_SEEK_TYPE_SET, stop_ns);

  g_snprintf (msg, sizeof (msg), "Seek result: %s",
      seek_result ? "SUCCESS" : "FAILED");
  LOG_INFO (msg);

  gst_element_get_state (GST_ELEMENT (pipeline), &current, &pending, 0);
  g_snprintf (msg, sizeof (msg), "After seek - State: %s, Pending: %s",
      gst_element_state_get_name (current),
      pending ==
      GST_STATE_VOID_PENDING ? "NONE" : gst_element_state_get_name (pending));
  LOG_INFO (msg);

  return seek_result;
}

int
main (int argc, char *argv[])
{
  gst_init (&argc, &argv);

  const gchar *default_decoders[] = {
    "niquadrah264dec",
    "niquadrah265dec",
    NULL
  };

  Parameters params = {
    .allowed_video_decoders = default_decoders,
    .video_encoder = "niquadrah265enc",
    .input_file = NULL,
    .output_file = NULL,
    .clip_start_seconds = 600,
    .clip_duration_seconds = 20
  };

  static struct option long_options[] = {
    {"input", required_argument, NULL, 'i'},
    {"output", required_argument, NULL, 'o'},
    {"encoder", required_argument, NULL, 'e'},
    {"decoders", required_argument, NULL, 'd'},
    {"start", required_argument, NULL, 's'},
    {"length", required_argument, NULL, 'l'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0}
  };

  gint option_index = 0;
  gint c;
  gchar *decoder_list = NULL;

  while ((c = getopt_long (argc, argv, "i:o:e:d:s:l:h", long_options,
              &option_index)) != -1) {
    switch (c) {
      case 'i':
        params.input_file = optarg;
        break;
      case 'o':
        params.output_file = optarg;
        break;
      case 'e':
        params.video_encoder = optarg;
        break;
      case 'd':
        decoder_list = optarg;
        break;
      case 's':
        params.clip_start_seconds = atoll (optarg);
        break;
      case 'l':
        params.clip_duration_seconds = atoll (optarg);
        break;
      case 'h':
        print_usage (argv[0]);
        return 0;
      case '?':
        print_usage (argv[0]);
        return -1;
      default:
        break;
    }
  }

  if (decoder_list) {
    gchar **new_decoders = parse_decoder_list (decoder_list);
    if (new_decoders) {
      params.allowed_video_decoders = (const gchar **) new_decoders;
    }
  }

  if (!params.input_file || !params.output_file) {
    LOG_ERROR ("Input and output files must be specified");
    print_usage (argv[0]);
    return -1;
  }

  g_print ("Configuration:\n");
  g_print ("  Input file: %s\n", params.input_file);
  g_print ("  Output file: %s\n", params.output_file);
  g_print ("  Video encoder: %s\n", params.video_encoder);
  g_print ("  Clip start: %ld seconds\n", params.clip_start_seconds);
  g_print ("  Clip duration: %ld seconds\n", params.clip_duration_seconds);
  g_print ("  Allowed decoders:");
  for (gint i = 0; params.allowed_video_decoders[i] != NULL; i++) {
    g_print (" %s", params.allowed_video_decoders[i]);
  }
  g_print ("\n\n");

  gboolean success = run (&params);

  if (params.allowed_video_decoders != (const gchar **) default_decoders) {
    free_decoder_list ((gchar **) params.allowed_video_decoders);
  }

  if (!success) {
    LOG_ERROR ("Transcoding failed");
    return 1;
  }

  LOG_INFO ("Transcoding completed successfully");
  return 0;
}
