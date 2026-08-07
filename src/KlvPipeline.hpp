#pragma once

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <cstdint>
#include <filesystem>

// ---------------------------------------------------------------------------
// 16-byte private Universal Label for NMEA sentences (SMPTE format)
// ---------------------------------------------------------------------------
static const uint8_t NMEA_KLV_KEY[16] = {
    0x06, 0x0E, 0x2B, 0x34,
    0x02, 0x0B, 0x01, 0x01,
    0x0E, 0x01, 0x03, 0x01,
    0x4E, 0x4D, 0x45, 0x41   // 'N','M','E','A'
};

class KlvPipeline {
public:
    KlvPipeline() 
        : pipeline(nullptr), src(nullptr), dep(nullptr), par(nullptr),
          audio_depay(nullptr), audio_dec(nullptr), audio_conv(nullptr),
          audio_resamp(nullptr), audio_enc(nullptr),
          klv_src(nullptr), mux(nullptr), que(nullptr), hls(nullptr),
          bus(nullptr), terminate_app(false), video_linked(false) {}

    ~KlvPipeline() { 
        forceCleanup(); 
    }

    int start(int argc, char *argv[]) {
        gst_init(&argc, &argv);

        // Ensure HLS output directory exists
        std::filesystem::create_directories("hls");
        std::filesystem::create_directories("recordings");

        pipeline     = gst_pipeline_new("vms-pipeline");

        /* 1. Video Pipeline Elements */
        src          = gst_element_factory_make("rtspsrc",       "source");
        dep          = gst_element_factory_make("rtph264depay",  "depay");
        par          = gst_element_factory_make("h264parse",     "parse");

        /* 2. Audio Pipeline Elements (U-Law to AAC) */
        audio_depay  = gst_element_factory_make("rtppcmudepay",  "audio_depay");
        audio_dec    = gst_element_factory_make("mulawdec",      "audio_dec");
        audio_conv   = gst_element_factory_make("audioconvert",  "audio_conv");
        audio_resamp = gst_element_factory_make("audioresample", "audio_resamp");
        audio_enc    = gst_element_factory_make("avenc_aac",     "audio_enc");

        /* 3. KLV Metadata Source */
        klv_src      = gst_element_factory_make("appsrc",        "klv_src");

        /* 4. Muxing and HLS Sink */
        mux          = gst_element_factory_make("mpegtsmux",     "mux");
        que          = gst_element_factory_make("queue",         "hls_queue");
        hls          = gst_element_factory_make("hlssink",       "hls_sink");


        //this is fot the continous video recording

        tee = gst_element_factory_make("tee" , "tee_splitter");
        file_que = gst_element_factory_make("queue" , "file_queue");
        file_sink = gst_element_factory_make("filesink" , "file_sink");

        if(!tee || !file_que || !file_sink){
            std::cerr<<"[[gst]] failed to create tee/filesink/filequ\n";
            return -1;
        }




        // Validate element creation
        if (!pipeline || !src || !dep || !par || 
            !audio_depay || !audio_dec || !audio_conv || !audio_resamp || !audio_enc ||
            !klv_src || !mux || !que || !hls) {
            std::cerr << "[GST] Error: Failed to create all elements. Check installed plugins!\n";
            return -1;
        }

        /* 5. Configure Element Properties */
        g_object_set(src,
            "location",  "rtsp://192.168.0.32:8080/h264_ulaw.sdp",
            "protocols",  4,       // TCP transport
            "latency",    300,
            nullptr);

        g_object_set(par, "config-interval", 1, nullptr);

        GstCaps *klv_caps = gst_caps_from_string("meta/x-klv, parsed=(bool)true");
        g_object_set(klv_src,
            "caps",         klv_caps,
            "format",       GST_FORMAT_TIME,
            "is-live",      TRUE,
            "do-timestamp", TRUE,
            nullptr);
        gst_caps_unref(klv_caps);

        g_object_set(hls,
            "location",          "hls/segment_%05d.ts",
            "playlist-location", "hls/playlist.m3u8",
            "target-duration",   2,
            "max-files",         20,
            nullptr);


        g_object_set(file_sink, "location", "recordings/complete_record.ts", nullptr); // this is where i am saving the long recording in the nvr

        /* 6. Add all elements to the Bin */
        gst_bin_add_many(GST_BIN(pipeline),
            src, dep, par,
            audio_depay, audio_dec, audio_conv, audio_resamp, audio_enc,
            klv_src, mux, tee, que, hls, file_que, file_sink, nullptr);

        /* 7. Link Static Video Branch */
        if (!gst_element_link(dep, par)) {
            std::cerr << "[GST] Failed to link video elements: depay -> parse.\n";
            return -1;
        }

        /* 8. Link Static Audio Branch */
        if (!gst_element_link_many(audio_depay, audio_dec, audio_conv, audio_resamp, audio_enc, nullptr)) {
            std::cerr << "[GST] Failed to link audio processing chain.\n";
            return -1;
        }

        /* 9. Request Pads and Link into mpegtsmux */

        // --- Video -> Muxer ---
        GstPad *par_src  = gst_element_get_static_pad(par, "src");
        GstPad *mux_vpad = gst_element_request_pad_simple(mux, "sink_%d");
        if (!mux_vpad || gst_pad_link(par_src, mux_vpad) != GST_PAD_LINK_OK) {
            std::cerr << "[GST] Failed to link video to muxer.\n";
            if (par_src) gst_object_unref(par_src);
            if (mux_vpad) gst_object_unref(mux_vpad);
            return -1;
        }
        gst_object_unref(par_src);
        gst_object_unref(mux_vpad);

        // --- Audio -> Muxer ---
        GstPad *aud_src  = gst_element_get_static_pad(audio_enc, "src");
        GstPad *mux_apad = gst_element_request_pad_simple(mux, "sink_%d");
        if (!mux_apad || gst_pad_link(aud_src, mux_apad) != GST_PAD_LINK_OK) {
            std::cerr << "[GST] Failed to link audio encoder to muxer.\n";
            if (aud_src) gst_object_unref(aud_src);
            if (mux_apad) gst_object_unref(mux_apad);
            return -1;
        }
        gst_object_unref(aud_src);
        gst_object_unref(mux_apad);

        // --- KLV Metadata -> Muxer ---
        GstPad *klv_pad  = gst_element_get_static_pad(klv_src, "src");
        GstPad *mux_kpad = gst_element_request_pad_simple(mux, "sink_%d");
        if (!mux_kpad || gst_pad_link(klv_pad, mux_kpad) != GST_PAD_LINK_OK) {
            std::cerr << "[GST] Failed to link KLV appsrc to muxer.\n";
            if (klv_pad) gst_object_unref(klv_pad);
            if (mux_kpad) gst_object_unref(mux_kpad);
            return -1;
        }
        gst_object_unref(klv_pad);
        gst_object_unref(mux_kpad);

        //10. Link Muxer to HLS Output 
        //if (!gst_element_link_many(mux, que, hls, nullptr)) {
           // std::cerr << "[GST] Failed to link mux -> queue -> hlssink.\n";
            //return -1;
        //} 

        if(!gst_element_link(mux , tee)){
            std::cerr<< "[gst] failed to link the mux with tee.\n";
            return -1;
        }

        if(!gst_element_link_many(tee, que, hls, nullptr)){  //branch 1 link the tee with hls
            std::cerr<<"[gst] failed to link the tee with que->hls\n";
            return -1;
        }

        // baranching for the actual nvr  for the long recordings
        if(!gst_element_link_many(tee , file_que, file_sink , nullptr)){
            std::cerr<<"[gst] failed to cretae tee with tee->file_que->file->sink.\n";
            return -1;
        }


        /* 11. Connect RTSP Signals */
        g_signal_connect(src, "select-stream", G_CALLBACK(selectStreamCb), this);
        g_signal_connect(src, "pad-added",     G_CALLBACK(padAddedCb),     this);

        /* 12. Start GPS Background Thread */
        terminate_app = false;
        video_linked  = false;
        gps_thread    = std::thread(&KlvPipeline::gpsThreadFunction, this);

        /* 13. Start Pipeline Execution */
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            std::cerr << "[GST] Failed to set pipeline to PLAYING state.\n";
            return -1;
        }

        /* 14. Start Bus Monitor Thread */
        bus_thread = std::thread(&KlvPipeline::busThreadFunction, this);
        return 0;
    }

    void play()  { if (pipeline) gst_element_set_state(pipeline, GST_STATE_PLAYING); }
    void pause() { if (pipeline) gst_element_set_state(pipeline, GST_STATE_PAUSED);  }

    void forceCleanup() {
        terminate_app = true;
        if (gps_thread.joinable()) gps_thread.join();
        if (bus_thread.joinable()) bus_thread.join();

        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
    }

private:
    // Core pipeline elements
    GstElement        *pipeline;
    GstElement        *src, *dep, *par;
    GstElement        *audio_depay, *audio_dec, *audio_conv, *audio_resamp, *audio_enc;
    GstElement        *klv_src;
    GstElement        *mux, *que, *hls;

    GstElement        *tee, *file_que, *file_sink;  // this is for the contioosu video recording 

    GstBus            *bus;

    // Threads and synchronization
    std::thread        bus_thread;
    std::thread        gps_thread;
    std::atomic<bool>  terminate_app;
    std::atomic<bool>  video_linked;

    /* BER Length encoding for KLV packs */
    static void ber_encode_length(size_t len, std::vector<uint8_t> &out) {
        if (len < 128) {
            out.push_back(static_cast<uint8_t>(len));
        } else if (len < 256) {
            out.push_back(0x81);
            out.push_back(static_cast<uint8_t>(len));
        } else {
            out.push_back(0x82);
            out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>( len       & 0xFF));
        }
    }

    /* Wrap raw NMEA line into SMPTE KLV structure */
    static std::vector<uint8_t> encode_klv(const std::string &nmea_sentence) {
        std::vector<uint8_t> packet;
        packet.reserve(16 + 3 + nmea_sentence.size());
        packet.insert(packet.end(), NMEA_KLV_KEY, NMEA_KLV_KEY + 16);
        ber_encode_length(nmea_sentence.size(), packet);
        const auto *data = reinterpret_cast<const uint8_t *>(nmea_sentence.data());
        packet.insert(packet.end(), data, data + nmea_sentence.size());
        return packet;
    }

    /* Background thread reading GPS port and injecting into appsrc */
    void gpsThreadFunction() {
        const std::string gps_port_path = "/dev/pts/4"; // Adjust port as needed
        std::ifstream gps_port(gps_port_path);

        if (!gps_port.is_open()) {
            std::cerr << "[GNSS] WARNING: Could not open GNSS port " << gps_port_path << "\n";
            return;
        }

        std::cout << "[GNSS] Waiting for video stream to connect...\n";
        while (!video_linked && !terminate_app) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        if (terminate_app) return;
        std::cout << "[GNSS] Started — injecting KLV packets into HLS stream...\n";

        std::string line;
        while (!terminate_app && std::getline(gps_port, line)) {
            if (line.empty()) continue;

            std::vector<uint8_t> klv_packet = encode_klv(line);
            GstBuffer *buf = gst_buffer_new_allocate(nullptr, klv_packet.size(), nullptr);
            gst_buffer_fill(buf, 0, klv_packet.data(), klv_packet.size());

            GstFlowReturn flow_ret;
            g_signal_emit_by_name(klv_src, "push-buffer", buf, &flow_ret);
            gst_buffer_unref(buf);

            if (flow_ret != GST_FLOW_OK) break;
        }

        GstFlowReturn eos_ret;
        g_signal_emit_by_name(klv_src, "end-of-stream", &eos_ret);
    }

    /* Bus monitor loop */
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
                    std::cout << "[GST] End of stream reached.\n";
                    terminate_app = true;
                    break;
                default:
                    break;
            }
            gst_message_unref(msg);
        }
        if (bus) {
            gst_object_unref(bus);
            bus = nullptr;
        }
    }

    /* Accept both video and audio streams from RTSP */
    static gboolean selectStreamCb(GstElement * /*src*/, guint /*num*/,
                                   GstCaps *caps, gpointer /*user_data*/) {
        GstStructure *s     = gst_caps_get_structure(caps, 0);
        const gchar  *media = gst_structure_get_string(s, "media");
        
        if (g_strcmp0(media, "video") == 0 || g_strcmp0(media, "audio") == 0) {
            return TRUE;
        }
        return FALSE;
    }

    /* Dynamic pad connector for RTP video and audio */
    static void padAddedCb(GstElement *src_elem, GstPad *new_pad, gpointer user_data) {
        KlvPipeline  *self       = static_cast<KlvPipeline *>(user_data);
        GstCaps      *caps       = gst_pad_get_current_caps(new_pad);
        if (!caps)    caps       = gst_pad_query_caps(new_pad, nullptr);
        if (!caps)    return;

        GstStructure *pad_struct = gst_caps_get_structure(caps, 0);
        const gchar  *pad_type   = gst_structure_get_name(pad_struct);
        const gchar  *media      = gst_structure_get_string(pad_struct, "media");

        std::cout << "[GST] New pad '" << GST_PAD_NAME(new_pad)
                  << "' from '" << GST_ELEMENT_NAME(src_elem) << "'\n";

        if (!g_str_has_prefix(pad_type, "application/x-rtp")) {
            gst_caps_unref(caps);
            return;
        }

        if (g_strcmp0(media, "video") == 0) {
            GstPad *sink_pad = gst_element_get_static_pad(self->dep, "sink");
            if (sink_pad) {
                if (!gst_pad_is_linked(sink_pad)) {
                    if (gst_pad_link(new_pad, sink_pad) == GST_PAD_LINK_OK) {
                        std::cout << "[GST] Video branch linked successfully.\n";
                        self->video_linked = true;
                    }
                }
                gst_object_unref(sink_pad);
            }
        } else if (g_strcmp0(media, "audio") == 0) {
            GstPad *sink_pad = gst_element_get_static_pad(self->audio_depay, "sink");
            if (sink_pad) {
                if (!gst_pad_is_linked(sink_pad)) {
                    if (gst_pad_link(new_pad, sink_pad) == GST_PAD_LINK_OK) {
                        std::cout << "[GST] Audio branch linked successfully.\n";
                    }
                }
                gst_object_unref(sink_pad);
            }
        }

        gst_caps_unref(caps);
    }
};


//g++ main.cpp -o vms_server -std=c++17 $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0) -loatpp -pthread

//socat -d -d pty,raw,echo=0 pty,raw,echo=0
// ./vms_server