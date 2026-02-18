#include "FS.h"
#include "SD_MMC.h"
#include "app_httpd.h"
#include "camera_pins.h"
#include "config.h"
#include "esp_camera.h"
#include "time.h"
#include <Arduino.h>
#include <WiFi.h>

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

// 画像をSDカードに保存
bool saveImageToSD(camera_fb_t *fb) {
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
    return true;
  }
  return false;
}

// 写真撮影とSDカード保存
void captureAndSave() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb)
    return;

  if (saveImageToSD(fb)) {
    imageCount++;
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
  config.xclk_freq_hz = 24000000;
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
    config.fb_count = 3;
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
