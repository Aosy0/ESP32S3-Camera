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
  Serial.println("\n========================================");
  Serial.println("Initializing SD card (SD_MMC 1-bit mode)");
  Serial.println("========================================");

  Serial.printf("SD_MMC Pins:\n");
  Serial.printf("  CLK: GPIO%d\n", SD_MMC_CLK);
  Serial.printf("  CMD: GPIO%d\n", SD_MMC_CMD);
  Serial.printf("  D0:  GPIO%d\n", SD_MMC_D0);

  delay(500);

  // SD_MMCピンの設定
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);

  // 試行1: 1ビットモードで初期化
  Serial.println("\nAttempt 1: 1-bit mode (default speed)");
  if (SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5)) {
    Serial.println("SD card mounted successfully!");
  } else {
    Serial.println("Failed with default speed");

    // 試行2: 低速で再試行
    Serial.println("\nAttempt 2: 1-bit mode (low speed)");
    if (SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_PROBING, 5)) {
      Serial.println("SD card mounted (low speed)!");
    } else {
      Serial.println("All attempts failed");
      Serial.println("\nPlease check:");
      Serial.println("1. SD card is properly inserted");
      Serial.println("2. SD card is formatted as FAT32");
      Serial.println("3. XIAO is seated on expansion board");
      Serial.println("4. Try another SD card");
      return false;
    }
  }

  // カード情報を表示
  uint8_t cardType = SD_MMC.cardType();

  if (cardType == CARD_NONE) {
    Serial.println("No SD card detected");
    return false;
  }

  Serial.print("Card Type: ");
  if (cardType == CARD_MMC) {
    Serial.println("MMC");
  } else if (cardType == CARD_SD) {
    Serial.println("SDSC");
  } else if (cardType == CARD_SDHC) {
    Serial.println("SDHC");
  } else {
    Serial.println("UNKNOWN");
  }

  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  uint64_t totalBytes = SD_MMC.totalBytes() / (1024 * 1024);
  uint64_t usedBytes = SD_MMC.usedBytes() / (1024 * 1024);

  Serial.printf("Card Size: %llu MB\n", cardSize);
  Serial.printf("Total Space: %llu MB\n", totalBytes);
  Serial.printf("Used Space: %llu MB\n", usedBytes);
  Serial.printf("Free Space: %llu MB\n", totalBytes - usedBytes);

  Serial.println("\n========================================");
  Serial.println("SD card initialized successfully!");
  Serial.println("========================================");
  return true;
}

// 画像をSDカードに保存
bool saveImageToSD(camera_fb_t *fb) {
  if (!sdCardAvailable) {
    Serial.println("SD card not available");
    return false;
  }

  // タイムスタンプ取得
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    char filename[32];
    snprintf(filename, sizeof(filename), "/image_%04d.jpg", imageCount);

    File file = SD_MMC.open(filename, FILE_WRITE);
    if (!file) {
      Serial.printf("Failed to open file: %s\n", filename);
      return false;
    }

    file.write(fb->buf, fb->len);
    file.close();

    Serial.printf("Image saved: %s (%d bytes)\n", filename, fb->len);
    return true;
  }

  // タイムスタンプベースのファイル名
  char filename[64];
  snprintf(filename, sizeof(filename), "/%04d%02d%02d_%02d%02d%02d.jpg",
           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

  File file = SD_MMC.open(filename, FILE_WRITE);
  if (!file) {
    Serial.printf("Failed to open file: %s\n", filename);
    return false;
  }

  size_t written = file.write(fb->buf, fb->len);
  file.close();

  if (written == fb->len) {
    Serial.printf("Image saved: %s (%d bytes)\n", filename, fb->len);
    return true;
  } else {
    Serial.printf("Write error: %s (written %d/%d bytes)\n", filename, written,
                  fb->len);
    return false;
  }
}

// 写真撮影とSDカード保存
void captureAndSave() {
  Serial.println("\n=== Capturing image ===");

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    return;
  }

  Serial.printf("Image captured: %dx%d, %d bytes\n", fb->width, fb->height,
                fb->len);

  if (saveImageToSD(fb)) {
    imageCount++;
    Serial.printf("Total images saved: %d\n", imageCount);
  }

  esp_camera_fb_return(fb);

  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  Serial.println("=== Capture complete ===");
}

void setup() {
  Serial.begin(115200);
  while (!Serial)
    ;
  Serial.setDebugOutput(true);
  Serial.println();
  Serial.println("==================================");
  Serial.println("ESP32-S3 Camera Auto Save to SD");
  Serial.println("==================================");

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
    Serial.println("PSRAM found - using high quality settings");
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
    Serial.println("No PSRAM - using QVGA settings");
  }

  // カメラ初期化
  Serial.println("Initializing camera...");
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    Serial.println("Please check camera connection and restart");
    while (true) {
      delay(1000);
    }
  }
  Serial.println("Camera initialized successfully!");

  // センサー設定の調整
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    Serial.println("Camera sensor info:");
    Serial.printf("  PID: 0x%02X\n", s->id.PID);

    if (s->id.PID == 0x3660) {
      s->set_vflip(s, 1);
      s->set_brightness(s, 1);
      s->set_saturation(s, -2);
      Serial.println("  OV3660 detected - applied adjustments");
    }
  }

  // SDカード初期化
  sdCardAvailable = initSDCard();
  if (!sdCardAvailable) {
    Serial.println("WARNING: Running without SD card!");
    Serial.println("Camera will still work, but images won't be saved.");
  }

  // WiFi接続開始
  Serial.println("\nConnecting to WiFi...");
  Serial.printf("SSID: %s\n", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(false);

  int connection_timeout = 30;
  while (WiFi.status() != WL_CONNECTED && connection_timeout > 0) {
    delay(500);
    Serial.print(".");
    connection_timeout--;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected successfully!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    // NTP時刻同期
    Serial.println("Synchronizing time with NTP...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      Serial.println(&timeinfo, "Current time: %Y-%m-%d %H:%M:%S");
    } else {
      Serial.println("Failed to obtain time");
    }

    // Webカメラサーバー起動
    startCameraServer();
    Serial.printf("Camera server: http://%s\n",
                  WiFi.localIP().toString().c_str());
  } else {
    Serial.println("WiFi connection failed!");
    Serial.println("Continuing without WiFi (time stamps will use counter)");
  }

  Serial.println("==================================");
  Serial.println("System ready!");
  Serial.printf("Capture interval: %d minutes\n", CAPTURE_INTERVAL / 60000);
  Serial.println("==================================");

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
      Serial.println("WiFi disconnected. Attempting to reconnect...");
      WiFi.reconnect();
    } else {
      if (sdCardAvailable) {
        Serial.printf("Status OK - Next capture in %d seconds\n",
                      (CAPTURE_INTERVAL - (currentTime - lastCaptureTime)) /
                          1000);
      } else {
        Serial.println("Status OK - SD card not available");
      }
    }
  }

  delay(1000);
}
