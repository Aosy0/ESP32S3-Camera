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

// ストリーム統計情報
static float stream_fps = 0;
static size_t stream_frame_size = 0;
static bool stream_active = false;

// main.cppの関数/変数を参照
extern bool saveImageToSD(camera_fb_t *fb);
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

    // 統計情報更新
    stream_frame_size = _jpg_buf_len;
    fps_count++;
    unsigned long now = millis();
    if (now - fps_start >= 1000) {
      stream_fps = (float)fps_count * 1000.0f / (float)(now - fps_start);
      fps_count = 0;
      fps_start = now;
    }

    // フレームレート制御（WiFiバッファ輻輳防止）
    unsigned long frame_time = millis() - frame_start;
    if (frame_time < STREAM_MIN_FRAME_MS) {
      delay(STREAM_MIN_FRAME_MS - frame_time);
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
    if (saveImageToSD(fb))
      imageCount++;
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
  snprintf(json, sizeof(json), "{\"framesize\":%d,\"grayscale\":%d}",
           s->status.framesize, grayscaleMode ? 1 : 0);
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
  char buf[128];
  int rssi = WiFi.RSSI();
  snprintf(buf, sizeof(buf),
           "{\"fps\":%.1f,\"frameKB\":%.1f,\"rssi\":%d,\"streaming\":%s}",
           stream_fps, stream_frame_size / 1024.0f, rssi,
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
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32-S3 Camera</title>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/jszip/3.10.1/jszip.min.js"></script>
  <style>
    :root{--bg:#121212;--card:#1e1e1e;--text:#e0e0e0;--accent:#90caf9;--accent-hover:#42a5f5;--border:#333;--danger:#ef5350}
    *{box-sizing:border-box;margin:0;padding:0}
    body{font-family:'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--text);height:100vh;display:flex;flex-direction:column;overflow:hidden}
    
    /* Layout */
    .main-grid{display:grid;grid-template-rows:1fr auto;height:100%;max-width:800px;margin:0 auto;width:100%}
    .stream-area{position:relative;background:#000;display:flex;align-items:center;justify-content:center;overflow:hidden;width:100%;height:100%}
    #stream{max-width:100%;max-height:100%;object-fit:contain}
    
    .controls{background:var(--card);padding:20px;border-top:1px solid var(--border);display:flex;flex-direction:column;gap:16px;z-index:10}
    
    /* Components */
    .header{display:flex;justify-content:space-between;align-items:center}
    h1{font-size:18px;font-weight:600;color:var(--accent);margin:0}
    .status-badge{font-size:12px;padding:4px 8px;border-radius:12px;background:#333;color:#888}
    .status-badge.ok{background:#1b5e20;color:#a5d6a7}
    .status-badge.er{background:#b71c1c;color:#ef9a9a}
    
    .action-bar{display:grid;grid-template-columns:1fr 1fr auto;gap:12px}
    .btn{border:none;border-radius:8px;padding:12px;font-size:14px;font-weight:600;cursor:pointer;transition:.2s;display:flex;align-items:center;justify-content:center;gap:6px;background:#333;color:var(--text)}
    .btn:hover{background:#444}
    .btn:active{transform:scale(0.98)}
    .btn-primary{background:var(--accent);color:#0d47a1}
    .btn-primary:hover{background:var(--accent-hover)}
    .btn-icon{padding:12px;width:48px}
    
    .settings-row{display:flex;gap:12px;overflow-x:auto;padding-bottom:4px}
    select{background:#2a2a2a;color:var(--text);border:1px solid var(--border);padding:8px 12px;border-radius:6px;font-size:14px;outline:none}
    
    .stats-row{display:flex;gap:16px;font-size:12px;color:#888;align-items:center}
    .stat-item{display:flex;gap:4px}
    .stat-val{color:var(--text);font-weight:600}
    
    /* Toggle Switch */
    .toggle{display:flex;align-items:center;gap:8px;cursor:pointer;font-size:14px}
    .toggle input{display:none}
    .slider{width:36px;height:20px;background:#444;border-radius:20px;position:relative;transition:.3s}
    .slider:before{content:"";position:absolute;height:16px;width:16px;left:2px;bottom:2px;background:#fff;border-radius:50%;transition:.3s}
    input:checked+.slider{background:var(--accent)}
    input:checked+.slider:before{transform:translateX(16px)}
    
    /* Gallery Modal */
    .modal{position:fixed;inset:0;background:rgba(0,0,0,0.95);z-index:100;display:none;flex-direction:column}
    .modal.active{display:flex}
    .modal-header{padding:16px;background:var(--card);display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid var(--border)}
    .modal-body{flex:1;overflow-y:auto;padding:16px}
    .gal-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:8px}
    .gal-item{aspect-ratio:4/3;background:#000;border-radius:4px;overflow:hidden;position:relative;cursor:pointer;border:1px solid #333}
    .gal-item img{width:100%;height:100%;object-fit:cover;transition:.2s}
    .gal-item:hover{border-color:var(--accent)}
    .gal-item.selected{border:2px solid var(--accent)}
    .gal-chk{position:absolute;top:4px;left:4px;width:18px;height:18px;accent-color:var(--accent);display:none}
    .gal-grid.select-mode .gal-chk{display:block}
    
    /* Media Query for PC */
    @media (min-width: 768px) {
      .main-grid{grid-template-rows:1fr;grid-template-columns:1fr 320px;max-width:1200px}
      .controls{border-top:none;border-left:1px solid var(--border);height:100%}
      .stream-area{border-right:1px solid var(--border)}
    }
  </style>
</head>
<body>
  <div class="main-grid">
    <div class="stream-area">
      <img id="stream" src="" alt="Live Stream">
    </div>
    
    <div class="controls">
      <div class="header">
        <h1>ESP32-S3 Camera</h1>
        <div id="st" class="status-badge">Ready</div>
      </div>

      <div class="action-bar">
        <button class="btn btn-primary" onclick="capture()">
          <span>Capture</span>
        </button>
        <button class="btn" onclick="startStream()">
          <span>Reload</span>
        </button>
        <button class="btn btn-icon" onclick="openGal()" title="Gallery">
          <svg style="width:24px;height:24px" viewBox="0 0 24 24"><path fill="currentColor" d="M22,16V4A2,2 0 0,0 20,2H8A2,2 0 0,0 6,4V16A2,2 0 0,0 8,18H20A2,2 0 0,0 22,16M11,12L13.03,14.71L16,11L20,16H8M2,6V20A2,2 0 0,0 4,22H18V20H4V6"></path></svg>
        </button>
      </div>

      <div class="settings-row">
        <select id="fs" onchange="setFS(this.value)">
          <option value="5">QVGA (320x240)</option>
          <option value="6">CIF (400x296)</option>
          <option value="8" selected>VGA (640x480)</option>
          <option value="9">SVGA (800x600)</option>
          <option value="10">XGA (1024x768)</option>
          <option value="11">HD (1280x720)</option>
        </select>
        
        <label class="toggle">
          <input type="checkbox" id="gs" onchange="setGS(this.checked)">
          <span class="slider"></span>
          <span>Grayscale</span>
        </label>
      </div>

      <div style="flex:1"></div> <!-- Spacer -->

      <div class="stats-row">
        <div class="stat-item">FPS: <span class="stat-val" id="s-fps">--</span></div>
        <div class="stat-item">KB: <span class="stat-val" id="s-frame">--</span></div>
        <div class="stat-item">WiFi: <span class="stat-val" id="s-rssi">--</span></div>
      </div>
    </div>
  </div>

  <!-- Gallery Modal -->
  <div class="modal" id="galModal">
    <div class="modal-header">
      <div style="display:flex;gap:8px;align-items:center">
        <h2>Gallery</h2>
        <span id="galCount" style="font-size:12px;color:#888"></span>
      </div>
      <div>
        <button class="btn" onclick="closeGal()">Close</button>
      </div>
    </div>
    
    <div style="padding:8px 16px;background:var(--card);border-bottom:1px solid var(--border);display:flex;gap:8px;overflow-x:auto">
      <button class="btn" id="selBtn" onclick="toggleSel()">Select</button>
      <button class="btn" id="dlBtn" onclick="dlSel()" style="display:none">Download</button>
      <button class="btn" id="zipBtn" onclick="dlZip()" style="display:none">ZIP</button>
      <button class="btn" id="delBtn" onclick="delSel()" style="display:none;color:var(--danger)">Delete</button>
    </div>

    <div class="modal-body">
      <div id="loading" style="text-align:center;padding:20px;color:#888">Loading...</div>
      <div class="gal-grid" id="galGrid"></div>
    </div>
  </div>
  
  <!-- Single Image Preview Modal -->
  <div class="modal" id="imgModal" style="background:rgba(0,0,0,0.98)" onclick="closeImg()">
    <button class="btn btn-icon" style="position:absolute;top:16px;right:16px;background:none;color:#fff;font-size:24px" onclick="closeImg()">&times;</button>
    <div style="flex:1;display:flex;align-items:center;justify-content:center;padding:20px">
      <img id="prevImg" src="" style="max-width:100%;max-height:90vh;border-radius:4px">
    </div>
    <div style="padding:20px;display:flex;justify-content:center;gap:16px;z-index:101" onclick="event.stopPropagation()">
      <button class="btn" onclick="dlCur()">Download</button>
      <button class="btn" style="color:var(--danger)" onclick="delCur()">Delete</button>
    </div>
  </div>

  <script>
    const EL = (id) => document.getElementById(id);
    const STREAM_URL = 'http://'+location.hostname+':81/stream';
    let selMode = false;
    let galFiles = [];
    let curFile = '';

    // -- Stream & Status --
    function startStream() {
      EL('stream').src = STREAM_URL + '?' + Date.now();
      showStatus('Streaming', 'ok');
    }
    
    function showStatus(msg, type='') {
      const el = EL('st');
      el.textContent = msg;
      el.className = 'status-badge ' + type;
    }

    function capture() {
      showStatus('Capturing...');
      fetch('/capture?' + Date.now())
        .then(r => r.ok ? showStatus('Saved', 'ok') : showStatus('Error', 'er'))
        .catch(() => showStatus('Error', 'er'));
    }

    function sendSet(q) {
      showStatus('Applying...');
      fetch('/settings', {method:'POST', body:q})
        .then(r => {
          if(r.ok) { showStatus('OK', 'ok'); setTimeout(startStream, 300); }
          else showStatus('Error', 'er');
        })
        .catch(() => showStatus('Error', 'er'));
    }
    function setFS(v) { sendSet('framesize='+v); }
    function setGS(c) { sendSet('grayscale='+(c?1:0)); }

    // -- Gallery --
    function openGal() {
      EL('galModal').classList.add('active');
      loadGal();
    }
    function closeGal() {
      EL('galModal').classList.remove('active');
      selMode = false; updateSelUI();
    }

    function loadGal() {
      EL('galGrid').innerHTML = '';
      EL('loading').style.display = 'block';
      fetch('/files').then(r => r.json()).then(d => {
        EL('loading').style.display = 'none';
        galFiles = d.files || [];
        EL('galCount').textContent = galFiles.length + ' items';
        galFiles.sort((a,b) => b.name.localeCompare(a.name)); // Descending
        
        const html = galFiles.map(f => `
          <div class="gal-item" onclick="onItemClick('${f.name}', this)">
            <input type="checkbox" class="gal-chk" data-name="${f.name}" onclick="event.stopPropagation();updateSelUI()">
            <img src="/sd/${f.name}" loading="lazy">
          </div>
        `).join('');
        EL('galGrid').innerHTML = html || '<div style="text-align:center;color:#666;grid-column:1/-1">No images</div>';
      });
    }

    function toggleSel() {
      selMode = !selMode;
      EL('galGrid').classList.toggle('select-mode', selMode);
      EL('selBtn').textContent = selMode ? 'Cancel' : 'Select';
      if(!selMode) document.querySelectorAll('.gal-chk').forEach(c => c.checked = false);
      updateSelUI();
    }

    function updateSelUI() {
      const n = document.querySelectorAll('.gal-chk:checked').length;
      EL('dlBtn').style.display = (selMode && n>0) ? 'block':'none';
      EL('zipBtn').style.display = (selMode && n>0) ? 'block':'none';
      EL('delBtn').style.display = (selMode && n>0) ? 'block':'none';
    }

    function onItemClick(name, el) {
      if(selMode) {
        const chk = el.querySelector('.gal-chk');
        chk.checked = !chk.checked;
        el.classList.toggle('selected', chk.checked);
        updateSelUI();
      } else {
        curFile = name;
        EL('prevImg').src = '/sd/' + name;
        EL('imgModal').classList.add('active');
      }
    }
    
    function closeImg() { EL('imgModal').classList.remove('active'); }

    // -- Actions (Download / Delete) --
    function getSel() {
      return Array.from(document.querySelectorAll('.gal-chk:checked')).map(c => c.dataset.name);
    }

    function dlSel() {
      const files = getSel();
      let i = 0;
      function next() {
        if(i >= files.length) return;
        const a = document.createElement('a');
        a.href = '/sd/' + files[i];
        a.download = files[i];
        a.click();
        i++; setTimeout(next, 500);
      }
      next();
    }

    function dlZip() {
      const files = getSel();
      if(!files.length) return;
      if(typeof JSZip === 'undefined') { alert('JSZip not loaded'); return; }
      const btn = EL('zipBtn');
      btn.innerText = 'Zipping...'; btn.disabled = true;
      
      const zip = new JSZip();
      let count = 0;
      
      const addFile = (idx) => {
        if(idx >= files.length) {
          zip.generateAsync({type:'blob'}).then(blob => {
            const a = document.createElement('a');
            a.href = URL.createObjectURL(blob);
            a.download = 'capture_' + new Date().getTime() + '.zip';
            a.click();
            btn.innerText = 'ZIP'; btn.disabled = false;
          });
          return;
        }
        fetch('/sd/'+files[idx]).then(r => r.blob()).then(blob => {
          zip.file(files[idx], blob);
          addFile(idx+1);
        });
      };
      addFile(0);
    }

    function delSel() {
      const files = getSel();
      if(!confirm(`Delete ${files.length} items?`)) return;
      let i = 0;
      function next() {
        if(i >= files.length) { loadGal(); toggleSel(); return; }
        fetch('/delete', {method:'POST', body:'file=/'+files[i]})
        .then(() => { i++; next(); });
      }
      next();
    }
    
    function dlCur() {
        const a = document.createElement('a');
        a.href = '/sd/' + curFile;
        a.download = curFile;
        a.click();
    }
    function delCur() {
        if(!confirm('Delete this image?')) return;
        fetch('/delete', {method:'POST', body:'file=/'+curFile})
        .then(() => { closeImg(); loadGal(); });
    }

    // -- Init --
    fetch('/status').then(r=>r.json()).then(d=>{
      EL('fs').value = d.framesize;
      EL('gs').checked = (d.grayscale === 1);
    });
    
    startStream();
    
    // Stats loop
    setInterval(() => {
      fetch('/stats').then(r=>r.json()).then(d=>{
        EL('s-fps').textContent = d.fps.toFixed(1);
        EL('s-frame').textContent = (d.frameKB).toFixed(1);
        EL('s-rssi').textContent = d.rssi + 'dBm';
      }).catch(()=>{});
    }, 2000); // Slow down stats to 2s
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
