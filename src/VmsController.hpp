#pragma once

#include "oatpp/web/server/api/ApiController.hpp"
#include "oatpp/data/mapping/ObjectMapper.hpp"
#include "oatpp/macro/codegen.hpp"
#include "KlvPipeline.hpp"

#include <fstream>
#include <string>

#include OATPP_CODEGEN_BEGIN(ApiController)

class VmsController : public oatpp::web::server::api::ApiController {
private:
    std::shared_ptr<KlvPipeline> m_pipeline;

    // Binary-safe file reader — preserves null bytes inside .ts segments
    static oatpp::String readFileBinary(const std::string &path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f.is_open()) return nullptr;

        auto size = f.tellg();
        if (size <= 0) return nullptr;

        f.seekg(0, std::ios::beg);
        auto buf = std::make_shared<std::string>(static_cast<size_t>(size), '\0');
        if (!f.read(&(*buf)[0], size)) return nullptr;
        return oatpp::String(buf);   // passes the full binary content
    }

public:
    VmsController(const std::shared_ptr<ObjectMapper> &objectMapper,
                  const std::shared_ptr<KlvPipeline>  &pipeline)
        : oatpp::web::server::api::ApiController(objectMapper),
          m_pipeline(pipeline) {}

    // ── HTML player ───────────────────────────────────────────────────────
    ENDPOINT("GET", "/player", getWebPlayer) {
        const char *html = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8"/>
  <meta name="viewport" content="width=device-width, initial-scale=1"/>
  <title>VMS Dashboard</title>
  <script src="https://cdn.jsdelivr.net/npm/hls.js@1.4.12/dist/hls.min.js"></script>
  <style>
    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background: #141414; color: #eee;
      font-family: 'Segoe UI', sans-serif;
      display: flex; flex-direction: column; align-items: center;
      padding: 40px 20px; gap: 16px;
    }
    h1  { font-size: 1.4rem; letter-spacing: 1px; }
    #video {
      width: 100%; max-width: 880px; aspect-ratio: 16/9;
      background: #000; border: 2px solid #333; border-radius: 6px;
    }
    .controls { display: flex; gap: 12px; }
    button {
      padding: 9px 28px; border: none; border-radius: 4px;
      cursor: pointer; font-size: 14px; font-weight: 600;
    }
    #btnPlay  { background: #28a745; color: #fff; }
    #btnPause { background: #dc3545; color: #fff; }
    #status   { font-size: 13px; color: #888; }
  </style>
</head>
<body>
  <h1>&#127910; Live Camera Feed</h1>
  <video id="video" controls autoplay muted playsinline></video>
  <div class="controls">
    <button id="btnPlay"  onclick="cameraPlay()">&#9654; Resume</button>
    <button id="btnPause" onclick="cameraPause()">&#9646;&#9646; Pause</button>
  </div>
  <p id="status">Initialising stream&#8230;</p>

<script>
  const video  = document.getElementById('video');
  const status = document.getElementById('status');

  function setStatus(msg) { status.textContent = msg; }

  if (Hls.isSupported()) {
    const hls = new Hls({
      liveSyncDurationCount:       3,
      liveMaxLatencyDurationCount: 10,
      maxLiveSyncPlaybackRate:     1.5,
      lowLatencyMode:              false,
      enableWorker:                true,
      backBufferLength:            30,
    });

    hls.loadSource('/hls/playlist.m3u8');
    hls.attachMedia(video);

    hls.on(Hls.Events.MANIFEST_PARSED, () => {
      setStatus('✅ Manifest loaded — starting playback');
      video.play().catch(() => setStatus('Click ▶ to start playback'));
    });

    hls.on(Hls.Events.FRAG_LOADED,    () => setStatus('🟢 Live'));
    hls.on(Hls.Events.BUFFER_STALLED, () => setStatus('⏳ Buffering…'));

    hls.on(Hls.Events.ERROR, (_, data) => {
      if (!data.fatal) return;
      if (data.type === Hls.ErrorTypes.NETWORK_ERROR) {
        setStatus('⚠ Network error — retrying in 3 s…');
        setTimeout(() => hls.startLoad(), 3000);
      } else if (data.type === Hls.ErrorTypes.MEDIA_ERROR) {
        setStatus('⚠ Media error — recovering…');
        hls.recoverMediaError();
      } else {
        setStatus('❌ Fatal HLS error: ' + data.details);
        hls.destroy();
      }
    });

  } else if (video.canPlayType('application/vnd.apple.mpegurl')) {
    // Safari / iOS — native HLS
    video.src = '/hls/playlist.m3u8';
    video.play();
    setStatus('Using native HLS (Safari)');
  } else {
    setStatus('❌ HLS is not supported by this browser.');
  }

  function cameraPlay()  { fetch('/api/camera/play',  { method: 'POST' }); }
  function cameraPause() { fetch('/api/camera/pause', { method: 'POST' }); }
</script>
</body>
</html>
)html";
        auto r = createResponse(Status::CODE_200, html);
        r->putHeader("Content-Type", "text/html; charset=utf-8");
        return r;
    }

    // ── HLS playlist ──────────────────────────────────────────────────────
    // BUG FIX: this endpoint was completely missing — browser got 404
    ENDPOINT("GET", "/hls/playlist.m3u8", getPlaylist) {
        auto body = readFileBinary("hls/playlist.m3u8");
        if (!body) {
            auto r = createResponse(Status::CODE_503,
                                    "Stream not ready yet — retry in a moment.");
            r->putHeader("Retry-After", "2");
            return r;
        }
        auto r = createResponse(Status::CODE_200, body);
        r->putHeader("Content-Type",  "application/vnd.apple.mpegurl");
        r->putHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        r->putHeader("Access-Control-Allow-Origin", "*");
        return r;
    }

    // ── HLS segments (.ts) ────────────────────────────────────────────────
    // BUG FIX: this endpoint was completely missing — browser got 404
    ENDPOINT("GET", "/hls/{filename}", getSegment,
             PATH(oatpp::String, filename)) {

        // Basic path-traversal guard
        std::string fname = filename->c_str();
        if (fname.find("..") != std::string::npos ||
            fname.find('/')  != std::string::npos) {
            return createResponse(Status::CODE_400, "Invalid filename.");
        }

        auto body = readFileBinary("hls/" + fname);
        if (!body) {
            return createResponse(Status::CODE_404, "Segment not found.");
        }
        auto r = createResponse(Status::CODE_200, body);
        r->putHeader("Content-Type",  "video/mp2t");
        r->putHeader("Cache-Control", "no-cache");
        r->putHeader("Access-Control-Allow-Origin", "*");
        return r;
    }

    // ── Camera control ────────────────────────────────────────────────────
    ENDPOINT("POST", "/api/camera/play", playCamera) {
        m_pipeline->play();
        return createResponse(Status::CODE_200, "PLAY");
    }

    ENDPOINT("POST", "/api/camera/pause", pauseCamera) {
        m_pipeline->pause();
        return createResponse(Status::CODE_200, "PAUSE");
    }
};

#include OATPP_CODEGEN_END(ApiController)