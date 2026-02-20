#include "FS.h"
#include "SD_MMC.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "img_converters.h"
#include <Arduino.h>
#include <WiFi.h>

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ポート80: API/ページ用、ポート81: ストリーム専用
httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;
static bool grayscaleMode = false;
static bool vflipMode = false;
static bool hmirrorMode = false;

// ストリーム統計情報
static float stream_fps = 0;
static size_t stream_frame_size = 0;
static bool stream_active = false;
static float chip_temp = 0;
static int adaptive_min_frame_ms = 33; // 温度に応じて調整

#define TEMP_NORMAL 70
#define TEMP_WARM 80
#define FRAME_MS_NORMAL 33  // ~30fps
#define FRAME_MS_WARM 66    // ~15fps
#define FRAME_MS_HOT 100    // ~10fps

// main.cppの関数/変数を参照
extern bool saveImageToSD(camera_fb_t *fb, char *outFilename, size_t maxLen);
extern bool uploadImage(camera_fb_t *fb, const char *filename);
extern bool sdCardAvailable;
extern int imageCount;

// ────────────────────────────────────────
// ストリームハンドラ（ポート81で動作・最適化版）
// ────────────────────────────────────────
#define STREAM_TARGET_FPS 30
#define STREAM_MIN_FRAME_MS (1000 / STREAM_TARGET_FPS)

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t *_jpg_buf = NULL;
  char part_buf[64];

  // FPS計測用
  unsigned long fps_start = millis();
  int fps_count = 0;

  stream_active = true;

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    stream_active = false;
    return res;
  }

  while (true) {
    unsigned long frame_start = millis();

    fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
    } else {
      if (fb->format != PIXFORMAT_JPEG) {
        bool ok = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
        esp_camera_fb_return(fb);
        fb = NULL;
        if (!ok)
          res = ESP_FAIL;
      } else {
        _jpg_buf_len = fb->len;
        _jpg_buf = fb->buf;
      }
    }
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY,
                                  strlen(_STREAM_BOUNDARY));
    

    
    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if (_jpg_buf) {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if (res != ESP_OK)
      break;
    
    // 統計情報更新（実際に送信したフレームサイズ）
    stream_frame_size = _jpg_buf_len;
    fps_count++;
    unsigned long now = millis();
    if (now - fps_start >= 1000) {
      stream_fps = (float)fps_count * 1000.0f / (float)(now - fps_start);
      fps_count = 0;
      fps_start = now;
      // 温度監視とFPS調整（1秒ごと）
      float t = temperatureRead();
      // 110度以上やマイナスはAPIやハードの取得エラー(125度など)とみなす
      chip_temp = (t > 110.0f || t < -20.0f) ? -1.0f : t;
      
      if (chip_temp < 0 || chip_temp < TEMP_NORMAL) {
        adaptive_min_frame_ms = FRAME_MS_NORMAL;
      } else if (chip_temp < TEMP_WARM) {
        adaptive_min_frame_ms = FRAME_MS_WARM;
      } else {
        adaptive_min_frame_ms = FRAME_MS_HOT;
      }
    }

    // フレームレート制御（温度とWiFi負荷に応じて調整）
    unsigned long frame_time = millis() - frame_start;
    if (frame_time < adaptive_min_frame_ms) {
      delay(adaptive_min_frame_ms - frame_time);
    }
  }
  stream_active = false;
  stream_fps = 0;
  return res;
}

// ────────────────────────────────────────
// API ハンドラ（ポート80で動作）
// ────────────────────────────────────────

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition",
                     "inline; filename=capture.jpg");
  esp_err_t res;
  if (fb->format == PIXFORMAT_JPEG) {
    res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  } else {
    uint8_t *jpg_buf = NULL;
    size_t jpg_buf_len = 0;
    if (frame2jpg(fb, 80, &jpg_buf, &jpg_buf_len)) {
      res = httpd_resp_send(req, (const char *)jpg_buf, jpg_buf_len);
      free(jpg_buf);
    } else {
      esp_camera_fb_return(fb);
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
  }
  // SDカードに保存
  if (sdCardAvailable) {
    char filename[64] = "";
    if (saveImageToSD(fb, filename, sizeof(filename))) {
      imageCount++;
      // アップロードパラメータがある場合はクラウドにアップロード
      if (req->uri && strstr(req->uri, "upload=1")) {
        uploadImage(fb, filename);
      }
    }
  }
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t settings_handler(httpd_req_t *req) {
  char buf[128];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  buf[ret] = '\0';
  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  char *p;
  p = strstr(buf, "framesize=");
  if (p)
    s->set_framesize(s, (framesize_t)atoi(p + 10));
  p = strstr(buf, "grayscale=");
  if (p) {
    int val = atoi(p + 10);
    if (val) {
      s->set_special_effect(s, 2);
      grayscaleMode = true;
    } else {
      s->set_special_effect(s, 0);
      grayscaleMode = false;
    }
  }
  p = strstr(buf, "vflip=");
  if (p) {
    int val = atoi(p + 10);
    s->set_vflip(s, val);
    vflipMode = val;
  }
  p = strstr(buf, "hmirror=");
  if (p) {
    int val = atoi(p + 10);
    s->set_hmirror(s, val);
    hmirrorMode = val;
  }
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  char json[128];
  snprintf(json, sizeof(json), "{\"framesize\":%d,\"grayscale\":%d,\"vflip\":%d,\"hmirror\":%d}",
           s->status.framesize, grayscaleMode ? 1 : 0, vflipMode ? 1 : 0, hmirrorMode ? 1 : 0);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  return ESP_OK;
}

static esp_err_t files_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  if (SD_MMC.cardType() == CARD_NONE) {
    httpd_resp_sendstr(req, "{\"files\":[]}");
    return ESP_OK;
  }
  File root = SD_MMC.open("/");
  if (!root || !root.isDirectory()) {
    httpd_resp_sendstr(req, "{\"files\":[]}");
    return ESP_OK;
  }
  httpd_resp_sendstr_chunk(req, "{\"files\":[");
  bool first = true;
  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      const char *name = file.name();
      size_t len = strlen(name);
      if (len > 4 && strcasecmp(name + len - 4, ".jpg") == 0) {
        char entry[128];
        snprintf(entry, sizeof(entry), "%s{\"name\":\"%s\",\"size\":%d}",
                 first ? "" : ",", name, (int)file.size());
        httpd_resp_sendstr_chunk(req, entry);
        first = false;
      }
    }
    file.close();
    file = root.openNextFile();
  }
  root.close();
  httpd_resp_sendstr_chunk(req, "]}");
  httpd_resp_sendstr_chunk(req, NULL);
  return ESP_OK;
}

static esp_err_t sdfile_handler(httpd_req_t *req) {
  const char *filepath = req->uri + 3; // skip "/sd"
  if (SD_MMC.cardType() == CARD_NONE) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  File file = SD_MMC.open(filepath, FILE_READ);
  if (!file) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  char *buf = (char *)malloc(4096);
  if (!buf) {
    file.close();
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  size_t rd;
  while ((rd = file.read((uint8_t *)buf, 4096)) > 0) {
    if (httpd_resp_send_chunk(req, buf, rd) != ESP_OK) {
      free(buf);
      file.close();
      return ESP_FAIL;
    }
    delay(2); // CPU負荷と発熱を抑えるためSDカード読み取り時に少し休止
  }
  free(buf);
  file.close();
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t delete_handler(httpd_req_t *req) {
  char buf[128];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  buf[ret] = '\0';
  char *p = strstr(buf, "file=");
  if (!p) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  SD_MMC.remove(p + 5);
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

// ストリーム統計情報API
static esp_err_t stats_handler(httpd_req_t *req) {
  char buf[160];
  int rssi = WiFi.RSSI();
  snprintf(buf, sizeof(buf),
           "{\"fps\":%.1f,\"frameKB\":%.1f,\"rssi\":%d,\"temp\":%.1f,\"streaming\":%s}",
           stream_fps, stream_frame_size / 1024.0f, rssi, chip_temp,
           stream_active ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, buf);
  return ESP_OK;
}

// ────────────────────────────────────────
// HTML（ストリームURLをポート81に向ける）
// ────────────────────────────────────────
static const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
  <title>ESP32-S3 Camera Stream</title>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/jszip/3.10.1/jszip.min.js"></script>
  <style>
    :root {
      --bg: #0f111a;
      --panel-bg: rgba(30, 33, 43, 0.6);
      --glass-border: rgba(255, 255, 255, 0.08);
      --primary: #6366f1;
      --primary-hover: #4f46e5;
      --accent: #0ea5e9;
      --text: #f8fafc;
      --text-muted: #94a3b8;
      --danger: #ef4444;
      --danger-hover: #dc2626;
      --success: #10b981;
      --warning: #f59e0b;
      --radius: 12px;
      --radius-sm: 8px;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      background: var(--bg);
      color: var(--text);
      min-height: 100vh;
      display: flex;
      flex-direction: column;
    }
    /* Glass Panel Utility */
    .glass {
      background: var(--panel-bg);
      backdrop-filter: blur(12px);
      -webkit-backdrop-filter: blur(12px);
      border: 1px solid var(--glass-border);
      box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
    }
    header {
      padding: 16px 24px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      border-bottom: 1px solid var(--glass-border);
      position: sticky;
      top: 0;
      z-index: 10;
      background: rgba(15, 17, 26, 0.8);
      backdrop-filter: blur(10px);
    }
    header h1 {
      font-size: 1.25rem;
      font-weight: 700;
      color: var(--primary);
      letter-spacing: -0.5px;
    }
    .tabs { display: flex; gap: 8px; background: rgba(0,0,0,0.2); padding: 4px; border-radius: 10px; }
    .tab {
      padding: 6px 16px;
      border-radius: 6px;
      font-size: 0.875rem;
      font-weight: 500;
      color: var(--text-muted);
      cursor: pointer;
      transition: all 0.3s ease;
    }
    .tab:hover { color: var(--text); }
    .tab.active { background: var(--primary); color: #fff; box-shadow: 0 2px 8px rgba(99,102,241,0.4); }
    
    .container {
      flex: 1;
      padding: 20px;
      max-width: 1200px;
      margin: 0 auto;
      width: 100%;
    }
    .pg { display: none; animation: fadeIn 0.4s ease; }
    .pg.active { display: block; }
    @keyframes fadeIn { from { opacity: 0; transform: translateY(10px); } to { opacity: 1; transform: translateY(0); } }

    /* Live Stream Layout */
    .live-layout {
      display: grid;
      gap: 24px;
      grid-template-columns: 1fr;
    }
    @media (min-width: 800px) {
      .live-layout { grid-template-columns: 1.5fr 1fr; }
    }
    
    .stream-container {
      position: relative;
      border-radius: var(--radius);
      overflow: hidden;
      display: flex;
      align-items: center;
      justify-content: center;
      min-height: 240px;
    }
    #stream {
      width: 100%;
      height: 100%;
      object-fit: contain;
      border-radius: var(--radius);
      background: #000;
    }
    
    .controls {
      display: flex;
      flex-direction: column;
      gap: 16px;
      padding: 20px;
      border-radius: var(--radius);
    }
    .control-group {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding-bottom: 12px;
      border-bottom: 1px solid rgba(255,255,255,0.05);
    }
    .control-group:last-child { border-bottom: none; padding-bottom: 0; }
    
    label.title { font-size: 0.875rem; font-weight: 500; color: var(--text-muted); }
    
    select {
      background: rgba(0,0,0,0.3);
      color: var(--text);
      border: 1px solid rgba(255,255,255,0.1);
      padding: 8px 12px;
      border-radius: var(--radius-sm);
      font-size: 0.875rem;
      outline: none;
      appearance: none;
      cursor: pointer;
      min-width: 140px;
    }
    select:focus { border-color: var(--primary); }
    select option { background: var(--bg); }

    /* Modern Toggle */
    .tg { position: relative; width: 48px; height: 26px; }
    .tg input { opacity: 0; width: 0; height: 0; }
    .sl {
      position: absolute; cursor: pointer; inset: 0;
      background: rgba(255,255,255,0.1); border-radius: 26px;
      transition: .3s cubic-bezier(0.4, 0.0, 0.2, 1);
      border: 1px solid rgba(255,255,255,0.1);
    }
    .sl:before {
      content: ""; position: absolute; height: 18px; width: 18px;
      left: 3px; bottom: 3px; background: #fff;
      border-radius: 50%; transition: .3s cubic-bezier(0.4, 0.0, 0.2, 1);
      box-shadow: 0 2px 4px rgba(0,0,0,0.2);
    }
    .tg input:checked + .sl { background: var(--primary); border-color: var(--primary); }
    .tg input:checked + .sl:before { transform: translateX(22px); }

    .btn-group { display: flex; gap: 12px; margin-top: 8px; }
    .btn {
      flex: 1; padding: 10px 16px;
      border: none; border-radius: var(--radius-sm);
      font-size: 0.875rem; font-weight: 600;
      color: #fff; cursor: pointer;
      background: rgba(255,255,255,0.05);
      border: 1px solid rgba(255,255,255,0.1);
      transition: all 0.2s;
      display: flex; align-items: center; justify-content: center; gap: 6px;
    }
    .btn:hover:not(:disabled) { background: rgba(255,255,255,0.1); transform: translateY(-1px); }
    .btn:active:not(:disabled) { transform: translateY(1px); }
    .btn:disabled { opacity: 0.5; cursor: not-allowed; }
    .btn-primary {
      background: var(--primary);
      border: none; box-shadow: 0 4px 12px rgba(99,102,241,0.3);
    }
    .btn-primary:hover:not(:disabled) { background: var(--primary-hover); box-shadow: 0 6px 16px rgba(99,102,241,0.4); }
    .btn-danger {
      background: rgba(239,68,68,0.1); color: var(--danger); border-color: rgba(239,68,68,0.2);
    }
    .btn-danger:hover:not(:disabled) { background: rgba(239,68,68,0.2); }

    /* Stats Ribbon */
    .stats-ribbon {
      display: flex; flex-wrap: wrap; gap: 12px;
      padding: 12px 16px; border-radius: var(--radius);
      margin-top: 16px; font-size: 0.75rem;
      justify-content: space-around;
      border: 1px solid var(--glass-border);
    }
    .stat-item { display: flex; flex-direction: column; align-items: center; gap: 4px; }
    .stat-label { color: var(--text-muted); text-transform: uppercase; letter-spacing: 0.5px; font-size: 0.65rem; }
    .stat-val { font-weight: 700; font-size: 0.9rem; }
    .val-good { color: var(--success); }
    .val-warn { color: var(--warning); }
    .val-bad { color: var(--danger); }
    
    /* Status Badge */
    .status-badge {
      display: inline-block; padding: 4px 10px; border-radius: 20px;
      font-size: 0.75rem; font-weight: 600; text-align: center;
      background: rgba(255,255,255,0.05); color: var(--text-muted);
      border: 1px solid rgba(255,255,255,0.1);
      margin-top: 12px; width: fit-content; align-self: center;
    }
    .status-badge.ok { background: rgba(16,185,129,0.1); color: var(--success); border-color: rgba(16,185,129,0.2); }
    .status-badge.er { background: rgba(239,68,68,0.1); color: var(--danger); border-color: rgba(239,68,68,0.2); }

    /* Gallery */
    .toolbar {
      display: flex; gap: 10px; margin-bottom: 20px; flex-wrap: wrap;
      align-items: center; padding: 12px; border-radius: var(--radius);
    }
    .gal-info { font-size: 0.875rem; color: var(--text-muted); margin-left: auto; }
    
    .gallery-grid {
      display: grid; grid-template-columns: repeat(auto-fill, minmax(140px, 1fr)); gap: 16px;
    }
    .thumb {
      position: relative; aspect-ratio: 4/3; border-radius: var(--radius-sm);
      overflow: hidden; cursor: pointer; border: 2px solid transparent;
      box-shadow: 0 4px 12px rgba(0,0,0,0.2);
      transition: transform 0.2s, border-color 0.2s;
    }
    .thumb:hover { transform: translateY(-4px); box-shadow: 0 8px 16px rgba(0,0,0,0.4); }
    .thumb img { width: 100%; height: 100%; object-fit: cover; }
    .thumb-info {
      position: absolute; bottom: 0; left: 0; right: 0;
      padding: 6px; background: rgba(0,0,0,0.7);
      font-size: 0.7rem; color: #fff; pointer-events: none;
    }
    .thumb .chk { position: absolute; top: 8px; left: 8px; width: 18px; height: 18px; display: none; accent-color: var(--primary); cursor: pointer; }
    .sel-mode .thumb .chk { display: block; }
    .thumb.selected { border-color: var(--primary); transform: scale(0.96); }
    .thumb.selected:hover { transform: scale(0.96); }

    .empty-state {
      padding: 40px; text-align: center; color: var(--text-muted);
      border: 1px dashed rgba(255,255,255,0.1); border-radius: var(--radius);
    }

    /* Modal */
    .modal {
      display: none; position: fixed; inset: 0; background: rgba(0,0,0,0.85);
      backdrop-filter: blur(4px); z-index: 100;
      align-items: center; justify-content: center; flex-direction: column;
      opacity: 0; transition: opacity 0.3s ease;
    }
    .modal.active { display: flex; opacity: 1; }
    .modal-content {
      position: relative; max-width: 90%; max-height: 80vh;
      border-radius: var(--radius); overflow: hidden;
      box-shadow: 0 20px 40px rgba(0,0,0,0.5);
    }
    .modal img { max-width: 100%; max-height: 80vh; display: block; }
    .close-btn {
      position: absolute; top: 16px; right: 16px; width: 32px; height: 32px;
      background: rgba(0,0,0,0.5); border: none; color: white; border-radius: 50%;
      font-size: 20px; cursor: pointer; display: flex; align-items: center; justify-content: center;
      transition: background 0.2s;
    }
    .close-btn:hover { background: rgba(0,0,0,0.8); }
    .modal-actions {
      display: flex; gap: 12px; margin-top: 20px;
    }
  </style>
</head>
<body>
  <header>
    <h1>ESP32-S3 Camera</h1>
    <div class="tabs">
      <div class="tab active" onclick="showPage('live')">Live</div>
      <div class="tab" onclick="showPage('gal')">Gallery</div>
    </div>
  </header>

  <div class="container">
    <div class="pg active" id="pg-live">
      <div class="live-layout">
        <div class="stream-container glass">
          <img id="stream" src="" alt="Camera Stream Disconnected">
        </div>
        
        <div class="controls glass">
          <div class="control-group">
            <label class="title">Resolution</label>
            <select id="fs" onchange="setFS(this.value)">
              <option value="5">QVGA (320x240)</option>
              <option value="6">CIF (400x296)</option>
              <option value="8" selected>VGA (640x480)</option>
              <option value="9">SVGA (800x600)</option>
              <option value="10">XGA (1024x768)</option>
            </select>
          </div>
          <div class="control-group">
            <label class="title">Grayscale</label>
            <label class="tg"><input type="checkbox" id="gs" onchange="setGS(this.checked)"><span class="sl"></span></label>
          </div>
          <div class="control-group">
            <label class="title">Flip Vertical</label>
            <label class="tg"><input type="checkbox" id="vf" onchange="setVF(this.checked)"><span class="sl"></span></label>
          </div>
          <div class="control-group">
            <label class="title">Mirror Horizontal</label>
            <label class="tg"><input type="checkbox" id="hm" onchange="setHM(this.checked)"><span class="sl"></span></label>
          </div>
          <div class="control-group">
            <label class="title">Upload to Cloud</label>
            <label class="tg"><input type="checkbox" id="upl"><span class="sl"></span></label>
          </div>
          
          <div class="btn-group">
            <button class="btn btn-primary" onclick="capture()">
              <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2z"></path><circle cx="12" cy="13" r="4"></circle></svg>
              Capture
            </button>
            <button class="btn" onclick="startStream()">Reload</button>
          </div>
          
          <div class="status-badge" id="st">Ready</div>

          <div class="stats-ribbon">
            <div class="stat-item"><span class="stat-label">FPS</span><span class="stat-val" id="s-fps">--</span></div>
            <div class="stat-item"><span class="stat-label">Size</span><span class="stat-val"><span id="s-frame">--</span> KB</span></div>
            <div class="stat-item"><span class="stat-label">Temp</span><span class="stat-val" id="s-temp">--&deg;C</span></div>
            <div class="stat-item"><span class="stat-label">WiFi</span><span class="stat-val" id="s-rssi">-- dBm</span></div>
          </div>
        </div>
      </div>
    </div>

    <div class="pg" id="pg-gal">
      <div class="toolbar glass">
        <button class="btn" onclick="loadGal()">Refresh</button>
        <button class="btn" id="selBtn" onclick="toggleSel()">Select</button>
        <button class="btn" id="saBtn" onclick="selAll()" style="display:none">All</button>
        <button class="btn btn-primary" id="dlAllBtn" onclick="dlAll()" style="display:none">Download</button>
        <button class="btn btn-primary" id="zipBtn" onclick="dlZip()" style="display:none">ZIP</button>
        <button class="btn btn-danger" id="rmAllBtn" onclick="rmAll()" style="display:none">Delete</button>
        <div class="gal-info" id="fc"></div>
      </div>
      <div id="prog" style="color:var(--text-muted); font-size: 0.875rem; margin-bottom: 12px; text-align: center;"></div>
      <div class="gallery-grid" id="gal"></div>
    </div>
  </div>

  <div class="modal" id="md" onclick="closeMd(event)">
    <div class="modal-content">
      <button class="close-btn" onclick="closeMd()">&times;</button>
      <img id="md-img" src="">
    </div>
    <div style="color:#aaa; font-size:13px; margin-top:12px;" id="md-info"></div>
    <div class="modal-actions">
      <button class="btn btn-primary" onclick="dlFile()">Download</button>
      <button class="btn btn-danger" onclick="rmFile()">Delete</button>
    </div>
  </div>

  <script>
    var STREAM_URL='http://'+location.hostname+':81/stream';
    var si=document.getElementById('stream');
    var stEl=document.getElementById('st');
    var curFile='';
    var selMode=false;
    var galFiles=[];

    function setSt(m,t){stEl.textContent=m;stEl.className='status-badge'+(t?' '+t:'')}
    function setProg(m){document.getElementById('prog').textContent=m}

    function showPage(p){
      document.querySelectorAll('.pg').forEach(function(e){e.classList.remove('active')});
      document.querySelectorAll('.tab').forEach(function(e){e.classList.remove('active')});
      document.getElementById('pg-'+p).classList.add('active');
      var tabs=document.querySelectorAll('.tab');
      if(p==='live'){tabs[0].classList.add('active');startStream();}
      else{tabs[1].classList.add('active');stopStream();loadGal();}
    }

    function stopStream(){si.src=''}
    function startStream(){
      si.src=STREAM_URL+'?'+Date.now();setSt('Streaming','ok');
    }

    var statsTimer=null;
    function startStats(){
      if(statsTimer)return;
      statsTimer=setInterval(function(){
        fetch('/stats').then(function(r){return r.json()}).then(function(d){
          var fpsEl=document.getElementById('s-fps');
          var frameEl=document.getElementById('s-frame');
          var tempEl=document.getElementById('s-temp');
          var rssiEl=document.getElementById('s-rssi');
          
          fpsEl.textContent=d.fps.toFixed(1);
          frameEl.textContent=d.frameKB.toFixed(1);
          
          if (d.temp < 0) {
            tempEl.innerHTML='Err';
            tempEl.className='stat-val val-warn';
          } else {
            tempEl.innerHTML=d.temp.toFixed(1)+'&deg;C';
            tempEl.className='stat-val'+(d.temp<70?' val-good':d.temp<80?' val-warn':' val-bad');
          }
          rssiEl.textContent=d.rssi+' dBm';
          
          fpsEl.className='stat-val'+(d.fps>=15?' val-good':d.fps>=8?' val-warn':' val-bad');
          rssiEl.className='stat-val'+(d.rssi>=-60?' val-good':d.rssi>=-80?' val-warn':' val-bad');
        }).catch(function(){});
      },1000);
    }
    function stopStats(){if(statsTimer){clearInterval(statsTimer);statsTimer=null;}}
    startStats();

    function sendSet(body){
      setSt('Applying...');stopStream();
      var x=new XMLHttpRequest();x.open('POST','/settings',true);
      x.onload=function(){if(x.status===200){setSt('Applied','ok');setTimeout(startStream,300);}else setSt('Error','er')};
      x.onerror=function(){setSt('Error','er')};x.send(body);
    }
    function setFS(v){sendSet('framesize='+v)}
    function setGS(on){sendSet('grayscale='+(on?1:0))}
    function setVF(on){sendSet('vflip='+(on?1:0))}
    function setHM(on){sendSet('hmirror='+(on?1:0))}

    function capture(){
      var upl=document.getElementById('upl').checked;
      setSt('Saving...');
      fetch('/capture?upload='+(upl?1:0)+'&'+Date.now()).then(function(r){
        if(r.ok){
          if(upl)setSt('Saved & Cloud','ok');
          else setSt('Saved to SD','ok');
        }
        else setSt('Failed','er');
      }).catch(function(){setSt('Failed','er')});
    }

    function toggleSel(){
      selMode=!selMode;
      var g=document.getElementById('gal');
      if(selMode){g.classList.add('sel-mode');document.getElementById('selBtn').textContent='Cancel';}
      else{
        g.classList.remove('sel-mode');document.getElementById('selBtn').textContent='Select';
        g.querySelectorAll('.chk').forEach(function(c){c.checked=false});
        g.querySelectorAll('.thumb').forEach(function(t){t.classList.remove('selected')});
      }
      updateSelUI();
    }
    function updateSelUI(){
      var n=getSelNames().length;
      document.getElementById('saBtn').style.display=selMode?'':'none';
      document.getElementById('dlAllBtn').style.display=(selMode&&n>0)?'':'none';
      document.getElementById('zipBtn').style.display=(selMode&&n>0)?'':'none';
      document.getElementById('rmAllBtn').style.display=(selMode&&n>0)?'':'none';
      document.getElementById('fc').textContent = selMode ? (n>0 ? n+' selected' : '0 selected') : galFiles.length+' images';
    }
    function selAll(){
      var g=document.getElementById('gal');var all=g.querySelectorAll('.chk');
      var allChecked=true;all.forEach(function(c){if(!c.checked)allChecked=false});
      all.forEach(function(c){c.checked=!allChecked;c.parentElement.classList.toggle('selected',!allChecked)});
      updateSelUI();
    }
    function getSelNames(){
      var names=[];document.querySelectorAll('.gallery-grid .chk:checked').forEach(function(c){names.push(c.dataset.name)});return names;
    }
    function onChk(el){el.parentElement.classList.toggle('selected',el.checked);updateSelUI()}
    function thClick(name,size,el){
      if(selMode){var c=el.querySelector('.chk');c.checked=!c.checked;onChk(c);}
      else openMd(name,size);
    }

    function loadGal(){
      var g=document.getElementById('gal');var fc=document.getElementById('fc');
      g.innerHTML='<div class="empty-state">Loading...</div>';setProg('');
      fetch('/files').then(function(r){return r.json()}).then(function(d){
        galFiles=d.files||[];
        if(galFiles.length===0){g.innerHTML='<div class="empty-state">No images on SD card</div>';fc.textContent='0 images';return;}
        galFiles.sort(function(a,b){return b.name.localeCompare(a.name)});
        fc.textContent=galFiles.length+' images';
        var h='';
        galFiles.forEach(function(f){
          var kb=Math.round(f.size/1024);
          h+='<div class="thumb" onclick="thClick(\''+f.name+'\','+f.size+',this)">';
          h+='<input type="checkbox" class="chk" data-name="'+f.name+'" onclick="event.stopPropagation();onChk(this)">';
          h+='<img data-src="/sd/'+f.name+'" alt="loading...">';
          h+='<div class="thumb-info">'+f.name+'<br>'+kb+'KB</div>';
          h+='</div>';
        });
        g.innerHTML=h;
        if(selMode)g.classList.add('sel-mode');
        updateSelUI();

        if('IntersectionObserver' in window){
          var obs=new IntersectionObserver(function(es,o){
            es.forEach(function(e){
              if(e.isIntersecting){var i=e.target;i.src=i.dataset.src;o.unobserve(i);}
            });
          },{rootMargin:'200px'});
          g.querySelectorAll('img[data-src]').forEach(function(i){obs.observe(i);});
        } else {
          g.querySelectorAll('img[data-src]').forEach(function(i){i.src=i.dataset.src;});
        }
      }).catch(function(e){
        g.innerHTML='<div class="empty-state">Failed to load</div>';
      });
    }

    function dlAll(){
      var names=getSelNames();if(names.length===0)return;
      var i=0;setProg('Downloading 0/'+names.length);
      function next(){
        if(i>=names.length){setProg('Download complete ('+names.length+')');return;}
        var a=document.createElement('a');a.href='/sd/'+names[i];a.download=names[i];a.click();
        i++;setProg('Downloading '+i+'/'+names.length);setTimeout(next,500);
      }
      next();
    }
    function dlZip(){
      var names=getSelNames();if(names.length===0)return;
      if(typeof JSZip==='undefined'){alert('JSZip library not loaded');return;}
      var zipBtn=document.getElementById('zipBtn');
      zipBtn.disabled=true;
      zipBtn.textContent='Processing...';
      setProg('Creating ZIP: 0/'+names.length);
      var zip=new JSZip();
      var i=0;
      function fetchNext(){
        if(i>=names.length){
          setProg('Generating ZIP file...');
          zip.generateAsync({type:'blob'}).then(function(blob){
            var a=document.createElement('a');
            a.href=URL.createObjectURL(blob);
            a.download='images_'+new Date().toISOString().replace(/[:.]/g,'-').slice(0,15)+'.zip';
            a.click();
            setProg('ZIP download complete');
            URL.revokeObjectURL(a.href);
            zipBtn.disabled=false;
            zipBtn.textContent='ZIP';
          }).catch(function(e){
            setProg('ZIP failed: '+e);
            zipBtn.disabled=false;
            zipBtn.textContent='ZIP';
          });
          return;
        }
        fetch('/sd/'+names[i]).then(function(r){return r.blob()}).then(function(blob){
          zip.file(names[i],blob);
          i++;setProg('Creating ZIP: '+i+'/'+names.length);
          fetchNext();
        }).catch(function(e){
          i++;setProg('Skipped '+names[i-1]);
          setTimeout(fetchNext,100);
        });
      }
      fetchNext();
    }
    function rmAll(){
      var names=getSelNames();if(names.length===0)return;
      if(!confirm('WARNING: '+names.length+' images will be deleted.\n\nAre you sure?'))return;
      var i=0,ok=0;setProg('Deleting 0/'+names.length);
      function next(){
        if(i>=names.length){setProg('Deleted '+ok+'/'+names.length);loadGal();return;}
        var x=new XMLHttpRequest();x.open('POST','/delete',true);var n=names[i];
        x.onload=function(){ok++;i++;setProg('Deleting '+i+'/'+names.length);setTimeout(next,100)};
        x.onerror=function(){i++;setTimeout(next,100)};x.send('file=/'+n);
      }
      next();
    }

    function openMd(name,size){
      curFile=name;document.getElementById('md-img').src='/sd/'+name;
      document.getElementById('md-info').textContent=name+' ('+Math.round(size/1024)+' KB)';
      document.getElementById('md').classList.add('active');
    }
    function closeMd(e){
      if(!e||e.target===document.getElementById('md')||e.target.classList.contains('close-btn'))
        document.getElementById('md').classList.remove('active');
    }
    function dlFile(){var a=document.createElement('a');a.href='/sd/'+curFile;a.download=curFile;a.click()}
    function rmFile(){
      if(!confirm('Delete "'+curFile+'"?'))return;
      var x=new XMLHttpRequest();x.open('POST','/delete',true);
      x.onload=function(){closeMd();loadGal()};x.send('file=/'+curFile);
    }

    fetch('/status').then(function(r){return r.json()}).then(function(d){
      document.getElementById('fs').value=d.framesize;
      document.getElementById('gs').checked=d.grayscale===1;
      document.getElementById('vf').checked=d.vflip===1;
      document.getElementById('hm').checked=d.hmirror===1;
    }).catch(function(){});
    startStream();
  </script>
</body>
</html>
)rawliteral";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

// ────────────────────────────────────────
// サーバー起動（2ポート構成）
// ────────────────────────────────────────
void startCameraServer() {
  // ポート80: API/ページ用サーバー
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 12;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.stack_size = 16384;  // アップロード処理用にスタックサイズ拡大

  httpd_uri_t index_uri = {.uri = "/",
                           .method = HTTP_GET,
                           .handler = index_handler,
                           .user_ctx = NULL};
  httpd_uri_t capture_uri = {.uri = "/capture",
                             .method = HTTP_GET,
                             .handler = capture_handler,
                             .user_ctx = NULL};
  httpd_uri_t settings_uri = {.uri = "/settings",
                              .method = HTTP_POST,
                              .handler = settings_handler,
                              .user_ctx = NULL};
  httpd_uri_t status_uri = {.uri = "/status",
                            .method = HTTP_GET,
                            .handler = status_handler,
                            .user_ctx = NULL};
  httpd_uri_t files_uri = {.uri = "/files",
                           .method = HTTP_GET,
                           .handler = files_handler,
                           .user_ctx = NULL};
  httpd_uri_t sdfile_uri = {.uri = "/sd/*",
                            .method = HTTP_GET,
                            .handler = sdfile_handler,
                            .user_ctx = NULL};
  httpd_uri_t delete_uri = {.uri = "/delete",
                            .method = HTTP_POST,
                            .handler = delete_handler,
                            .user_ctx = NULL};
  httpd_uri_t stats_uri = {.uri = "/stats",
                           .method = HTTP_GET,
                           .handler = stats_handler,
                           .user_ctx = NULL};

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &settings_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &files_uri);
    httpd_register_uri_handler(camera_httpd, &sdfile_uri);
    httpd_register_uri_handler(camera_httpd, &delete_uri);
    httpd_register_uri_handler(camera_httpd, &stats_uri);
  }

  // ポート81: ストリーム専用サーバー（スタックサイズ拡大）
  httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
  stream_config.server_port = 81;
  stream_config.ctrl_port = 32769;
  stream_config.max_uri_handlers = 1;
  stream_config.stack_size = 8192;

  httpd_uri_t stream_uri = {.uri = "/stream",
                            .method = HTTP_GET,
                            .handler = stream_handler,
                            .user_ctx = NULL};

  if (httpd_start(&stream_httpd, &stream_config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}
