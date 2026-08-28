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
#include <getopt.h>
#include <string.h>
#include <stdlib.h>

// Mode definitions
#define MODE_ADD_INPUT     1
#define MODE_REMOVE_INPUT  2
#define MODE_CHANGE_LAYOUT 3

typedef struct _CustomData {
    GstElement *pipeline;
    GstElement *filesrc1, *filesrc2, *filesrc3;
    GstElement *parser1, *parser2, *parser3;
    GstElement *decoder1, *decoder2, *decoder3;
    GstElement *stack;
    GstElement *encoder;
    GstElement *filesink;
    GstBus *bus;
    GMainLoop *loop;
    gint frame_count;
    gboolean operation_done;

    // Configuration parameters
    gint mode;
    gint device_index;
    gint trigger_frame;
    gchar *input1, *input2, *input3;
    gchar *output;
    gchar *initial_layout, *new_layout;
    gchar *initial_size, *new_size;
    gint initial_inputs, new_inputs;
} CustomData;

// Function to determine codec type from file extension
static gchar* get_codec_type(const gchar *filename) {
    if (!filename) return NULL;

    gchar *lower_name = g_ascii_strdown(filename, -1);

    if (g_str_has_suffix(lower_name, ".h264") ||
        g_str_has_suffix(lower_name, ".264")) {
        g_free(lower_name);
        return g_strdup("h264");
    } else if (g_str_has_suffix(lower_name, ".h265") ||
               g_str_has_suffix(lower_name, ".265") ||
               g_str_has_suffix(lower_name, ".hevc")) {
        g_free(lower_name);
        return g_strdup("h265");
    }

    g_free(lower_name);
    return NULL;
}

// Function to create parser element based on codec
static GstElement* create_parser(const gchar *codec, const gchar *name) {
    if (g_strcmp0(codec, "h264") == 0) {
        return gst_element_factory_make("h264parse", name);
    } else if (g_strcmp0(codec, "h265") == 0) {
        return gst_element_factory_make("h265parse", name);
    }
    return NULL;
}

// Function to create decoder element based on codec
static GstElement* create_decoder(const gchar *codec, const gchar *name) {
    if (g_strcmp0(codec, "h264") == 0) {
        return gst_element_factory_make("niquadrah264dec", name);
    } else if (g_strcmp0(codec, "h265") == 0) {
        return gst_element_factory_make("niquadrah265dec", name);
    }
    return NULL;
}

// Function to create encoder element based on codec
static GstElement* create_encoder(const gchar *codec) {
    if (g_strcmp0(codec, "h264") == 0) {
        return gst_element_factory_make("niquadrah264enc", "encoder");
    } else if (g_strcmp0(codec, "h265") == 0) {
        return gst_element_factory_make("niquadrah265enc", "encoder");
    }
    return NULL;
}

// Function to add third input stream (MODE_ADD_INPUT)
static gboolean add_third_stream(CustomData *data) {
    GstPad *decoder3_src_pad, *stack_sink_pad3;
    gchar *codec3;

    g_print("Adding third input stream...\n");

    codec3 = get_codec_type(data->input3);
    if (!codec3) {
        g_printerr("Invalid codec for third input file\n");
        return FALSE;
    }

    // Create third input elements
    data->filesrc3 = gst_element_factory_make("filesrc", "source3");
    data->parser3 = create_parser(codec3, "parser3");
    data->decoder3 = create_decoder(codec3, "decoder3");

    if (!data->filesrc3 || !data->parser3 || !data->decoder3) {
        g_printerr("Could not create third stream elements\n");
        g_free(codec3);
        return FALSE;
    }

    // Set properties
    g_object_set(G_OBJECT(data->filesrc3), "location", data->input3, NULL);
    g_object_set(G_OBJECT(data->decoder3), "dec", data->device_index, "xcoder-params", "out=hw", NULL);

    // Add elements to pipeline
    gst_bin_add_many(GST_BIN(data->pipeline),
                     data->filesrc3, data->parser3, data->decoder3, NULL);

    // Link third path
    if (gst_element_link_many(data->filesrc3, data->parser3, data->decoder3, NULL) != TRUE) {
        g_printerr("Could not link third stream elements\n");
        g_free(codec3);
        return FALSE;
    }

    // Update stack parameters
    if (data->new_inputs > 0) {
        g_object_set(G_OBJECT(data->stack), "inputs", data->new_inputs, NULL);
    }
    if (data->new_layout) {
        g_object_set(G_OBJECT(data->stack), "layout", data->new_layout, NULL);
    }
    if (data->new_size) {
        g_object_set(G_OBJECT(data->stack), "size", data->new_size, NULL);
    }

    // Get sink pad and link
    stack_sink_pad3 = gst_element_request_pad_simple(data->stack, "sink_2");
    decoder3_src_pad = gst_element_get_static_pad(data->decoder3, "src");

    if (gst_pad_link(decoder3_src_pad, stack_sink_pad3) != GST_PAD_LINK_OK) {
        g_printerr("Could not link decoder3 to stack\n");
        g_object_unref(decoder3_src_pad);
        g_object_unref(stack_sink_pad3);
        g_free(codec3);
        return FALSE;
    }

    // Sync state
    gst_element_sync_state_with_parent(data->filesrc3);
    gst_element_sync_state_with_parent(data->parser3);
    gst_element_sync_state_with_parent(data->decoder3);

    // Clean up
    gst_object_unref(decoder3_src_pad);
    gst_object_unref(stack_sink_pad3);
    g_free(codec3);

    g_print("Successfully added third input stream\n");
    return TRUE;
}

// Function to remove third input stream (MODE_REMOVE_INPUT)
static gboolean remove_third_stream(CustomData *data) {
    GstPad *decoder3_src_pad, *stack_sink_pad3;

    g_print("Removing third input stream...\n");

    // Get pads
    decoder3_src_pad = gst_element_get_static_pad(data->decoder3, "src");
    stack_sink_pad3 = gst_pad_get_peer(decoder3_src_pad);

    if (decoder3_src_pad) {
      gst_pad_add_probe(decoder3_src_pad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
          NULL, NULL, NULL);
    }

    // set state to GST_STATE_PAUSED
    gst_element_set_state(data->filesrc3, GST_STATE_PAUSED);
    gst_element_set_state(data->parser3, GST_STATE_PAUSED);
    gst_element_set_state(data->decoder3, GST_STATE_PAUSED);

    gst_element_get_state(data->filesrc3, NULL, NULL, GST_CLOCK_TIME_NONE);
    gst_element_get_state(data->parser3, NULL, NULL, GST_CLOCK_TIME_NONE);
    gst_element_get_state(data->decoder3, NULL, NULL, GST_CLOCK_TIME_NONE);

    if (decoder3_src_pad && stack_sink_pad3) {
        // Release pad
        gst_element_release_request_pad(data->stack, stack_sink_pad3);
        gst_object_unref(stack_sink_pad3);
        gst_object_unref(decoder3_src_pad);
    }

    // Set state to NULL and remove
    gst_element_set_state(data->decoder3, GST_STATE_NULL);
    gst_element_set_state(data->parser3, GST_STATE_NULL);
    gst_element_set_state(data->filesrc3, GST_STATE_NULL);

    gst_bin_remove_many(GST_BIN(data->pipeline),
                        data->filesrc3, data->parser3, data->decoder3, NULL);

    // Update stack parameters
    g_object_set(G_OBJECT(data->stack), "inputs", data->new_inputs,
        "layout", data->new_layout, "size", data->new_size, NULL);

    g_print("Successfully removed third input stream\n");
    return TRUE;
}

// Function to change layout only (MODE_CHANGE_LAYOUT)
static gboolean change_layout(CustomData *data) {
    g_print("Changing layout...\n");

    if (data->new_layout) {
        g_object_set(G_OBJECT(data->stack), "layout", data->new_layout, NULL);
        g_print("Layout changed to: %s\n", data->new_layout);
    }

    if (data->new_size) {
        g_object_set(G_OBJECT(data->stack), "size", data->new_size, NULL);
        g_print("Size changed to: %s\n", data->new_size);
    }

    return TRUE;
}

// Probe callback
static GstPadProbeReturn stack_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    CustomData *data = (CustomData*)user_data;

    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        data->frame_count++;

        if (data->frame_count == data->trigger_frame && !data->operation_done) {
            g_print("Frame %d reached, executing operation for mode %d...\n",
                   data->frame_count, data->mode);

            gboolean success = FALSE;
            switch (data->mode) {
                case MODE_ADD_INPUT:
                    success = add_third_stream(data);
                    break;
                case MODE_REMOVE_INPUT:
                    success = remove_third_stream(data);
                    break;
                case MODE_CHANGE_LAYOUT:
                    success = change_layout(data);
                    break;
                default:
                    g_printerr("Unknown mode: %d\n", data->mode);
                    break;
            }

            if (success) {
                data->operation_done = TRUE;
            } else {
                g_printerr("Operation failed for mode %d\n", data->mode);
            }
        }

        if (data->frame_count % 50 == 0) {
            g_print("Processed %d frames\n", data->frame_count);
        }
    }

    return GST_PAD_PROBE_OK;
}

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    CustomData *user_data = (CustomData*)data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            g_print("End of stream\n");
            g_main_loop_quit(user_data->loop);
            break;
        case GST_MESSAGE_ERROR: {
            gchar *debug;
            GError *error;

            gst_message_parse_error(msg, &error, &debug);
            g_free(debug);

            g_printerr("Error: %s\n", error->message);
            g_error_free(error);

            g_main_loop_quit(user_data->loop);
            break;
        }
        default:
            break;
    }

    return TRUE;
}

static gboolean create_pipeline(CustomData *data) {
    GstPad *stack_sink_pad1, *stack_sink_pad2, *stack_sink_pad3 = NULL;
    GstPad *decoder1_src_pad, *decoder2_src_pad, *decoder3_src_pad = NULL;
    GstPad *stack_src_pad;
    gchar *codec1, *codec2, *codec3 = NULL, *output_codec;

    // Initialize
    data->frame_count = 0;
    data->operation_done = FALSE;

    // Get codec types
    codec1 = get_codec_type(data->input1);
    codec2 = get_codec_type(data->input2);
    output_codec = get_codec_type(data->output);

    if (!codec1 || !codec2 || !output_codec) {
        g_printerr("Invalid codec types for input/output files\n");
        return FALSE;
    }

    // Create pipeline
    data->pipeline = gst_pipeline_new("video-stack-pipeline");

    // Create first path elements
    data->filesrc1 = gst_element_factory_make("filesrc", "source1");
    data->parser1 = create_parser(codec1, "parser1");
    data->decoder1 = create_decoder(codec1, "decoder1");

    // Create second path elements
    data->filesrc2 = gst_element_factory_make("filesrc", "source2");
    data->parser2 = create_parser(codec2, "parser2");
    data->decoder2 = create_decoder(codec2, "decoder2");

    // Create stack and output elements
    data->stack = gst_element_factory_make("niquadrastack", "xstack");
    data->encoder = create_encoder(output_codec);
    data->filesink = gst_element_factory_make("filesink", "sink");

    if (!data->pipeline || !data->filesrc1 || !data->parser1 || !data->decoder1 ||
        !data->filesrc2 || !data->parser2 || !data->decoder2 ||
        !data->stack || !data->encoder || !data->filesink) {
        g_printerr("Not all elements could be created\n");
        return FALSE;
    }

    // For MODE_REMOVE_INPUT, create third input initially
    if (data->mode == MODE_REMOVE_INPUT) {
        codec3 = get_codec_type(data->input3);
        if (!codec3) {
            g_printerr("Invalid codec for third input\n");
            return FALSE;
        }

        data->filesrc3 = gst_element_factory_make("filesrc", "source3");
        data->parser3 = create_parser(codec3, "parser3");
        data->decoder3 = create_decoder(codec3, "decoder3");

        if (!data->filesrc3 || !data->parser3 || !data->decoder3) {
            g_printerr("Could not create third stream elements\n");
            g_free(codec3);
            return FALSE;
        }

        g_object_set(G_OBJECT(data->filesrc3), "location", data->input3, NULL);
        g_object_set(G_OBJECT(data->decoder3), "xcoder-params", "out=hw", NULL);
    }

    // Set properties
    g_object_set(G_OBJECT(data->filesrc1), "location", data->input1, NULL);
    g_object_set(G_OBJECT(data->filesrc2), "location", data->input2, NULL);

    g_object_set(G_OBJECT(data->decoder1), "dec", data->device_index, "xcoder-params", "out=hw", NULL);
    g_object_set(G_OBJECT(data->decoder2), "dec", data->device_index, "xcoder-params", "out=hw", NULL);
    if (data->mode == MODE_REMOVE_INPUT) {
      g_object_set(G_OBJECT(data->decoder3), "dec", data->device_index, "xcoder-params", "out=hw", NULL);
    }

    g_object_set(G_OBJECT(data->encoder), "xcoder-params", "disableAdaptiveBuffers=1", NULL);

    g_object_set(G_OBJECT(data->filesink), "location", data->output, NULL);

    // Set initial stack parameters
    if (data->initial_inputs > 0) {
        g_object_set(G_OBJECT(data->stack), "inputs", data->initial_inputs, NULL);
    }
    if (data->initial_layout) {
        g_object_set(G_OBJECT(data->stack), "layout", data->initial_layout, NULL);
    }
    if (data->initial_size) {
        g_object_set(G_OBJECT(data->stack), "size", data->initial_size, NULL);
    }

    // Add elements to pipeline
    if (data->mode == MODE_REMOVE_INPUT) {
        gst_bin_add_many(GST_BIN(data->pipeline),
                         data->filesrc1, data->parser1, data->decoder1,
                         data->filesrc2, data->parser2, data->decoder2,
                         data->filesrc3, data->parser3, data->decoder3,
                         data->stack, data->encoder, data->filesink, NULL);
    } else {
        gst_bin_add_many(GST_BIN(data->pipeline),
                         data->filesrc1, data->parser1, data->decoder1,
                         data->filesrc2, data->parser2, data->decoder2,
                         data->stack, data->encoder, data->filesink, NULL);
    }

    // Link elements
    if (gst_element_link_many(data->filesrc1, data->parser1, data->decoder1, NULL) != TRUE) {
        g_printerr("Elements could not be linked (path 1)\n");
        return FALSE;
    }

    if (gst_element_link_many(data->filesrc2, data->parser2, data->decoder2, NULL) != TRUE) {
        g_printerr("Elements could not be linked (path 2)\n");
        return FALSE;
    }

    if (data->mode == MODE_REMOVE_INPUT) {
        if (gst_element_link_many(data->filesrc3, data->parser3, data->decoder3, NULL) != TRUE) {
            g_printerr("Elements could not be linked (path 3)\n");
            return FALSE;
        }
    }

    if (gst_element_link_many(data->stack, data->encoder, data->filesink, NULL) != TRUE) {
        g_printerr("Elements could not be linked (output path)\n");
        return FALSE;
    }

    // Link to stack
    decoder1_src_pad = gst_element_get_static_pad(data->decoder1, "src");
    stack_sink_pad1 = gst_element_request_pad_simple(data->stack, "sink_0");
    if (gst_pad_link(decoder1_src_pad, stack_sink_pad1) != GST_PAD_LINK_OK) {
        g_printerr("Could not link decoder1 to stack\n");
        return FALSE;
    }

    decoder2_src_pad = gst_element_get_static_pad(data->decoder2, "src");
    stack_sink_pad2 = gst_element_request_pad_simple(data->stack, "sink_1");
    if (gst_pad_link(decoder2_src_pad, stack_sink_pad2) != GST_PAD_LINK_OK) {
        g_printerr("Could not link decoder2 to stack\n");
        return FALSE;
    }

    if (data->mode == MODE_REMOVE_INPUT) {
        decoder3_src_pad = gst_element_get_static_pad(data->decoder3, "src");
        stack_sink_pad3 = gst_element_request_pad_simple(data->stack, "sink_2");
        if (gst_pad_link(decoder3_src_pad, stack_sink_pad3) != GST_PAD_LINK_OK) {
            g_printerr("Could not link decoder3 to stack\n");
            return FALSE;
        }
    }

    // Add probe
    stack_src_pad = gst_element_get_static_pad(data->stack, "src");
    if (stack_src_pad) {
        gst_pad_add_probe(stack_src_pad, GST_PAD_PROBE_TYPE_BUFFER,
                         stack_probe_cb, data, NULL);
        gst_object_unref(stack_src_pad);
    }

    // Clean up pad references
    gst_object_unref(decoder1_src_pad);
    gst_object_unref(decoder2_src_pad);
    gst_object_unref(stack_sink_pad1);
    gst_object_unref(stack_sink_pad2);

    if (data->mode == MODE_REMOVE_INPUT) {
        gst_object_unref(decoder3_src_pad);
        gst_object_unref(stack_sink_pad3);
    }

    // Clean up codec strings
    g_free(codec1);
    g_free(codec2);
    g_free(output_codec);
    if (codec3) g_free(codec3);

    return TRUE;
}

static void cleanup(CustomData *data) {
    if (data->pipeline) {
        gst_element_set_state(data->pipeline, GST_STATE_NULL);
        gst_object_unref(data->pipeline);
    }

    if (data->bus) {
        gst_object_unref(data->bus);
    }

    if (data->loop) {
        g_main_loop_unref(data->loop);
    }

    g_free(data->input1);
    g_free(data->input2);
    g_free(data->input3);
    g_free(data->output);
    g_free(data->initial_layout);
    g_free(data->new_layout);
    g_free(data->initial_size);
    g_free(data->new_size);
}

// Enhanced parameter validation
static gboolean validate_parameters(CustomData *data) {
    // Basic validation
    if (data->mode < MODE_ADD_INPUT || data->mode > MODE_CHANGE_LAYOUT) {
        g_printerr("Invalid mode. Must be %d (add), %d (remove), or %d (change layout).\n",
                   MODE_ADD_INPUT, MODE_REMOVE_INPUT, MODE_CHANGE_LAYOUT);
        return FALSE;
    }

    if (!data->input1 || !data->input2 || !data->output) {
        g_printerr("Missing required input/output files.\n");
        return FALSE;
    }

    if (data->trigger_frame <= 0) {
        g_printerr("Invalid trigger frame number (must be > 0).\n");
        return FALSE;
    }

    // Validate codec types
    gchar *codec1 = get_codec_type(data->input1);
    gchar *codec2 = get_codec_type(data->input2);
    gchar *output_codec = get_codec_type(data->output);

    if (!codec1) {
        g_printerr("Unsupported codec format for input1: %s\n", data->input1);
        return FALSE;
    }
    if (!codec2) {
        g_printerr("Unsupported codec format for input2: %s\n", data->input2);
        g_free(codec1);
        return FALSE;
    }
    if (!output_codec) {
        g_printerr("Unsupported codec format for output: %s\n", data->output);
        g_free(codec1);
        g_free(codec2);
        return FALSE;
    }

    // Mode-specific validation
    switch (data->mode) {
        case MODE_ADD_INPUT:
            if (!data->input3) {
                g_printerr("Third input file required for add input mode.\n");
                g_free(codec1);
                g_free(codec2);
                g_free(output_codec);
                return FALSE;
            }

            gchar *codec3 = get_codec_type(data->input3);
            if (!codec3) {
                g_printerr("Unsupported codec format for input3: %s\n", data->input3);
                g_free(codec1);
                g_free(codec2);
                g_free(output_codec);
                return FALSE;
            }
            g_free(codec3);

            if (data->new_inputs <= 0 || data->new_inputs <= data->initial_inputs) {
                g_printerr("For add input mode, new_inputs must be greater than initial_inputs.\n");
                g_free(codec1);
                g_free(codec2);
                g_free(output_codec);
                return FALSE;
            }
            break;

        case MODE_REMOVE_INPUT:
            if (!data->input3) {
                g_printerr("Third input file required for remove input mode.\n");
                g_free(codec1);
                g_free(codec2);
                g_free(output_codec);
                return FALSE;
            }

            codec3 = get_codec_type(data->input3);
            if (!codec3) {
                g_printerr("Unsupported codec format for input3: %s\n", data->input3);
                g_free(codec1);
                g_free(codec2);
                g_free(output_codec);
                return FALSE;
            }
            g_free(codec3);

            if (data->new_inputs <= 0 || data->new_inputs >= data->initial_inputs) {
                g_printerr("For remove input mode, new_inputs must be less than initial_inputs.\n");
                g_free(codec1);
                g_free(codec2);
                g_free(output_codec);
                return FALSE;
            }
            break;

        case MODE_CHANGE_LAYOUT:
            if (!data->new_layout) {
                g_printerr("New layout required for change layout mode.\n");
                g_free(codec1);
                g_free(codec2);
                g_free(output_codec);
                return FALSE;
            }
            break;
    }

    // Check for required initial parameters
    if (data->initial_inputs <= 0) {
        g_printerr("Initial inputs count must be specified and > 0.\n");
        g_free(codec1);
        g_free(codec2);
        g_free(output_codec);
        return FALSE;
    }

    if (!data->initial_layout) {
        g_printerr("Initial layout must be specified.\n");
        g_free(codec1);
        g_free(codec2);
        g_free(output_codec);
        return FALSE;
    }

    g_free(codec1);
    g_free(codec2);
    g_free(output_codec);
    return TRUE;
}

static void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("Options:\n");
    printf("  -m, --mode MODE              Operation mode (%d=add input, %d=remove input, %d=change layout)\n",
           MODE_ADD_INPUT, MODE_REMOVE_INPUT, MODE_CHANGE_LAYOUT);
    printf("  -d, --device-index           Select which device to use by index\n");
    printf("  -f, --frame FRAME            Trigger frame number\n");
    printf("  -i, --input1 FILE            First input file (H264/H265)\n");
    printf("  -j, --input2 FILE            Second input file (H264/H265)\n");
    printf("  -k, --input3 FILE            Third input file (H264/H265, required for mode %d&%d)\n",
           MODE_ADD_INPUT, MODE_REMOVE_INPUT);
    printf("  -o, --output FILE            Output file (H264/H265)\n");
    printf("  -l, --initial-layout LAYOUT  Initial layout string\n");
    printf("  -L, --new-layout LAYOUT      New layout string\n");
    printf("  -s, --initial-size SIZE      Initial size string\n");
    printf("  -S, --new-size SIZE          New size string\n");
    printf("  -n, --initial-inputs NUM     Initial input count\n");
    printf("  -N, --new-inputs NUM         New input count\n");
    printf("  -h, --help                   Show this help message\n");
    printf("\nSupported file formats: .h264, .264, .h265, .265, .hevc (case insensitive)\n");
    printf("\nExamples:\n");
    printf("Mode %d (add input): %s -m %d -f 100 -i input1.h264 -j input2.h265 -k input3.h264 -o output.h265 -n 2 -N 3 -l \"0_0|0_h0\" -L \"0_0|0_h0|0_h0+h1\" -s \"320_240|320_240\" -S \"320_240|320_240|320_240\"\n",
           MODE_ADD_INPUT, program_name, MODE_ADD_INPUT);
    printf("Mode %d (remove input): %s -m %d -f 100 -i input1.h264 -j input2.h264 -k input3.h264 -o output.h264 -n 3 -N 2 -l \"0_0|0_h0|0_h0+h1\" -L \"0_0|0_h0\" -s \"320_240|320_240|320_240\" -S \"320_240|320_240\"\n",
           MODE_REMOVE_INPUT, program_name, MODE_REMOVE_INPUT);
    printf("Mode %d (change layout): %s -m %d -f 100 -i input1.h264 -j input2.h264 -o output.h264 -n 2 -l \"0_0|0_h0\" -L \"0_h0|0_0\" -s \"320_240|320_240\"\n",
           MODE_CHANGE_LAYOUT, program_name, MODE_CHANGE_LAYOUT);
}

int main(int argc, char *argv[]) {
    CustomData data = {0};
    GstStateChangeReturn ret;
    guint bus_watch_id;

    static struct option long_options[] = {
        {"mode", required_argument, 0, 'm'},
        {"device-index", required_argument, 0, 'd'},
        {"frame", required_argument, 0, 'f'},
        {"input1", required_argument, 0, 'i'},
        {"input2", required_argument, 0, 'j'},
        {"input3", required_argument, 0, 'k'},
        {"output", required_argument, 0, 'o'},
        {"initial-layout", required_argument, 0, 'l'},
        {"new-layout", required_argument, 0, 'L'},
        {"initial-size", required_argument, 0, 's'},
        {"new-size", required_argument, 0, 'S'},
        {"initial-inputs", required_argument, 0, 'n'},
        {"new-inputs", required_argument, 0, 'N'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    int c;

    // Parse command line arguments
    while ((c = getopt_long(argc, argv, "m:d:f:i:j:k:o:l:L:s:S:n:N:h", long_options, &option_index)) != -1) {
        switch (c) {
            case 'm':
                data.mode = atoi(optarg);
                break;
            case 'd':
                data.device_index = atoi(optarg);
                break;
            case 'f':
                data.trigger_frame = atoi(optarg);
                break;
            case 'i':
                data.input1 = g_strdup(optarg);
                break;
            case 'j':
                data.input2 = g_strdup(optarg);
                break;
            case 'k':
                data.input3 = g_strdup(optarg);
                break;
            case 'o':
                data.output = g_strdup(optarg);
                break;
            case 'l':
                data.initial_layout = g_strdup(optarg);
                break;
            case 'L':
                data.new_layout = g_strdup(optarg);
                break;
            case 's':
                data.initial_size = g_strdup(optarg);
                break;
            case 'S':
                data.new_size = g_strdup(optarg);
                break;
            case 'n':
                data.initial_inputs = atoi(optarg);
                break;
            case 'N':
                data.new_inputs = atoi(optarg);
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            case '?':
                print_usage(argv[0]);
                return -1;
        }
    }

    // Validate parameters
    if (!validate_parameters(&data)) {
        print_usage(argv[0]);
        cleanup(&data);
        return -1;
    }

    // Initialize GStreamer
    gst_init(&argc, &argv);

    // Create main loop
    data.loop = g_main_loop_new(NULL, FALSE);

    // Create pipeline
    if (!create_pipeline(&data)) {
        g_printerr("Failed to create pipeline.\n");
        cleanup(&data);
        return -1;
    }

    // Set message monitoring
    data.bus = gst_pipeline_get_bus(GST_PIPELINE(data.pipeline));
    bus_watch_id = gst_bus_add_watch(data.bus, bus_call, &data);

    // Start pipeline
    const char* mode_names[] = {"", "add input", "remove input", "change layout"};
    g_print("Starting pipeline in mode %d (%s)...\n", data.mode, mode_names[data.mode]);
    g_print("Will trigger operation at frame %d\n", data.trigger_frame);

    ret = gst_element_set_state(data.pipeline, GST_STATE_PLAYING);

    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("Unable to set the pipeline to the playing state.\n");
        cleanup(&data);
        return -1;
    }

    // Run main loop
    g_print("Running...\n");
    g_main_loop_run(data.loop);

    // Clean up resources
    g_print("Cleaning up...\n");
    g_source_remove(bus_watch_id);
    cleanup(&data);

    return 0;
}
