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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

typedef struct _AppData {
    GMainLoop *loop;
    GstElement *pipeline;
    GstElement *decoder;
    gint frame_count;
    gboolean params_changed;
    gint change_frame;
    gchar *initial_params;
    gchar *changed_params;
} AppData;

typedef struct _Config {
    gchar *input_file;
    gchar *output_file1;
    gchar *output_file2;
    gchar *output_file3;
    gchar *initial_xcoder_params;
    gchar *changed_xcoder_params;
    gint change_frame_number;
} Config;

static void print_usage(const char *program_name)
{
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("Options:\n");
    printf("  -i, --input FILE              Input file (H264/H265/264/265)\n");
    printf("  -o, --output1 FILE            Output file 1 (H264/H265/264/265)\n");
    printf("  -p, --output2 FILE            Output file 2 (H264/H265/264/265)\n");
    printf("  -q, --output3 FILE            Output file 3 (H264/H265/264/265)\n");
    printf("  -x, --initial-params PARAMS   Initial xcoder-params\n");
    printf("  -y, --changed-params PARAMS   Changed xcoder-params\n");
    printf("  -f, --change-frame NUMBER     Frame number to change params\n");
    printf("  -h, --help                    Show this help message\n");
}

static gboolean validate_file_extension(const gchar *filename)
{
    if (!filename) return FALSE;

    gchar *lower_name = g_ascii_strdown(filename, -1);
    gboolean valid = g_str_has_suffix(lower_name, ".h264") ||
                     g_str_has_suffix(lower_name, ".h265") ||
                     g_str_has_suffix(lower_name, ".264") ||
                     g_str_has_suffix(lower_name, ".265");

    g_free(lower_name);
    return valid;
}

static gchar* get_parser_name(const gchar *filename)
{
    if (!filename) return NULL;

    gchar *lower_name = g_ascii_strdown(filename, -1);
    gchar *parser_name = NULL;

    if (g_str_has_suffix(lower_name, ".h264") || g_str_has_suffix(lower_name, ".264")) {
        parser_name = g_strdup("h264parse");
    } else if (g_str_has_suffix(lower_name, ".h265") || g_str_has_suffix(lower_name, ".265")) {
        parser_name = g_strdup("h265parse");
    }

    g_free(lower_name);
    return parser_name;
}

static gchar* get_decoder_name(const gchar *filename)
{
    if (!filename) return NULL;

    gchar *lower_name = g_ascii_strdown(filename, -1);
    gchar *decoder_name = NULL;

    if (g_str_has_suffix(lower_name, ".h264") || g_str_has_suffix(lower_name, ".264")) {
        decoder_name = g_strdup("niquadrah264dec");
    } else if (g_str_has_suffix(lower_name, ".h265") || g_str_has_suffix(lower_name, ".265")) {
        decoder_name = g_strdup("niquadrah265dec");
    }

    g_free(lower_name);
    return decoder_name;
}

static gchar* get_encoder_name(const gchar *filename)
{
    if (!filename) return NULL;

    gchar *lower_name = g_ascii_strdown(filename, -1);
    gchar *encoder_name = NULL;

    if (g_str_has_suffix(lower_name, ".h264") || g_str_has_suffix(lower_name, ".264")) {
        encoder_name = g_strdup("niquadrah264enc");
    } else if (g_str_has_suffix(lower_name, ".h265") || g_str_has_suffix(lower_name, ".265")) {
        encoder_name = g_strdup("niquadrah265enc");
    }

    g_free(lower_name);
    return encoder_name;
}

static gboolean parse_arguments(int argc, char *argv[], Config *config)
{
    static struct option long_options[] = {
        {"input", required_argument, 0, 'i'},
        {"output1", required_argument, 0, 'o'},
        {"output2", required_argument, 0, 'p'},
        {"output3", required_argument, 0, 'q'},
        {"initial-params", required_argument, 0, 'x'},
        {"changed-params", required_argument, 0, 'y'},
        {"change-frame", required_argument, 0, 'f'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;

    memset(config, 0, sizeof(Config));

    while ((opt = getopt_long(argc, argv, "i:o:p:q:x:y:f:h", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'i':
                config->input_file = g_strdup(optarg);
                break;
            case 'o':
                config->output_file1 = g_strdup(optarg);
                break;
            case 'p':
                config->output_file2 = g_strdup(optarg);
                break;
            case 'q':
                config->output_file3 = g_strdup(optarg);
                break;
            case 'x':
                config->initial_xcoder_params = g_strdup(optarg);
                break;
            case 'y':
                config->changed_xcoder_params = g_strdup(optarg);
                break;
            case 'f':
                config->change_frame_number = atoi(optarg);
                break;
            case 'h':
                print_usage(argv[0]);
                return FALSE;
            default:
                print_usage(argv[0]);
                return FALSE;
        }
    }

    // Validate required parameters
    if (!config->input_file || !config->output_file1 || !config->output_file2 ||
        !config->output_file3 || !config->initial_xcoder_params ||
        !config->changed_xcoder_params || config->change_frame_number <= 0) {
        printf("Error: All parameters are required.\n");
        print_usage(argv[0]);
        return FALSE;
    }

    // Validate file extensions
    if (!validate_file_extension(config->input_file) ||
        !validate_file_extension(config->output_file1) ||
        !validate_file_extension(config->output_file2) ||
        !validate_file_extension(config->output_file3)) {
        printf("Error: All files must have H264, H265, 264, or 265 extensions.\n");
        return FALSE;
    }

    return TRUE;
}

static gboolean
bus_call(GstBus *bus, GstMessage *msg, gpointer data)
{
    AppData *app = (AppData *)data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            g_print("End of stream\n");
            g_main_loop_quit(app->loop);
            break;
        case GST_MESSAGE_ERROR: {
            gchar *debug;
            GError *error;

            gst_message_parse_error(msg, &error, &debug);
            g_free(debug);

            g_printerr("Error: %s\n", error->message);
            g_error_free(error);

            g_main_loop_quit(app->loop);
            break;
        }
        default:
            break;
    }

    return TRUE;
}

static GstPadProbeReturn
decoder_sink_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    AppData *app = (AppData *)user_data;

    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        app->frame_count++;

        // Change xcoder-params at configured frame
        if (app->frame_count == app->change_frame && !app->params_changed) {
            g_print("Changing xcoder-params at frame %d\n", app->frame_count);

            // Set new xcoder-params
            g_object_set(app->decoder,
                "xcoder-params", app->changed_params,
                NULL);

            app->params_changed = TRUE;
            g_print("xcoder-params changed successfully\n");
        }

        // Print progress every 100 frames
        if (app->frame_count % 100 == 0) {
            g_print("Processed %d frames\n", app->frame_count);
        }
    }

    return GST_PAD_PROBE_OK;
}

static gboolean
link_split_request_pads(GstElement *split, GstElement *queue1, GstElement *queue2, GstElement *queue3)
{
    GstPad *split_src1 = NULL, *split_src2 = NULL, *split_src3 = NULL;
    GstPad *queue1_sink = NULL, *queue2_sink = NULL, *queue3_sink = NULL;
    gboolean ret = TRUE;

    // Get request pads from split element
    split_src1 = gst_element_request_pad_simple(split, "src_0");
    split_src2 = gst_element_request_pad_simple(split, "src_1");
    split_src3 = gst_element_request_pad_simple(split, "src_2");

    // Get sink pads from queue elements
    queue1_sink = gst_element_get_static_pad(queue1, "sink");
    queue2_sink = gst_element_get_static_pad(queue2, "sink");
    queue3_sink = gst_element_get_static_pad(queue3, "sink");

    if (!split_src1 || !split_src2 || !split_src3 ||
        !queue1_sink || !queue2_sink || !queue3_sink) {
        g_printerr("Failed to get pads for linking split to queues\n");
        ret = FALSE;
        goto cleanup;
    }

    // Link split output pads to queue input pads
    if (gst_pad_link(split_src1, queue1_sink) != GST_PAD_LINK_OK ||
        gst_pad_link(split_src2, queue2_sink) != GST_PAD_LINK_OK ||
        gst_pad_link(split_src3, queue3_sink) != GST_PAD_LINK_OK) {
        g_printerr("Failed to link split pads to queue pads\n");
        ret = FALSE;
    }

cleanup:
    if (split_src1) gst_object_unref(split_src1);
    if (split_src2) gst_object_unref(split_src2);
    if (split_src3) gst_object_unref(split_src3);
    if (queue1_sink) gst_object_unref(queue1_sink);
    if (queue2_sink) gst_object_unref(queue2_sink);
    if (queue3_sink) gst_object_unref(queue3_sink);

    return ret;
}

int main(int argc, char *argv[])
{
    Config config;
    AppData app_data = {0};
    GstBus *bus;
    guint bus_watch_id;

    GstElement *filesrc, *parser, *decoder, *split;
    GstElement *queue1, *queue2, *queue3;
    GstElement *encoder1, *encoder2, *encoder3;
    GstElement *filesink1, *filesink2, *filesink3;

    // Parse command line arguments
    if (!parse_arguments(argc, argv, &config)) {
        return -1;
    }

    // Initialize GStreamer
    gst_init(&argc, &argv);

    app_data.loop = g_main_loop_new(NULL, FALSE);
    app_data.frame_count = 0;
    app_data.params_changed = FALSE;
    app_data.change_frame = config.change_frame_number;
    app_data.initial_params = g_strdup(config.initial_xcoder_params);
    app_data.changed_params = g_strdup(config.changed_xcoder_params);

    // Create pipeline
    app_data.pipeline = gst_pipeline_new("transcoding-pipeline");

    if (!app_data.pipeline) {
        g_printerr("Failed to create pipeline\n");
        return -1;
    }

    // Get element names based on file extensions
    gchar *parser_name = get_parser_name(config.input_file);
    gchar *decoder_name = get_decoder_name(config.input_file);
    gchar *encoder1_name = get_encoder_name(config.output_file1);
    gchar *encoder2_name = get_encoder_name(config.output_file2);
    gchar *encoder3_name = get_encoder_name(config.output_file3);

    if (!parser_name || !decoder_name || !encoder1_name || !encoder2_name || !encoder3_name) {
        g_printerr("Failed to determine element names from file extensions\n");
        return -1;
    }

    // Create elements
    filesrc = gst_element_factory_make("filesrc", "file-source");
    parser = gst_element_factory_make(parser_name, "parser");
    decoder = gst_element_factory_make(decoder_name, "decoder");
    split = gst_element_factory_make("niquadrasplit", "split");

    queue1 = gst_element_factory_make("queue", "queue1");
    queue2 = gst_element_factory_make("queue", "queue2");
    queue3 = gst_element_factory_make("queue", "queue3");

    encoder1 = gst_element_factory_make(encoder1_name, "encoder1");
    encoder2 = gst_element_factory_make(encoder2_name, "encoder2");
    encoder3 = gst_element_factory_make(encoder3_name, "encoder3");

    filesink1 = gst_element_factory_make("filesink", "file-sink1");
    filesink2 = gst_element_factory_make("filesink", "file-sink2");
    filesink3 = gst_element_factory_make("filesink", "file-sink3");

    if (!filesrc || !parser || !decoder || !split ||
        !queue1 || !queue2 || !queue3 ||
        !encoder1 || !encoder2 || !encoder3 ||
        !filesink1 || !filesink2 || !filesink3) {
        g_printerr("One element could not be created. Exiting.\n");
        return -1;
    }

    // Save decoder reference for later parameter modification
    app_data.decoder = decoder;

    // Set element properties
    g_object_set(G_OBJECT(filesrc), "location", config.input_file, NULL);
    g_object_set(G_OBJECT(decoder),
        "dec", 0,
        "xcoder-params", config.initial_xcoder_params,
        NULL);
    g_object_set(G_OBJECT(split), "output", "1:1:1", NULL);

    // Set queue properties
    g_object_set(G_OBJECT(queue1), "max-size-buffers", 1, NULL);
    g_object_set(G_OBJECT(queue2), "max-size-buffers", 1, NULL);
    g_object_set(G_OBJECT(queue3), "max-size-buffers", 1, NULL);

    // Set encoder properties
    g_object_set(G_OBJECT(encoder1), "xcoder-params", "disableAdaptiveBuffers=1", NULL);
    g_object_set(G_OBJECT(encoder2), "xcoder-params", "disableAdaptiveBuffers=1", NULL);
    g_object_set(G_OBJECT(encoder3), "xcoder-params", "disableAdaptiveBuffers=1", NULL);

    // Set output files
    g_object_set(G_OBJECT(filesink1), "location", config.output_file1, "async", FALSE, NULL);
    g_object_set(G_OBJECT(filesink2), "location", config.output_file2, "async", FALSE, NULL);
    g_object_set(G_OBJECT(filesink3), "location", config.output_file3, "async", FALSE, NULL);

    // Set message handling
    bus = gst_pipeline_get_bus(GST_PIPELINE(app_data.pipeline));
    bus_watch_id = gst_bus_add_watch(bus, bus_call, &app_data);
    gst_object_unref(bus);

    // Add all elements to pipeline
    gst_bin_add_many(GST_BIN(app_data.pipeline),
        filesrc, parser, decoder, split,
        queue1, queue2, queue3,
        encoder1, encoder2, encoder3,
        filesink1, filesink2, filesink3, NULL);

    // Link first half of pipeline
    if (!gst_element_link_many(filesrc, parser, decoder, split, NULL)) {
        g_printerr("Failed to link source elements.\n");
        return -1;
    }

    // Handle split request pads and link to queues
    if (!link_split_request_pads(split, queue1, queue2, queue3)) {
        g_printerr("Failed to link split request pads\n");
        return -1;
    }

    // Link queues to encoders and outputs
    if (!gst_element_link_many(queue1, encoder1, filesink1, NULL) ||
        !gst_element_link_many(queue2, encoder2, filesink2, NULL) ||
        !gst_element_link_many(queue3, encoder3, filesink3, NULL)) {
        g_printerr("Failed to link encoder elements.\n");
        return -1;
    }

    // Add probe to count frames
    GstPad *decoder_sink_pad = gst_element_get_static_pad(decoder, "sink");
    if (decoder_sink_pad) {
        gst_pad_add_probe(decoder_sink_pad,
            GST_PAD_PROBE_TYPE_BUFFER,
            decoder_sink_probe,
            &app_data, NULL);
        gst_object_unref(decoder_sink_pad);
    }

    // Start pipeline
    g_print("Starting pipeline...\n");
    g_print("Input: %s\n", config.input_file);
    g_print("Output1: %s\n", config.output_file1);
    g_print("Output2: %s\n", config.output_file2);
    g_print("Output3: %s\n", config.output_file3);
    g_print("Change frame: %d\n", config.change_frame_number);

    gst_element_set_state(app_data.pipeline, GST_STATE_PLAYING);

    // Run main loop
    g_print("Running...\n");
    g_main_loop_run(app_data.loop);

    // Cleanup
    g_print("Stopping pipeline...\n");
    gst_element_set_state(app_data.pipeline, GST_STATE_NULL);

    g_print("Deleting pipeline...\n");
    gst_object_unref(app_data.pipeline);
    g_source_remove(bus_watch_id);
    g_main_loop_unref(app_data.loop);

    // Free config strings
    g_free(config.input_file);
    g_free(config.output_file1);
    g_free(config.output_file2);
    g_free(config.output_file3);
    g_free(config.initial_xcoder_params);
    g_free(config.changed_xcoder_params);

    // Free app data strings
    g_free(app_data.initial_params);
    g_free(app_data.changed_params);

    // Free element names
    g_free(parser_name);
    g_free(decoder_name);
    g_free(encoder1_name);
    g_free(encoder2_name);
    g_free(encoder3_name);

    return 0;
}
