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

#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

typedef struct
{
  const gchar **allowed_video_decoders; // NULL-terminated array of strings
  const gchar *video_encoder;
  const gchar *input_file;
  const gchar *output_file;
} Parameters;

typedef struct
{
  gboolean terminate;
  gboolean playing;
  GstClockTime duration;
} PipelineState;

typedef struct
{
  GstElement *pipeline;
  GstElement *mux;
  const gchar *video_encoder;
} PadAddedData;

static void
print_usage (const gchar * program_name)
{
  printf ("Usage: %s [OPTIONS]\n", program_name);
  printf ("Options:\n");
  printf ("  -i, --input FILE         Input file path (mp4)\n");
  printf ("  -o, --output FILE        Output file path (mp4)\n");
  printf ("  -e, --encoder ENCODER    "
      "Video encoder to use (default: niquadrah265enc)\n");
  printf ("  -d, --decoders DECODERS  "
      "Comma-separated list of allowed decoders "
      "(default: niquadrah264dec,niquadrah265dec)\n");
  printf ("  -h, --help               Show this help message\n");
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

  // Allocate array (+1 for NULL terminator)
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

static void
set_factory_ranks (const Parameters * params)
{
  GList *factories =
      gst_element_factory_list_get_elements (GST_ELEMENT_FACTORY_TYPE_DECODER |
      GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO, GST_RANK_NONE);

  for (GList * l = factories; l != NULL; l = l->next) {
    GstElementFactory *factory = GST_ELEMENT_FACTORY (l->data);
    const gchar *factory_name = gst_object_get_name (GST_OBJECT (factory));

    gboolean is_allowed = FALSE;
    if (params->allowed_video_decoders) {
      for (gint i = 0; params->allowed_video_decoders[i] != NULL; i++) {
        if (g_strcmp0 (factory_name, params->allowed_video_decoders[i]) == 0) {
          is_allowed = TRUE;
          break;
        }
      }
    }

    if (is_allowed) {
      g_print ("Setting factory %s rank to PRIMARY\n", factory_name);
      gst_plugin_feature_set_rank (GST_PLUGIN_FEATURE (factory),
          GST_RANK_PRIMARY);
    } else {
      g_print ("Setting factory %s rank to NONE\n", factory_name);
      gst_plugin_feature_set_rank (GST_PLUGIN_FEATURE (factory), GST_RANK_NONE);
    }
  }

  gst_plugin_feature_list_free (factories);
}

static const gchar *
get_parser_for_encoder (const gchar * encoder_name)
{
  if (strstr (encoder_name, "264")) {
    return "h264parse";
  } else if (strstr (encoder_name, "265")) {
    return "h265parse";
  } else {
    g_print ("Warning: Unknown encoder type '%s', defaulting to h265parse\n",
        encoder_name);
    return "h265parse";
  }
}

static void
on_pad_added (GstElement * decodebin, GstPad * pad, gpointer user_data)
{
  PadAddedData *data = (PadAddedData *) user_data;
  GstCaps *caps;
  GstStructure *str;
  const gchar *name;
  GstPad *sink_pad;
  GstPadLinkReturn ret;

  g_print ("Received new pad '%s' from '%s':\n", GST_PAD_NAME (pad),
      GST_ELEMENT_NAME (decodebin));

  caps = gst_pad_get_current_caps (pad);
  if (!caps) {
    g_printerr ("Failed to get caps\n");
    return;
  }

  str = gst_caps_get_structure (caps, 0);
  name = gst_structure_get_name (str);

  if (g_str_has_prefix (name, "video/x-raw")) {
    GstElement *input_queue = gst_element_factory_make ("queue", NULL);
    GstElement *videoconvert = gst_element_factory_make ("videoconvert", NULL);
    GstElement *encoder = gst_element_factory_make (data->video_encoder, NULL);
    GstElement *parser = NULL;
    GstElement *output_queue = gst_element_factory_make ("queue", NULL);

    if (!input_queue || !videoconvert || !encoder || !output_queue) {
      g_printerr ("Failed to create elements\n");
      gst_caps_unref (caps);
      return;
    }

    const gchar *parser_name = get_parser_for_encoder (data->video_encoder);
    if (parser_name) {
      parser = gst_element_factory_make (parser_name, NULL);
      if (!parser) {
        g_printerr ("Failed to create parser '%s'\n", parser_name);
        gst_caps_unref (caps);
        return;
      }
      g_print ("Using parser: %s\n", parser_name);
    } else {
      g_print ("No parser needed for encoder: %s\n", data->video_encoder);
    }

    if (parser) {
      gst_bin_add_many (GST_BIN (data->pipeline), input_queue, videoconvert,
          encoder, parser, output_queue, NULL);
    } else {
      gst_bin_add_many (GST_BIN (data->pipeline), input_queue, videoconvert,
          encoder, output_queue, NULL);
    }

    gboolean link_success = FALSE;
    if (parser) {
      link_success = gst_element_link_many (input_queue, videoconvert, encoder,
          parser, output_queue, NULL);
    } else {
      link_success = gst_element_link_many (input_queue, videoconvert, encoder,
          output_queue, NULL);
    }

    if (!link_success) {
      g_printerr ("Failed to link elements\n");
      gst_caps_unref (caps);
      return;
    }

    gst_element_sync_state_with_parent (input_queue);
    gst_element_sync_state_with_parent (videoconvert);
    gst_element_sync_state_with_parent (encoder);
    if (parser) {
      gst_element_sync_state_with_parent (parser);
    }
    gst_element_sync_state_with_parent (output_queue);

    sink_pad = gst_element_get_static_pad (input_queue, "sink");
    ret = gst_pad_link (pad, sink_pad);
    if (GST_PAD_LINK_FAILED (ret)) {
      g_printerr ("Failed to link decodebin to queue\n");
    }
    gst_object_unref (sink_pad);

    GstPad *mux_pad = gst_element_request_pad_simple (data->mux, "video_%u");
    GstPad *src_pad = gst_element_get_static_pad (output_queue, "src");
    ret = gst_pad_link (src_pad, mux_pad);
    if (GST_PAD_LINK_FAILED (ret)) {
      g_printerr ("Failed to link queue to mux\n");
    }
    gst_object_unref (src_pad);
    gst_object_unref (mux_pad);
  } else {
    g_print ("Unsupported pad type: %s. Ignoring.\n", name);
  }

  gst_caps_unref (caps);
}

static void
on_unknown_type (GstElement * decodebin, GstPad * pad, GstCaps * caps,
    gpointer user_data)
{
  gchar *caps_str = gst_caps_to_string (caps);
  g_print ("Unknown type: %s\n", caps_str);
  g_free (caps_str);
}

static void
on_drained (GstElement * decodebin, gpointer user_data)
{
  g_print ("Decodebin: finished decoding\n");
}

static gboolean
handle_message (PipelineState * state, GstMessage * msg, GstElement * pipeline)
{
  GError *err;
  gchar *debug_info;

  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ERROR:
      gst_message_parse_error (msg, &err, &debug_info);
      g_printerr ("Error received from element %s: %s\n",
          GST_OBJECT_NAME (msg->src), err->message);
      g_printerr ("Debugging information: %s\n",
          debug_info ? debug_info : "none");
      g_clear_error (&err);
      g_free (debug_info);
      state->terminate = TRUE;
      return FALSE;

    case GST_MESSAGE_EOS:
      g_print ("End-Of-Stream reached.\n");
      state->terminate = TRUE;
      break;

    case GST_MESSAGE_DURATION_CHANGED:
      state->duration = GST_CLOCK_TIME_NONE;
      break;

    case GST_MESSAGE_STATE_CHANGED:
      if (GST_MESSAGE_SRC (msg) == GST_OBJECT (pipeline)) {
        GstState old_state, new_state, pending_state;
        gst_message_parse_state_changed (msg, &old_state, &new_state,
            &pending_state);
        g_print ("Pipeline state changed from %s to %s\n",
            gst_element_state_get_name (old_state),
            gst_element_state_get_name (new_state));
        state->playing = (new_state == GST_STATE_PLAYING);
      }
      break;

    default:
      break;
  }

  return TRUE;
}

static gint
run_main_loop (GstElement * pipeline)
{
  PipelineState state = {
    .terminate = FALSE,
    .playing = FALSE,
    .duration = GST_CLOCK_TIME_NONE
  };

  GstBus *bus = gst_element_get_bus (pipeline);

  g_print ("Starting the pipeline\n");
  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  while (!state.terminate) {
    GstMessage *msg = gst_bus_timed_pop (bus, 500 * GST_MSECOND);

    if (msg != NULL) {
      if (!handle_message (&state, msg, pipeline)) {
        gst_message_unref (msg);
        gst_object_unref (bus);
        gst_element_set_state (pipeline, GST_STATE_NULL);
        return -1;
      }
      gst_message_unref (msg);
    } else {
      if (state.playing) {
        gint64 pos, len;

        if (gst_element_query_position (pipeline, GST_FORMAT_TIME, &pos)) {
          if (state.duration == GST_CLOCK_TIME_NONE) {
            if (gst_element_query_duration (pipeline, GST_FORMAT_TIME, &len)) {
              state.duration = len;
            }
          }

          g_print ("Position %" GST_TIME_FORMAT " / %" GST_TIME_FORMAT "\r",
              GST_TIME_ARGS (pos), GST_TIME_ARGS (state.duration));
        }
      }
    }
  }

  g_print ("\nEnding the pipeline\n");
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (bus);

  return 0;
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
    .output_file = NULL
  };

  static struct option long_options[] = {
    {"input", required_argument, NULL, 'i'},
    {"output", required_argument, NULL, 'o'},
    {"encoder", required_argument, NULL, 'e'},
    {"decoders", required_argument, NULL, 'd'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0}
  };

  gint option_index = 0;
  gint c;
  gchar *decoder_list = NULL;

  while ((c = getopt_long (argc, argv, "i:o:e:d:h", long_options,
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
    g_printerr ("Error: Input and output files must be specified\n\n");
    print_usage (argv[0]);
    return -1;
  }

  g_print ("Configuration:\n");
  g_print ("  Input file: %s\n", params.input_file);
  g_print ("  Output file: %s\n", params.output_file);
  g_print ("  Video encoder: %s\n", params.video_encoder);
  g_print ("  Allowed decoders:");
  for (gint i = 0; params.allowed_video_decoders[i] != NULL; i++) {
    g_print (" %s", params.allowed_video_decoders[i]);
  }
  g_print ("\n\n");

  set_factory_ranks (&params);

  GstElement *pipeline, *filesrc, *decodebin, *mp4mux, *filesink;

  filesrc = gst_element_factory_make ("filesrc", "filesrc");
  decodebin = gst_element_factory_make ("decodebin", "decodebin");
  mp4mux = gst_element_factory_make ("mp4mux", "mp4mux");
  filesink = gst_element_factory_make ("filesink", "filesink");

  if (!filesrc || !decodebin || !mp4mux || !filesink) {
    g_printerr ("Not all elements could be created.\n");
    return -1;
  }

  g_object_set (G_OBJECT (filesrc), "location", params.input_file, NULL);
  g_object_set (G_OBJECT (filesink), "location", params.output_file, NULL);

  pipeline = gst_pipeline_new ("transcoding-pipeline");

  gst_bin_add_many (GST_BIN (pipeline), filesrc, decodebin, mp4mux, filesink,
      NULL);

  if (!gst_element_link (filesrc, decodebin)) {
    g_printerr ("Elements could not be linked.\n");
    gst_object_unref (pipeline);
    return -1;
  }

  if (!gst_element_link (mp4mux, filesink)) {
    g_printerr ("Elements could not be linked.\n");
    gst_object_unref (pipeline);
    return -1;
  }

  PadAddedData *pad_data = g_new0 (PadAddedData, 1);
  pad_data->pipeline = pipeline;
  pad_data->mux = mp4mux;
  pad_data->video_encoder = params.video_encoder;

  g_signal_connect (decodebin, "pad-added", G_CALLBACK (on_pad_added),
      pad_data);
  g_signal_connect (decodebin, "unknown-type", G_CALLBACK (on_unknown_type),
      NULL);
  g_signal_connect (decodebin, "drained", G_CALLBACK (on_drained), NULL);

  gint ret = run_main_loop (pipeline);

  gst_object_unref (pipeline);
  g_free (pad_data);

  if (params.allowed_video_decoders != (const gchar **) default_decoders) {
    free_decoder_list ((gchar **) params.allowed_video_decoders);
  }

  return ret;
}
