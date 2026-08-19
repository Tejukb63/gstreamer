#include <gst/gst.h>
#include <iostream>
#include <string>

int main(int argc, char *argv[]) {
    // 1. Initialize GStreamer
    gst_init(&argc, &argv);

    // Hardcoded paths matching your NVR output
    std::string input_file  = "recordings/complete_record.ts";
    std::string output_file = "recordings/download.mp4";

    std::cout << "[Converter] Starting conversion...\n";
    std::cout << "Input:  " << input_file << "\n";
    std::cout << "Output: " << output_file << "\n";

    // 2. Build the Pipeline String
    // tsdemux opens the .ts file, and routes the raw video/audio into mp4mux
    std::string pipeline_str = 
        "filesrc location=" + input_file + " ! tsdemux name=demux "
        "mp4mux name=mux ! filesink location=" + output_file + " "
        "demux. ! queue ! h264parse ! mux. "
        "demux. ! queue ! aacparse ! mux.";

    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(pipeline_str.c_str(), &error);

    if (error) {
        std::cerr << "[Error] Failed to parse pipeline: " << error->message << "\n";
        g_clear_error(&error);
        return -1;
    }

    // 3. Start the     Conversion
    std::cout << "[Converter] Transmuxing in progress... Please wait.\n";
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // 4. Wait for End of Stream (EOS)
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));

    if (msg != nullptr) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = nullptr;
            gchar *debug_info = nullptr;
            gst_message_parse_error(msg, &err, &debug_info);
            std::cerr << "[Error] " << err->message << "\n";
            g_clear_error(&err);
            g_free(debug_info);
        } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
            std::cout << "[Success] MP4 Conversion Complete!\n";
        }
        gst_message_unref(msg);
    }

    // 5. Clean up Memory
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    return 0;
}



//g++ ../src/converter.cpp -o mp4_converter -std=c++17 $(pkg-config --cflags --libs gstreamer-1.0)
//./mp4_converter