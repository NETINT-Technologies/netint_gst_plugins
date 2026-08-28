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
#include <glib.h>
#include <string.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

/* Global variables for configuration */
typedef struct {
    char *input_file;
    char *output_file;
    int initial_width;
    int initial_height;
    int change_width;
    int change_height;
    int change_frame;
    char *input_codec;
    char *output_codec;
} Config;

/* Function to parse resolution string (format: aaaxbbb) */
static gboolean parse_resolution(const char *resolution, int *width, int *height) {
    char *x_pos = strchr(resolution, 'x');
    if (!x_pos) {
        return FALSE;
    }

    char *endptr;
    *width = strtol(resolution, &endptr, 10);
    if (endptr != x_pos) {
        return FALSE;
    }

    *height = strtol(x_pos + 1, &endptr, 10);
    if (*endptr != '\0') {
        return FALSE;
    }

    return (*width > 0 && *height > 0);
}

/* Function to detect codec from file extension */
static const char* detect_codec_from_extension(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) {
        return NULL;
    }

    if (strcasecmp(ext, ".h264") == 0 || strcasecmp(ext, ".264") == 0) {
        return "h264";
    } else if (strcasecmp(ext, ".h265") == 0 || strcasecmp(ext, ".265") == 0) {
        return "h265";
    }

    return NULL;
}

/* Function to validate file extension */
static gboolean validate_file_extension(const char *filename) {
    const char *codec = detect_codec_from_extension(filename);
    return (codec != NULL);
}

/* Function to change caps */
static gboolean change_caps(GstElement *capsfilter, int width, int height) {
    GstCaps *new_caps;

    g_print("Attempting to change caps to %dx%d...\n", width, height);

    /* Create new caps with different width and height */
    new_caps = gst_caps_new_simple("video/x-raw",
                                   "width", G_TYPE_INT, width,
                                   "height", G_TYPE_INT, height,
                                   NULL);

    if (!new_caps) {
        g_printerr("Failed to create new caps.\n");
        return FALSE;
    }

    g_object_set(capsfilter, "caps", new_caps, NULL);
    gst_caps_unref(new_caps);

    g_print("Caps successfully changed to width=%d, height=%d\n", width, height);
    return FALSE; // Return FALSE to stop the timer
}

/* Bus message handler function */
static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    GMainLoop *loop = (GMainLoop *)data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            g_print("End-Of-Stream reached.\n");
            g_main_loop_quit(loop);
            break;
        case GST_MESSAGE_ERROR: {
            gchar *debug;
            GError *error;

            gst_message_parse_error(msg, &error, &debug);
            g_printerr("Error: %s\n", error->message);
            g_free(debug);
            g_error_free(error);

            g_main_loop_quit(loop);
            break;
        }
        default:
            break;
    }

    return TRUE;
}

/* Frame count probe callback function */
static GstPadProbeReturn frame_count_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    static guint frame_count = 0;
    Config *config = (Config *)user_data;

    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        frame_count++;
        if (frame_count == config->change_frame) {
            g_print("%d frame reached, changing caps...\n", config->change_frame);
            // We need to find the capsfilter element from the pipeline
            // For simplicity, we'll use a global variable for capsfilter
            extern GstElement *g_capsfilter;
            change_caps(g_capsfilter, config->change_width, config->change_height);
        }
    }

    return GST_PAD_PROBE_OK;
}

/* Global capsfilter element for probe callback */
GstElement *g_capsfilter = NULL;

/* Print usage information */
static void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("Options:\n");
    printf("  -i, --input FILE          Input file (H264/H265 format)\n");
    printf("  -o, --output FILE         Output file (H264/H265 format)\n");
    printf("  -r, --initial-res WxH     Initial resolution (e.g., 1280x720)\n");
    printf("  -c, --change-res WxH      Changed resolution (e.g., 560x480)\n");
    printf("  -f, --frame-num NUM       Frame number to change resolution\n");
    printf("  -h, --help                Show this help message\n");
    printf("\nSupported file extensions: .h264, .H264, .264, .h265, .H265, .265\n");
    printf("Resolution format: WIDTHxHEIGHT (e.g., 1920x1080)\n");
}

/* Parse command line arguments */
static gboolean parse_arguments(int argc, char *argv[], Config *config) {
    static struct option long_options[] = {
        {"input", required_argument, 0, 'i'},
        {"output", required_argument, 0, 'o'},
        {"initial-res", required_argument, 0, 'r'},
        {"change-res", required_argument, 0, 'c'},
        {"frame-num", required_argument, 0, 'f'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    int c;

    memset(config, 0, sizeof(Config));

    while ((c = getopt_long(argc, argv, "i:o:r:c:f:h", long_options, &option_index)) != -1) {
        switch (c) {
            case 'i':
                config->input_file = g_strdup(optarg);
                break;
            case 'o':
                config->output_file = g_strdup(optarg);
                break;
            case 'r':
                if (!parse_resolution(optarg, &config->initial_width, &config->initial_height)) {
                    g_printerr("Error: Invalid initial resolution format. Use WIDTHxHEIGHT (e.g., 1280x720)\n");
                    return FALSE;
                }
                break;
            case 'c':
                if (!parse_resolution(optarg, &config->change_width, &config->change_height)) {
                    g_printerr("Error: Invalid change resolution format. Use WIDTHxHEIGHT (e.g., 560x480)\n");
                    return FALSE;
                }
                break;
            case 'f':
                config->change_frame = atoi(optarg);
                if (config->change_frame <= 0) {
                    g_printerr("Error: Frame number must be a positive integer\n");
                    return FALSE;
                }
                break;
            case 'h':
                print_usage(argv[0]);
                return FALSE;
            case '?':
                print_usage(argv[0]);
                return FALSE;
            default:
                return FALSE;
        }
    }

    /* Check if all required arguments are provided */
    if (!config->input_file || !config->output_file ||
        config->initial_width == 0 || config->initial_height == 0 ||
        config->change_width == 0 || config->change_height == 0 ||
        config->change_frame == 0) {
        g_printerr("Error: All parameters are required\n");
        print_usage(argv[0]);
        return FALSE;
    }

    /* Validate file extensions */
    if (!validate_file_extension(config->input_file)) {
        g_printerr("Error: Input file must have a valid H264/H265 extension\n");
        return FALSE;
    }

    if (!validate_file_extension(config->output_file)) {
        g_printerr("Error: Output file must have a valid H264/H265 extension\n");
        return FALSE;
    }

    /* Detect codecs from file extensions */
    config->input_codec = g_strdup(detect_codec_from_extension(config->input_file));
    config->output_codec = g_strdup(detect_codec_from_extension(config->output_file));

    return TRUE;
}

int main(int argc, char *argv[]) {
    GstElement *pipeline, *source, *parser, *decoder, *scale, *capsfilter, *encoder, *parser2, *sink;
    GstBus *bus;
    GMainLoop *main_loop;
    Config config;

    /* Initialize GStreamer */
    gst_init(&argc, &argv);
    g_print("GStreamer initialized.\n");

    /* Parse command line arguments */
    if (!parse_arguments(argc, argv, &config)) {
        return -1;
    }

    g_print("Configuration:\n");
    g_print("  Input file: %s (%s)\n", config.input_file, config.input_codec);
    g_print("  Output file: %s (%s)\n", config.output_file, config.output_codec);
    g_print("  Initial resolution: %dx%d\n", config.initial_width, config.initial_height);
    g_print("  Change resolution: %dx%d at frame %d\n",
            config.change_width, config.change_height, config.change_frame);

    /* Create the elements */
    g_print("Creating GStreamer elements...\n");
    source = gst_element_factory_make("filesrc", "source");

    /* Create parser based on input codec */
    if (strcmp(config.input_codec, "h264") == 0) {
        parser = gst_element_factory_make("h264parse", "parser");
        decoder = gst_element_factory_make("niquadrah264dec", "decoder");
    } else { // h265
        parser = gst_element_factory_make("h265parse", "parser");
        decoder = gst_element_factory_make("niquadrah265dec", "decoder");
    }

    scale = gst_element_factory_make("niquadrascale", "scale");
    capsfilter = gst_element_factory_make("capsfilter", "capsfilter");
    g_capsfilter = capsfilter; // Set global reference for probe callback

    /* Create encoder based on output codec */
    if (strcmp(config.output_codec, "h264") == 0) {
        encoder = gst_element_factory_make("niquadrah264enc", "encoder");
        parser2 = gst_element_factory_make("h264parse", "parser2");
    } else { // h265
        encoder = gst_element_factory_make("niquadrah265enc", "encoder");
        parser2 = gst_element_factory_make("h265parse", "parser2");
    }

    sink = gst_element_factory_make("filesink", "sink");

    if (!source || !parser || !decoder || !scale || !capsfilter || !encoder || !parser2 || !sink) {
        g_printerr("Failed to create one or more elements.\n");
        return -1;
    }
    g_print("All elements created successfully.\n");

    /* Create the empty pipeline */
    pipeline = gst_pipeline_new("video-pipeline");
    if (!pipeline) {
        g_printerr("Failed to create pipeline.\n");
        return -1;
    }
    g_print("Pipeline created.\n");

    /* Set the file location to the source element */
    g_object_set(source, "location", config.input_file, NULL);
    g_print("File source location set to '%s'.\n", config.input_file);

    g_object_set(decoder, "xcoder-params", "out=hw", NULL);
    g_object_set(encoder, "xcoder-params", "disableAdaptiveBuffers=1", NULL);

    /* Set the file location to the sink element */
    g_object_set(sink, "location", config.output_file, NULL);
    g_print("File sink location set to '%s'.\n", config.output_file);

    /* Create the initial caps */
    GstCaps *initial_caps = gst_caps_new_simple("video/x-raw",
                                                "width", G_TYPE_INT, config.initial_width,
                                                "height", G_TYPE_INT, config.initial_height,
                                                NULL);
    if (!initial_caps) {
        g_printerr("Failed to create initial caps.\n");
        gst_object_unref(pipeline);
        return -1;
    }
    g_object_set(capsfilter, "caps", initial_caps, NULL);
    gst_caps_unref(initial_caps);
    g_print("Initial caps set to width=%d, height=%d.\n", config.initial_width, config.initial_height);

    /* Build the pipeline */
    gst_bin_add_many(GST_BIN(pipeline), source, parser, decoder, scale, capsfilter, encoder, parser2, sink, NULL);
    if (gst_element_link_many(source, parser, decoder, scale, capsfilter, encoder, parser2, sink, NULL) != TRUE) {
        g_printerr("Elements could not be linked.\n");
        gst_object_unref(pipeline);
        return -1;
    }
    g_print("Pipeline elements linked successfully.\n");

    /* Add a pad probe to count frames */
    GstPad *src_pad = gst_element_get_static_pad(decoder, "src");
    gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER, frame_count_probe, &config, NULL);
    gst_object_unref(src_pad);

    /* Start playing */
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_printerr("Failed to set pipeline to PLAYING state.\n");
        gst_object_unref(pipeline);
        return -1;
    }
    g_print("Pipeline set to PLAYING state.\n");

    /* Create a GLib Main Loop and set it to run */
    main_loop = g_main_loop_new(NULL, FALSE);

    /* Add a bus watch to handle messages */
    bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_call, main_loop);

    /* Run the main loop */
    g_main_loop_run(main_loop);

    /* Free resources */
    g_main_loop_unref(main_loop);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_print("Pipeline resources freed and program exiting.\n");

    /* Cleanup config */
    g_free(config.input_file);
    g_free(config.output_file);
    g_free(config.input_codec);
    g_free(config.output_codec);

    return 0;
}
