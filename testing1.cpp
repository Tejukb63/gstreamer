// this is klv code and gstreamer code
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

// ---------------------------------------------------------------------------
// 16-byte private Universal Label for NMEA sentences.
// Uses the SMPTE experimental UL prefix (06.0E.2B.34) with private trailing
// bytes — last four spell 'N','M','E','A' for easy identification in a hex dump.
// ---------------------------------------------------------------------------
static const uint8_t NMEA_KLV_KEY[16] = {
    0x06, 0x0E, 0x2B, 0x34,
    0x02, 0x0B, 0x01, 0x01,
    0x0E, 0x01, 0x03, 0x01,
    0x4E, 0x4D, 0x45, 0x41   // 'N','M','E','A'
};

class KlvPipeline {
public:
    KlvPipeline() : pipeline(nullptr), source(nullptr), depay(nullptr),
                    parse(nullptr), sink(nullptr), klv_src(nullptr),
                    bus(nullptr), terminate_app(false), video_linked(false) {}

    ~KlvPipeline() {
        stop();
    }

    int run(int argc, char *argv[]) {
        /* 1. Initialize GStreamer */
        gst_init(&argc, &argv);

        /* 2. Create the elements */
        source   = gst_element_factory_make("rtspsrc",     "source");
        depay    = gst_element_factory_make("rtph264depay", "depay");
        parse    = gst_element_factory_make("h264parse",    "parse");
        sink     = gst_element_factory_make("splitmuxsink", "sink");
        klv_src  = gst_element_factory_make("appsrc",       "klv_src");
        pipeline = gst_pipeline_new("rtsp-split-pipeline");

        if (!pipeline || !source || !depay || !parse || !sink || !klv_src) {
            std::cerr << "Not all elements could be created.\n";
            return -1;
        }

        /* 3. Configure elements */
        g_object_set(source,
                     "location",  "rtsp://192.168.0.32:8080/h264_ulaw.sdp",
                     "protocols", 4,
                     nullptr);

        g_object_set(parse, "config-interval", 1, nullptr);

        g_object_set(sink,
                     "location",      "cameratej_%02d.ts",
                     "max-size-time", (guint64)(600 * GST_SECOND),
                     "muxer-factory", "mpegtsmux",
                     nullptr);

        // BUG 2 FIX: caps changed from bare "meta/x-klv" to include parsed=true
        // so mpegtsmux knows each buffer is already a complete, framed KLV packet.
        
        GstCaps *klv_caps = gst_caps_from_string("meta/x-klv, parsed=(bool)true");
        g_object_set(klv_src,
                     "caps",         klv_caps,
                     "format",       GST_FORMAT_TIME,
                     "is-live",      TRUE,
                     "do-timestamp", TRUE,
                     nullptr);
        gst_caps_unref(klv_caps);

        /* 4. Build the pipeline */
        gst_bin_add_many(GST_BIN(pipeline), source, depay, parse, sink, klv_src, nullptr);

        if (!gst_element_link(depay, parse)) {
            std::cerr << "depay -> parse could not be linked.\n";
            gst_object_unref(pipeline);
            return -1;
        }

        /* 5. Request pads from splitmuxsink */

        // --- video pad ---
        GstPad *parse_src  = gst_element_get_static_pad(parse, "src");
        GstPad *mux_video  = gst_element_request_pad_simple(sink, "video");

        // BUG 3 FIX: null-check on every requested pad before calling gst_pad_link
        if (!mux_video) {
            std::cerr << "splitmuxsink refused video pad.\n";
            gst_object_unref(parse_src);
            gst_object_unref(pipeline);
            return -1;
        }
        if (gst_pad_link(parse_src, mux_video) != GST_PAD_LINK_OK)
            std::cerr << "Failed to link parse to splitmuxsink video pad.\n";

        gst_object_unref(parse_src);
        gst_object_unref(mux_video);

        // --- KLV / subtitle pad ---
        GstPad *klv_src_pad   = gst_element_get_static_pad(klv_src, "src");
        GstPad *mux_meta_sink = gst_element_request_pad_simple(sink, "subtitle_%u");

        // BUG 3 FIX: null-check before link
        if (!mux_meta_sink) {
            std::cerr << "splitmuxsink refused subtitle pad — check muxer/caps.\n";
            gst_object_unref(klv_src_pad);
            gst_object_unref(pipeline);
            return -1;
        }
        if (gst_pad_link(klv_src_pad, mux_meta_sink) != GST_PAD_LINK_OK)
            std::cerr << "Failed to link KLV appsrc to splitmuxsink subtitle pad.\n";

        gst_object_unref(klv_src_pad);
        gst_object_unref(mux_meta_sink);

        /* 6. Connect signals */
        g_signal_connect(source, "select-stream", G_CALLBACK(selectStreamCb), this);
        g_signal_connect(source, "pad-added",     G_CALLBACK(padAddedCb),     this);

        /* 7. Start background GPS/KLV thread */
        terminate_app = false;
        gps_thread = std::thread(&KlvPipeline::gpsThreadFunction, this);

        /* 8. Start playing */
        std::cout << "Connecting to camera...\n";
        GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            std::cerr << "Unable to set pipeline to PLAYING.\n";
            gst_object_unref(pipeline);
            return -1;
        }

        /* 9. Message bus loop */
        bus = gst_element_get_bus(pipeline);

        while (!terminate_app) {
            GstMessage *msg = gst_bus_timed_pop_filtered(   //GST_CLOCK_TIME_NONE
                bus, 30 * GST_SECOND,
                (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));

            if (msg != nullptr) {
                if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                    GError *err       = nullptr;
                    gchar  *debug_info = nullptr;
                    gst_message_parse_error(msg, &err, &debug_info);
                    std::cerr << "\nPipeline Error: " << err->message << "\n"
                              << "Debug info: " << (debug_info ? debug_info : "none") << "\n";
                    g_clear_error(&err);
                    g_free(debug_info);
                } else {
                    std::cout << "\nStream finished naturally.\n";
                }
                gst_message_unref(msg);
                break;
            } else {
                std::cout << "\nTimeout reached — safely finalizing file...\n";
                gst_element_send_event(pipeline, gst_event_new_eos());
                std::this_thread::sleep_for(std::chrono::seconds(2));
                break;
            }
        }

        return 0;
    }

    void stop() {
        terminate_app = true;

        if (gps_thread.joinable())
            gps_thread.join();

        if (bus) {
            gst_object_unref(bus);
            bus = nullptr;
        }

        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
    }

private:
    GstElement *pipeline, *source, *depay, *parse, *sink, *klv_src;
    GstBus     *bus;

    std::thread       gps_thread;
    std::atomic<bool> terminate_app;
    std::atomic<bool> video_linked;

    // ----------------------------------------------------------------------
    // BER-encode a byte length into `out`.
    // Handles values up to 65535 (short-form + 1- and 2-byte long-forms).
    // -----------------------------------------------------------------------
    static void ber_encode_length(size_t len, std::vector<uint8_t> &out) {
        if (len < 128) {
            // Short form: single byte
            out.push_back(static_cast<uint8_t>(len));
        } else if (len < 256) {
            // Long form, 1 subsequent byte
            out.push_back(0x81);
            out.push_back(static_cast<uint8_t>(len));
        } else {
            // Long form, 2 subsequent bytes
            out.push_back(0x82);
            out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>( len       & 0xFF));
        }
    }

    // -----------------------------------------------------------------------
    // BUG 2 FIX: wrap an NMEA sentence in a proper binary KLV packet.
    //
    // Previously the code pushed the raw "$GPGGA,..." ASCII string directly
    // into buffers advertised as meta/x-klv.  KLV is a binary format:
    //   [ 16-byte Universal Label ][ BER length ][ value bytes ]
    // mpegtsmux / downstream readers expect this structure, not plain text.
    // -----------------------------------------------------------------------
    static std::vector<uint8_t> encode_klv(const std::string &nmea_sentence) {
        std::vector<uint8_t> packet;
        packet.reserve(16 + 3 + nmea_sentence.size());

        // Key — 16-byte private Universal Label
        packet.insert(packet.end(), NMEA_KLV_KEY, NMEA_KLV_KEY + 16);

        // Length — BER-encoded
        ber_encode_length(nmea_sentence.size(), packet);

        // Value — raw NMEA sentence bytes
        const auto *data = reinterpret_cast<const uint8_t *>(nmea_sentence.data());
        packet.insert(packet.end(), data, data + nmea_sentence.size());

        return packet;
    }

    // -----------------------------------------------------------------------
    // Background thread: reads NMEA lines and injects them as KLV buffers.
    // -----------------------------------------------------------------------
    void gpsThreadFunction() {
        const std::string gps_port_path = "/dev/pts/4";
        std::ifstream gps_port(gps_port_path);

        if (!gps_port.is_open()) {
            std::cerr << "WARNING: Could not open GNSS port " << gps_port_path << "\n";
            return;
        }

        std::cout << "GNSS Thread: waiting for video stream to connect...\n";
        while (!video_linked && !terminate_app)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

        if (terminate_app) return;

        std::cout << "GNSS Thread: started — injecting KLV into MPEG-TS...\n";

        std::string line;
        while (!terminate_app && std::getline(gps_port, line)) {
            if (line.empty()) continue;

            // Build a properly framed KLV binary packet from the NMEA sentence
            std::vector<uint8_t> klv_packet = encode_klv(line);

            GstBuffer *buf = gst_buffer_new_allocate(nullptr, klv_packet.size(), nullptr);
            gst_buffer_fill(buf, 0, klv_packet.data(), klv_packet.size());

            GstFlowReturn flow_ret;
            g_signal_emit_by_name(klv_src, "push-buffer", buf, &flow_ret);
            gst_buffer_unref(buf);

            if (flow_ret != GST_FLOW_OK) {
                std::cerr << "Error pushing KLV buffer (flow=" << flow_ret << ")\n";
                break;
            }
        }

        std::cout << "GNSS Thread: shutting down...\n";
        GstFlowReturn eos_ret;
        g_signal_emit_by_name(klv_src, "end-of-stream", &eos_ret);
    }

    // -----------------------------------------------------------------------
    // BUG 1 FIX: only select video streams.
    //
    // Previously this returned TRUE for both video and audio.  rtspsrc then
    // created an audio RTP pad that padAddedCb ignored ("we are already
    // linked"), leaving it unlinked.  When rtspsrc pushed audio buffers onto
    // that pad it got GST_FLOW_NOT_LINKED, which rtspsrc escalated to a fatal
    // GST_MESSAGE_ERROR — "Internal data stream error."
    // -----------------------------------------------------------------------
    static gboolean selectStreamCb(GstElement * /*rtspsrc*/, guint /*num*/,
                                   GstCaps *caps, gpointer /*user_data*/) {
        GstStructure *s     = gst_caps_get_structure(caps, 0);
        const gchar  *media = gst_structure_get_string(s, "media");
        return (g_strcmp0(media, "video") == 0) ? TRUE : FALSE;
    }

    // -----------------------------------------------------------------------
    // Dynamically link the video RTP pad from rtspsrc to rtph264depay.
    // -----------------------------------------------------------------------
    static void padAddedCb(GstElement *src, GstPad *new_pad, gpointer user_data) {
        KlvPipeline  *self          = static_cast<KlvPipeline *>(user_data);
        GstPad       *sink_pad      = gst_element_get_static_pad(self->depay, "sink");
        GstCaps      *new_pad_caps  = gst_pad_get_current_caps(new_pad);
        GstStructure *pad_struct    = gst_caps_get_structure(new_pad_caps, 0);
        const gchar  *pad_type      = gst_structure_get_name(pad_struct);

        std::cout << "New pad '" << GST_PAD_NAME(new_pad)
                  << "' from '"  << GST_ELEMENT_NAME(src) << "'\n";

        if (gst_pad_is_linked(sink_pad)) {
            std::cout << "Already linked — ignoring.\n";
            goto exit;
        }

        if (!g_str_has_prefix(pad_type, "application/x-rtp")) {
            std::cout << "Not RTP ('" << pad_type << "') — ignoring.\n";
            goto exit;
        }

        if (g_strcmp0(gst_structure_get_string(pad_struct, "media"), "video") != 0) {
            std::cout << "Not video — ignoring.\n";
            goto exit;
        }

        if (GST_PAD_LINK_FAILED(gst_pad_link(new_pad, sink_pad))) {
            std::cerr << "Link failed for type '" << pad_type << "'\n";
        } else {
            std::cout << "Video linked. Recording started...\n";
            self->video_linked = true;
        }

    exit:
        if (new_pad_caps) gst_caps_unref(new_pad_caps);
        gst_object_unref(sink_pad);
    }
};

int main(int argc, char *argv[]) {
    KlvPipeline pipeline;
    return pipeline.run(argc, argv);
}