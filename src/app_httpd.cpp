#include "FS.h"
#include "SD_MMC.h"
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

// ポート80: API/ページ用、ポート81: ストリーム専用
httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;
static bool grayscaleMode = false;

// main.cppの関数/変数を参照
extern bool saveImageToSD(camera_fb_t *fb);
extern bool sdCardAvailable;
extern int imageCount;

// ────────────────────────────────────────
// ストリームハンドラ（ポート81で動作）
// ────────────────────────────────────────
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
  }
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
    *{box-sizing:border-box;margin:0;padding:0}
    body{font-family:'Segoe UI',Arial,sans-serif;background:#121212;color:#e0e0e0;min-height:100vh}
    .hd{background:#1e1e1e;padding:12px 20px;border-bottom:1px solid #333;display:flex;justify-content:space-between;align-items:center}
    .hd h1{font-size:16px;font-weight:600;color:#90caf9}
    .tabs{display:flex;gap:4px}
    .tab{padding:6px 14px;border:1px solid #444;border-radius:4px;background:#2a2a2a;color:#aaa;font-size:13px;cursor:pointer}
    .tab.active{background:#333;color:#90caf9;border-color:#90caf9}
    .pg{display:none;padding:16px;max-width:720px;margin:0 auto}
    .pg.active{display:block}
    .sc{text-align:center}
    #stream{max-width:100%;border:2px solid #333;border-radius:8px;background:#000}
    .ct{display:flex;flex-direction:column;gap:10px;margin-top:12px}
    .cg{background:#1e1e1e;border-radius:8px;padding:14px;border:1px solid #333}
    .cg label{display:block;font-size:12px;color:#90caf9;margin-bottom:6px;font-weight:600}
    select{width:100%;padding:8px;background:#2a2a2a;color:#e0e0e0;border:1px solid #444;border-radius:6px;font-size:14px;outline:none}
    .tr{display:flex;justify-content:space-between;align-items:center}
    .tg{position:relative;width:44px;height:24px}
    .tg input{opacity:0;width:0;height:0}
    .sl{position:absolute;cursor:pointer;inset:0;background:#444;border-radius:24px;transition:.3s}
    .sl:before{content:"";position:absolute;height:18px;width:18px;left:3px;bottom:3px;background:#fff;border-radius:50%;transition:.3s}
    .tg input:checked+.sl{background:#90caf9}
    .tg input:checked+.sl:before{transform:translateX(20px)}
    .br{display:flex;gap:6px}
    .btn{flex:1;padding:8px;border:1px solid #444;border-radius:6px;background:#2a2a2a;color:#e0e0e0;font-size:13px;cursor:pointer;transition:background .2s}
    .btn:hover{background:#383838}
    .btn:active{background:#444}
    .btn:disabled{background:#1a1a1a;color:#666;cursor:not-allowed;opacity:0.6}
    .btn-g{background:#1b5e20;border-color:#2e7d32}
    .btn-g:hover{background:#2e7d32}
    .btn-r{background:#b71c1c;border-color:#c62828}
    .btn-r:hover{background:#c62828}
    .st{font-size:11px;color:#888;text-align:center;min-height:16px;margin-top:4px}
    .st.ok{color:#4caf50}.st.er{color:#f44336}
    .gal{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:8px}
    .th{position:relative;aspect-ratio:4/3;overflow:hidden;border-radius:6px;border:1px solid #333;cursor:pointer;background:#000}
    .th img{width:100%;height:100%;object-fit:cover}
    .th .inf{position:absolute;bottom:0;left:0;right:0;padding:4px 6px;background:rgba(0,0,0,.7);font-size:10px;color:#ccc}
    .th .chk{position:absolute;top:4px;left:4px;width:20px;height:20px;accent-color:#90caf9;display:none}
    .sel-mode .th .chk{display:block}
    .th.selected{border:2px solid #90caf9}
    .md{display:none;position:fixed;inset:0;background:rgba(0,0,0,.9);z-index:100;align-items:center;justify-content:center;flex-direction:column}
    .md.active{display:flex}
    .md img{max-width:95%;max-height:80vh;border-radius:8px}
    .md .cl{position:absolute;top:16px;right:20px;font-size:28px;color:#fff;cursor:pointer;background:none;border:none}
    .md .mi{color:#aaa;font-size:13px;margin-top:8px}
    .md .mb{display:flex;gap:8px;margin-top:10px}
    .em{text-align:center;color:#666;padding:40px;font-size:14px}
    .fc{font-size:12px;color:#888;margin-bottom:8px}
    .bar{display:flex;gap:6px;margin-bottom:12px;flex-wrap:wrap;align-items:center}
    .bar .cnt{font-size:12px;color:#90caf9;margin-left:4px}
    .prog{font-size:11px;color:#aaa;min-height:16px;margin-top:4px}
  </style>
</head>
<body>
  <div class="hd">
    <h1>ESP32-S3 Camera</h1>
    <div class="tabs">
      <div class="tab active" onclick="showPage('live')">Live</div>
      <div class="tab" onclick="showPage('gal')">Gallery</div>
    </div>
  </div>

  <div class="pg active" id="pg-live">
    <div class="sc">
      <img id="stream" src="" alt="Stream">
    </div>
    <div class="ct">
      <div class="cg">
        <label>Resolution</label>
        <select id="fs" onchange="setFS(this.value)">
          <option value="5">QVGA (320x240)</option>
          <option value="6">CIF (400x296)</option>
          <option value="8" selected>VGA (640x480)</option>
          <option value="9">SVGA (800x600)</option>
          <option value="10">XGA (1024x768)</option>
        </select>
      </div>
      <div class="cg">
        <div class="tr">
          <span style="font-size:14px">Grayscale</span>
          <label class="tg"><input type="checkbox" id="gs" onchange="setGS(this.checked)"><span class="sl"></span></label>
        </div>
      </div>
      <div class="br">
        <button class="btn" onclick="capture()">Capture</button>
        <button class="btn" onclick="startStream()">Reload</button>
      </div>
      <div class="st" id="st">Ready</div>
    </div>
  </div>

  <div class="pg" id="pg-gal">
    <div class="prog" id="prog"></div>
    <div class="fc" id="fc"></div>
    <div class="bar">
      <button class="btn" onclick="loadGal()">Refresh</button>
      <button class="btn" id="selBtn" onclick="toggleSel()">Select</button>
      <button class="btn" id="saBtn" onclick="selAll()" style="display:none">All</button>
      <button class="btn" id="dlAllBtn" onclick="dlAll()" style="display:none">Download</button>
      <button class="btn" id="zipBtn" onclick="dlZip()" style="display:none">ZIP Download</button>
      <button class="btn btn-r" id="rmAllBtn" onclick="rmAll()" style="display:none">Delete</button>
      <span class="cnt" id="selCnt"></span>
    </div>
    <div class="gal" id="gal"></div>
  </div>

  <div class="md" id="md" onclick="closeMd(event)">
    <button class="cl" onclick="closeMd()">&times;</button>
    <img id="md-img" src="">
    <div class="mi" id="md-info"></div>
    <div class="mb">
      <button class="btn" onclick="dlFile()">Download</button>
      <button class="btn btn-r" onclick="rmFile()">Delete</button>
    </div>
  </div>

  <script>
    var STREAM_URL='http://'+location.hostname+':81/stream';
    var si=document.getElementById('stream');
    var stEl=document.getElementById('st');
    var curFile='';
    var selMode=false;
    var galFiles=[];

    function setSt(m,t){stEl.textContent=m;stEl.className='st'+(t?' '+t:'')}
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

    function sendSet(body){
      setSt('Applying...');stopStream();
      var x=new XMLHttpRequest();x.open('POST','/settings',true);
      x.onload=function(){if(x.status===200){setSt('Applied','ok');setTimeout(startStream,300);}else setSt('Error','er')};
      x.onerror=function(){setSt('Error','er')};x.send(body);
    }
    function setFS(v){sendSet('framesize='+v)}
    function setGS(on){sendSet('grayscale='+(on?1:0))}

    function capture(){
      setSt('Saving...');
      fetch('/capture?'+Date.now()).then(function(r){
        if(r.ok)setSt('Saved to SD','ok');
        else setSt('Failed','er');
      }).catch(function(){setSt('Failed','er')});
    }

    // -- Gallery --
    function toggleSel(){
      selMode=!selMode;
      var g=document.getElementById('gal');
      if(selMode){g.classList.add('sel-mode');document.getElementById('selBtn').textContent='Cancel';}
      else{g.classList.remove('sel-mode');document.getElementById('selBtn').textContent='Select';
        g.querySelectorAll('.chk').forEach(function(c){c.checked=false});
        g.querySelectorAll('.th').forEach(function(t){t.classList.remove('selected')});
      }
      updateSelUI();
    }
    function updateSelUI(){
      var n=getSelNames().length;
      document.getElementById('saBtn').style.display=selMode?'':'none';
      document.getElementById('dlAllBtn').style.display=(selMode&&n>0)?'':'none';
      document.getElementById('zipBtn').style.display=(selMode&&n>0)?'':'none';
      document.getElementById('rmAllBtn').style.display=(selMode&&n>0)?'':'none';
      document.getElementById('selCnt').textContent=selMode?(n>0?n+' selected':''):'';}
    function selAll(){
      var g=document.getElementById('gal');var all=g.querySelectorAll('.chk');
      var allChecked=true;all.forEach(function(c){if(!c.checked)allChecked=false});
      all.forEach(function(c){c.checked=!allChecked;c.parentElement.classList.toggle('selected',!allChecked)});
      updateSelUI();
    }
    function getSelNames(){
      var names=[];document.querySelectorAll('.gal .chk:checked').forEach(function(c){names.push(c.dataset.name)});return names;
    }
    function onChk(el){el.parentElement.classList.toggle('selected',el.checked);updateSelUI()}
    function thClick(name,size,el){
      if(selMode){var c=el.querySelector('.chk');c.checked=!c.checked;onChk(c);}
      else openMd(name,size);
    }

    function loadGal(){
      var g=document.getElementById('gal');var fc=document.getElementById('fc');
      g.innerHTML='<div class="em">Loading...</div>';setProg('');
      fetch('/files').then(function(r){return r.json()}).then(function(d){
        galFiles=d.files||[];
        if(galFiles.length===0){g.innerHTML='<div class="em">No images on SD card</div>';fc.textContent='';return;}
        galFiles.sort(function(a,b){return b.name.localeCompare(a.name)});
        fc.textContent=galFiles.length+' images';
        var h='';
        galFiles.forEach(function(f){
          var kb=Math.round(f.size/1024);
          h+='<div class="th" onclick="thClick(\''+f.name+'\','+f.size+',this)">';
          h+='<input type="checkbox" class="chk" data-name="'+f.name+'" onclick="event.stopPropagation();onChk(this)">';
          h+='<img src="/sd/'+f.name+'" loading="lazy">';
          h+='<div class="inf">'+f.name+' ('+kb+'KB)</div>';
          h+='</div>';
        });
        g.innerHTML=h;
        if(selMode)g.classList.add('sel-mode');
        updateSelUI();
      }).catch(function(e){
        g.innerHTML='<div class="em">Failed to load</div>';fc.textContent='';
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
            setProg('ZIP download complete ('+names.length+' files)');
            URL.revokeObjectURL(a.href);
            zipBtn.disabled=false;
            zipBtn.textContent='ZIP Download';
          }).catch(function(e){
            setProg('ZIP generation failed: '+e);
            zipBtn.disabled=false;
            zipBtn.textContent='ZIP Download';
          });
          return;
        }
        fetch('/sd/'+names[i]).then(function(r){return r.blob()}).then(function(blob){
          zip.file(names[i],blob);
          i++;setProg('Creating ZIP: '+i+'/'+names.length);
          fetchNext();
        }).catch(function(e){
          i++;setProg('Error on '+names[i-1]+', skipping...');
          setTimeout(fetchNext,100);
        });
      }
      fetchNext();
    }
    function rmAll(){
      var names=getSelNames();if(names.length===0)return;
      if(!confirm('WARNING: '+names.length+' images will be permanently deleted.\n\nAre you sure?'))return;
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
      if(!e||e.target===document.getElementById('md')||e.target.classList.contains('cl'))
        document.getElementById('md').classList.remove('active');
    }
    function dlFile(){var a=document.createElement('a');a.href='/sd/'+curFile;a.download=curFile;a.click()}
    function rmFile(){
      if(!confirm('"'+curFile+'" to delete. Are you sure?'))return;
      var x=new XMLHttpRequest();x.open('POST','/delete',true);
      x.onload=function(){closeMd();loadGal()};x.send('file=/'+curFile);
    }

    fetch('/status').then(function(r){return r.json()}).then(function(d){
      document.getElementById('fs').value=d.framesize;
      document.getElementById('gs').checked=d.grayscale===1;
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

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &settings_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &files_uri);
    httpd_register_uri_handler(camera_httpd, &sdfile_uri);
    httpd_register_uri_handler(camera_httpd, &delete_uri);
  }

  // ポート81: ストリーム専用サーバー
  httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
  stream_config.server_port = 81;
  stream_config.ctrl_port = 32769;
  stream_config.max_uri_handlers = 1;

  httpd_uri_t stream_uri = {.uri = "/stream",
                            .method = HTTP_GET,
                            .handler = stream_handler,
                            .user_ctx = NULL};

  if (httpd_start(&stream_httpd, &stream_config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}
