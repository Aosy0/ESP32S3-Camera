#include "esp_camera.h"
#include "esp_http_server.h"
#include "img_converters.h"
#include <Arduino.h>


#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t camera_httpd = NULL;

// 現在のグレースケール状態
static bool grayscaleMode = false;

// ストリームハンドラ
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t *_jpg_buf = NULL;
  char part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK)
    return res;

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      if (fb->format != PIXFORMAT_JPEG) {
        bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
        esp_camera_fb_return(fb);
        fb = NULL;
        if (!jpeg_converted) {
          Serial.println("JPEG compression failed");
          res = ESP_FAIL;
        }
      } else {
        _jpg_buf_len = fb->len;
        _jpg_buf = fb->buf;
      }
    }
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY,
                                  strlen(_STREAM_BOUNDARY));
    }
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
  }
  return res;
}

// 静止画キャプチャハンドラ
static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition",
                     "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

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
  esp_camera_fb_return(fb);
  Serial.println("Photo captured via web");
  return res;
}

// 設定変更APIハンドラ
static esp_err_t settings_handler(httpd_req_t *req) {
  char buf[128];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  buf[ret] = '\0';
  Serial.printf("Settings request: %s\n", buf);

  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  char *p;

  p = strstr(buf, "framesize=");
  if (p) {
    int val = atoi(p + 10);
    s->set_framesize(s, (framesize_t)val);
    Serial.printf("Frame size changed to: %d\n", val);
  }

  p = strstr(buf, "grayscale=");
  if (p) {
    int val = atoi(p + 10);
    if (val) {
      s->set_special_effect(s, 2); // Grayscale
      grayscaleMode = true;
      Serial.println("Grayscale mode: ON");
    } else {
      s->set_special_effect(s, 0); // Normal
      grayscaleMode = false;
      Serial.println("Grayscale mode: OFF");
    }
  }

  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

// 現在の設定取得APIハンドラ
static esp_err_t status_handler(httpd_req_t *req) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  char json[128];
  snprintf(json, sizeof(json), "{\"framesize\":%d,\"grayscale\":%d}",
           s->status.framesize, grayscaleMode ? 1 : 0);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_sendstr(req, json);
  return ESP_OK;
}

// WebページHTML
static const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32-S3 Camera</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: 'Segoe UI', Arial, sans-serif;
      background: #121212;
      color: #e0e0e0;
      min-height: 100vh;
    }
    .header {
      background: #1e1e1e;
      padding: 16px 24px;
      border-bottom: 1px solid #333;
    }
    .header h1 {
      font-size: 18px;
      font-weight: 600;
      color: #90caf9;
    }
    .main {
      display: flex;
      flex-direction: column;
      align-items: center;
      padding: 16px;
      gap: 16px;
    }
    #stream-container {
      position: relative;
      width: 640px;
      max-width: 100%;
    }
    #stream {
      width: 100%;
      height: auto;
      border: 2px solid #333;
      border-radius: 8px;
      background: #000;
      display: block;
    }
    #capture-preview {
      width: 100%;
      height: auto;
      border: 2px solid #4caf50;
      border-radius: 8px;
      background: #000;
      display: none;
    }
    .controls {
      width: 100%;
      max-width: 640px;
      display: flex;
      flex-direction: column;
      gap: 12px;
    }
    .control-group {
      background: #1e1e1e;
      border-radius: 8px;
      padding: 16px;
      border: 1px solid #333;
    }
    .control-group label {
      display: block;
      font-size: 13px;
      color: #90caf9;
      margin-bottom: 8px;
      font-weight: 600;
    }
    select {
      width: 100%;
      padding: 10px 12px;
      background: #2a2a2a;
      color: #e0e0e0;
      border: 1px solid #444;
      border-radius: 6px;
      font-size: 14px;
      cursor: pointer;
      outline: none;
    }
    select:focus { border-color: #90caf9; }
    .toggle-row {
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    .toggle-label { font-size: 14px; }
    .toggle {
      position: relative;
      width: 48px;
      height: 26px;
    }
    .toggle input { opacity: 0; width: 0; height: 0; }
    .slider {
      position: absolute;
      cursor: pointer;
      top: 0; left: 0; right: 0; bottom: 0;
      background: #444;
      border-radius: 26px;
      transition: 0.3s;
    }
    .slider:before {
      content: "";
      position: absolute;
      height: 20px; width: 20px;
      left: 3px; bottom: 3px;
      background: #fff;
      border-radius: 50%;
      transition: 0.3s;
    }
    .toggle input:checked + .slider { background: #90caf9; }
    .toggle input:checked + .slider:before { transform: translateX(22px); }
    .btn-row { display: flex; gap: 8px; }
    .btn {
      flex: 1;
      padding: 10px;
      border: 1px solid #444;
      border-radius: 6px;
      background: #2a2a2a;
      color: #e0e0e0;
      font-size: 14px;
      cursor: pointer;
      transition: background 0.2s;
    }
    .btn:hover { background: #383838; }
    .btn:active { background: #444; }
    .btn-save {
      background: #1b5e20;
      border-color: #2e7d32;
    }
    .btn-save:hover { background: #2e7d32; }
    .status {
      font-size: 12px;
      color: #888;
      text-align: center;
      min-height: 18px;
    }
    .status.ok { color: #4caf50; }
    .status.error { color: #f44336; }
  </style>
</head>
<body>
  <div class="header">
    <h1>ESP32-S3 Camera</h1>
  </div>
  <div class="main">
    <div id="stream-container">
      <img id="stream" src="" alt="Camera Stream">
      <img id="capture-preview" alt="Captured Photo">
    </div>
    <div class="controls">
      <div class="control-group">
        <label>Resolution</label>
        <select id="framesize" onchange="setFrameSize(this.value)">
          <option value="5">QVGA (320x240)</option>
          <option value="8" selected>VGA (640x480)</option>
          <option value="9">SVGA (800x600)</option>
          <option value="11">HD (1280x720)</option>
          <option value="13">UXGA (1600x1200)</option>
        </select>
      </div>
      <div class="control-group">
        <div class="toggle-row">
          <span class="toggle-label">Grayscale</span>
          <label class="toggle">
            <input type="checkbox" id="grayscale" onchange="setGrayscale(this.checked)">
            <span class="slider"></span>
          </label>
        </div>
      </div>
      <div class="btn-row">
        <button class="btn" onclick="capturePhoto()">Capture Photo</button>
        <button class="btn" onclick="startStream()">Reload Stream</button>
        <button class="btn btn-save" id="saveBtn" onclick="saveCapture()" style="display:none">Save Photo</button>
      </div>
      <div class="status" id="status">Ready</div>
    </div>
  </div>
  <script>
    var streamImg = document.getElementById('stream');
    var previewImg = document.getElementById('capture-preview');
    var statusEl = document.getElementById('status');
    var saveBtn = document.getElementById('saveBtn');

    function setStatus(msg, type) {
      statusEl.textContent = msg;
      statusEl.className = 'status' + (type ? ' ' + type : '');
    }

    function startStream() {
      previewImg.style.display = 'none';
      streamImg.style.display = 'block';
      saveBtn.style.display = 'none';
      streamImg.src = '/stream?' + Date.now();
      setStatus('Streaming', 'ok');
    }

    function sendSetting(body) {
      setStatus('Applying...');
      var xhr = new XMLHttpRequest();
      xhr.open('POST', '/settings', true);
      xhr.onload = function() {
        if (xhr.status === 200) {
          setStatus('Applied', 'ok');
          // 設定変更後にストリームを再接続
          setTimeout(function() { startStream(); }, 500);
        } else {
          setStatus('Error: ' + xhr.status, 'error');
        }
      };
      xhr.onerror = function() {
        setStatus('Connection error', 'error');
      };
      xhr.send(body);
    }

    function setFrameSize(val) { sendSetting('framesize=' + val); }
    function setGrayscale(on) { sendSetting('grayscale=' + (on ? 1 : 0)); }

    function capturePhoto() {
      setStatus('Capturing...');
      // ストリームを一旦停止
      streamImg.src = '';
      streamImg.style.display = 'none';

      // キャプチャ画像を取得
      previewImg.src = '/capture?' + Date.now();
      previewImg.style.display = 'block';
      previewImg.onload = function() {
        setStatus('Photo captured', 'ok');
        saveBtn.style.display = 'block';
      };
      previewImg.onerror = function() {
        setStatus('Capture failed', 'error');
        startStream();
      };
    }

    function saveCapture() {
      var a = document.createElement('a');
      a.href = '/capture?' + Date.now();
      a.download = 'capture_' + new Date().toISOString().replace(/[:.]/g, '-') + '.jpg';
      a.click();
      setStatus('Photo saved', 'ok');
    }

    // 起動時に設定を取得してストリーム開始
    fetch('/status')
      .then(function(r) { return r.json(); })
      .then(function(d) {
        document.getElementById('framesize').value = d.framesize;
        document.getElementById('grayscale').checked = d.grayscale === 1;
      })
      .catch(function() {});

    startStream();
  </script>
</body>
</html>
)rawliteral";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 8;

  httpd_uri_t index_uri = {.uri = "/",
                           .method = HTTP_GET,
                           .handler = index_handler,
                           .user_ctx = NULL};
  httpd_uri_t stream_uri = {.uri = "/stream",
                            .method = HTTP_GET,
                            .handler = stream_handler,
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

  Serial.printf("Starting web server on port: '%d'\n", config.server_port);
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &stream_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &settings_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
  }

  Serial.println("Camera Server started successfully!");
}
