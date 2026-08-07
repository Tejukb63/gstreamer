#pragma once

#include <gst/gst.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <filesystem>

class KlvPipeline {
public:
    KlvPipeline() : pipeline(nullptr), bus(nullptr), terminate_app(false) {}
    ~KlvPipeline() { forceCleanup(); }

    int start(int argc, char *argv[]) {
        gst_init(&argc, &argv);

        // Make sure the output directory exists
        std::filesystem::create_directories("hls");

        pipeline        = gst_pipeline_new("vms-pipeline");
        GstElement *src = gst_element_factory_make("rtspsrc",      "source");
        GstElement *dep = gst_element_factory_make("rtph264depay", "depay");
        GstElement *par = gst_element_factory_make("h264parse",    "parse");
        
        // FIX: Re-added the missing MPEG-TS Muxer
        GstElement *mux = gst_element_factory_make("mpegtsmux",    "mux"); 
        
        GstElement *que = gst_element_factory_make("queue",        "hls_queue");
        GstElement *hls = gst_element_factory_make("hlssink",      "hls_sink");

        if (!pipeline || !src || !dep || !par || !mux || !que || !hls) {
            std::cerr << "[GST] Missing elements. Ensure gstreamer1.0-plugins-bad is installed.\n";
            return -1;
        }

        g_object_set(src,
            "location",  "rtsp://192.168.0.32:8080/h264_ulaw.sdp",
            "protocols",  4,    // TCP only
            "latency",    300,
            nullptr);

        g_object_set(par, "config-interval", -1, nullptr);

        g_object_set(hls,
            "location",          "hls/segment_%05d.ts",
            "playlist-location", "hls/playlist.m3u8",
            "target-duration",   2,
            "max-files",         20,
            nullptr);

        // Add all elements to the bin, including 'mux'
        gst_bin_add_many(GST_BIN(pipeline), src, dep, par, mux, que, hls, nullptr);

        // 1. Link standard elements up to the parser
        if (!gst_element_link_many(dep, par, nullptr)) {
            std::cerr << "[GST] Failed to link depay -> parse.\n";
            return -1;
        }

        // 2. Link parse -> muxer (Requires a Request Pad)
        GstPad *par_src = gst_element_get_static_pad(par, "src");
        
        // FIX: mpegtsmux explicitly requires 'sink_%d', not 'sink_%u'
        GstPad *mux_sink = gst_element_request_pad_simple(mux, "sink_%d"); 
        
        if (!mux_sink) {
            std::cerr << "[GST] Failed to request 'sink_%d' pad from muxer.\n";
            gst_object_unref(par_src);
            return -1;
        }

        if (gst_pad_link(par_src, mux_sink) != GST_PAD_LINK_OK) {
            std::cerr << "[GST] Failed to link parse -> mux.\n";
            gst_object_unref(par_src);
            gst_object_unref(mux_sink);
            return -1;
        }
        gst_object_unref(par_src);
        gst_object_unref(mux_sink);

        // 3. Link muxer -> queue -> hlssink
        if (!gst_element_link_many(mux, que, hls, nullptr)) {
            std::cerr << "[GST] Failed to link mux -> hlssink.\n";
            return -1;
        }

        g_signal_connect(src, "pad-added", G_CALLBACK(padAddedCb), dep);

        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            std::cerr << "[GST] Failed to set pipeline to PLAYING.\n";
            return -1;
        }

        terminate_app = false;
        bus_thread = std::thread(&KlvPipeline::busThreadFunction, this);
        return 0;
    }

    void play()  { if (pipeline) gst_element_set_state(pipeline, GST_STATE_PLAYING); }
    void pause() { if (pipeline) gst_element_set_state(pipeline, GST_STATE_PAUSED);  }

private:
    GstElement        *pipeline;
    GstBus            *bus;
    std::thread        bus_thread;
    std::atomic<bool>  terminate_app;

    void busThreadFunction() {
        bus = gst_element_get_bus(pipeline);
        while (!terminate_app) {
            GstMessage *msg = gst_bus_timed_pop_filtered(
                bus, 500 * GST_MSECOND,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
            if (!msg) continue;

            switch (GST_MESSAGE_TYPE(msg)) {
                case GST_MESSAGE_ERROR: {
                    GError *err = nullptr;
                    gchar  *dbg = nullptr;
                    gst_message_parse_error(msg, &err, &dbg);
                    std::cerr << "[GST Error] " << err->message << "\n";
                    if (dbg) std::cerr << "[GST Debug] " << dbg << "\n";
                    g_clear_error(&err);
                    g_free(dbg);
                    terminate_app = true;
                    break;
                }
                case GST_MESSAGE_EOS:
                    std::cout << "[GST] End of stream.\n";
                    terminate_app = true;
                    break;
                default:
                    break;
            }
            gst_message_unref(msg);
        }
        gst_object_unref(bus);
        bus = nullptr;
    }

    void forceCleanup() {
        terminate_app = true;
        if (bus_thread.joinable()) bus_thread.join();
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
    }

    static void padAddedCb(GstElement * /*src*/, GstPad *new_pad, GstElement *depay) {
        GstCaps *caps = gst_pad_get_current_caps(new_pad);
        if (!caps)  caps = gst_pad_query_caps(new_pad, nullptr);
        if (!caps) return; 

        GstStructure *str   = gst_caps_get_structure(caps, 0);
        const gchar  *name  = gst_structure_get_name(str);
        const gchar  *media = gst_structure_get_string(str, "media");
        gst_caps_unref(caps);

        if (!g_str_has_prefix(name, "application/x-rtp") || g_strcmp0(media, "video") != 0) {
            return;
        }

        GstPad *sink = gst_element_get_static_pad(depay, "sink");
        if (!sink) return;

        if (!gst_pad_is_linked(sink)) {
            GstPadLinkReturn ret = gst_pad_link(new_pad, sink);
            if (ret == GST_PAD_LINK_OK)
                std::cout << "[GST] Video pad linked successfully.\n";
            else
                std::cerr << "[GST] Pad link failed: " << ret << "\n";
        }
        gst_object_unref(sink);
    }
};