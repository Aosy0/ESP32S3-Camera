#include "FS.h"
#include "SD_MMC.h"
#include "app_httpd.h"
#include "camera_pins.h"
#include "config.h"
#include "esp_camera.h"
#include "time.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

// XIAO ESP32S3 拡張基板 SDカードピン設定（SD_MMC 1ビットモード）
#define SD_MMC_CLK 7
#define SD_MMC_CMD 9
#define SD_MMC_D0 8

// 撮影間隔（ミリ秒）
const unsigned long CAPTURE_INTERVAL = 10 * 60 * 1000; // 10分

// NTPサーバー設定
const char *ntpServer = "ntp.nict.jp";
const long gmtOffset_sec = 9 * 3600; // JST (UTC+9)
const int daylightOffset_sec = 0;

// グローバル変数
unsigned long lastCaptureTime = 0;
int imageCount = 0;
bool sdCardAvailable = false;

// SDカード初期化（SD_MMC 1ビットモード）
bool initSDCard() {
  Serial.print("SD card: ");
  delay(500);
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);

  if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5)) {
    if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_PROBING, 5)) {
      Serial.println("Failed");
      return false;
    }
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("Not detected");
    return false;
  }

  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  Serial.printf("OK (%lluMB)\n", cardSize);
  return true;
}

// 画像をSDカードに保存（ファイル名を生成して返す）
bool saveImageToSD(camera_fb_t *fb, char *outFilename, size_t maxLen) {
  if (!sdCardAvailable)
    return false;

  struct tm timeinfo;
  char filename[64];

  if (!getLocalTime(&timeinfo)) {
    snprintf(filename, sizeof(filename), "/image_%04d.jpg", imageCount);
  } else {
    snprintf(filename, sizeof(filename), "/%04d%02d%02d_%02d%02d%02d.jpg",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  }

  File file = SD_MMC.open(filename, FILE_WRITE);
  if (!file)
    return false;

  size_t written = file.write(fb->buf, fb->len);
  file.close();

  if (written == fb->len) {
    Serial.printf("Saved: %s\n", filename);
    if (outFilename && maxLen > 0) {
      strncpy(outFilename, filename, maxLen - 1);
      outFilename[maxLen - 1] = '\0';
    }
    return true;
  }
  return false;
}

// 画像をクラウドにアップロード
bool uploadImage(camera_fb_t *fb, const char *filename) {
  if (strlen(UPLOAD_URL) == 0 || strlen(API_KEY) == 0) {
    Serial.println("Upload: Disabled (no config)");
    return false;
  }

  Serial.printf("Upload: Size=%d bytes\n", fb->len);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  Serial.println("Upload: Connecting...");
  if (!client.connect("asia-northeast1-cloud-storage-gateway-cps.cloudfunctions.net", 443)) {
    Serial.println("Upload: Connection failed");
    return false;
  }
  Serial.println("Upload: Connected");

  String boundary = "----ESP32Cam" + String(millis());
  
  String fileHeader = "--" + boundary + "\r\n";
  fileHeader += "Content-Disposition: form-data; name=\"file\"; filename=\"";
  fileHeader += String(filename).substring(1);
  fileHeader += "\"\r\nContent-Type: image/jpeg\r\n\r\n";
  String fileFooter = "\r\n--" + boundary + "--\r\n";
  size_t contentLength = fileHeader.length() + fb->len + fileFooter.length();

  // ヘッダー送信
  client.print("POST /upload-entry HTTP/1.1\r\n");
  client.print("Host: asia-northeast1-cloud-storage-gateway-cps.cloudfunctions.net\r\n");
  client.print("X-API-Key: " + String(API_KEY) + "\r\n");
  client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
  client.print("Content-Length: " + String(contentLength) + "\r\n");
  client.print("Connection: close\r\n");
  client.print("\r\n");

  // ボディ送信
  client.print(fileHeader);
  
  // 画像データを分割送信（8KBずつ）
  size_t totalSent = 0;
  const size_t chunkSize = 8192;
  while (totalSent < fb->len) {
    size_t toSend = (fb->len - totalSent > chunkSize) ? chunkSize : (fb->len - totalSent);
    size_t sent = client.write(fb->buf + totalSent, toSend);
    if (sent == 0) {
      Serial.println("Upload: Write failed");
      client.stop();
      return false;
    }
    totalSent += sent;
    delay(1);  // バッファ消化待ち
  }
  
  client.print(fileFooter);

  Serial.printf("Upload: Sent %d bytes\n", totalSent);

  // レスポンス待機
  Serial.println("Upload: Waiting response...");
  unsigned long startTime = millis();
  while (!client.available() && millis() - startTime < 10000) {
    delay(10);
  }

  // レスポンス読み取り
  String response = "";
  while (client.available()) {
    response += (char)client.read();
  }

  Serial.println("Upload: Response start");
  Serial.println(response.substring(0, 200));  // 最初の200文字だけ表示

  bool success = response.indexOf("200") >= 0 || response.indexOf("201") >= 0;
  if (success) {
    Serial.println("Upload: OK");
  } else {
    Serial.println("Upload: Failed");
  }

  client.stop();
  return success;
}

// 写真撮影とSDカード保存（自動アップロード付き）
void captureAndSave() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb)
    return;

  char filename[64] = "";
  if (saveImageToSD(fb, filename, sizeof(filename))) {
    imageCount++;
    uploadImage(fb, filename);
  }

  esp_camera_fb_return(fb);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nESP32-S3 Camera");

  // カメラ設定
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;

  // 安定性重視の設定
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  // PSRAMがある場合の最適化設定
  if (psramFound()) {
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  // カメラ初期化
  Serial.print("Camera: ");
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Failed (0x%x)\n", err);
    while (true)
      delay(1000);
  }
  Serial.println("OK");

  // センサー設定の調整
  sensor_t *s = esp_camera_sensor_get();
  if (s && s->id.PID == 0x3660) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }

  // SDカード初期化
  sdCardAvailable = initSDCard();

  // WiFi接続開始
  Serial.print("WiFi: ");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(false);

  int connection_timeout = 30;
  while (WiFi.status() != WL_CONNECTED && connection_timeout > 0) {
    delay(500);
    connection_timeout--;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("OK");
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    startCameraServer();
  } else {
    Serial.println("Failed");
  }

  Serial.println("Ready");

  // 初回撮影（SDカードがある場合のみ）
  if (sdCardAvailable) {
    delay(2000);
    captureAndSave();
    lastCaptureTime = millis();
  }
}

void loop() {
  unsigned long currentTime = millis();

  // 10分ごとに撮影（SDカードがある場合のみ）
  if (sdCardAvailable && (currentTime - lastCaptureTime >= CAPTURE_INTERVAL)) {
    captureAndSave();
    lastCaptureTime = currentTime;
  }

  // WiFi接続チェック（1分ごと）
  static unsigned long lastWiFiCheck = 0;
  if (currentTime - lastWiFiCheck >= 60000) {
    lastWiFiCheck = currentTime;

    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
    }
  }

  delay(1000);
}
