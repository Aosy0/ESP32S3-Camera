#include "app_httpd.h"
#include "camera_pins.h"
#include "config.h" // WiFi設定を読み込み
#include "esp_camera.h"
#include <Arduino.h>
#include <WiFi.h>

// WiFi設定はconfig.hファイルで定義されています

void setup() {
  Serial.begin(115200);
  while (!Serial)
    ;
  Serial.setDebugOutput(true);
  Serial.println();
  Serial.println("==================================");
  Serial.println("ESP32-S3 Camera Streaming Server");
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
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG; // ストリーミング用
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  // PSRAMがある場合はより高品質な設定
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
      Serial.println("PSRAM found - using high quality settings");
    } else {
      // PSRAMがない場合はフレームサイズを制限
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
      Serial.println("No PSRAM - using limited settings");
    }
  } else {
    // 顔検出/認識用の設定
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
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

  // センサー設定の取得と調整
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    Serial.println("Camera sensor info:");
    Serial.printf("  PID: 0x%02X\n", s->id.PID);

    // OV3660センサーの場合は調整（XIAOで使用されることが多い）
    if (s->id.PID == 0x3660) {
      s->set_vflip(s, 1);       // 垂直反転
      s->set_brightness(s, 1);  // 明るさアップ
      s->set_saturation(s, -2); // 彩度を少し下げる
      Serial.println("  OV3660 detected - applied adjustments");
    }

    // 初期フレームレート向上のためフレームサイズを調整
    if (config.pixel_format == PIXFORMAT_JPEG) {
      s->set_framesize(s, FRAMESIZE_QVGA);
      Serial.println("  Initial frame size set to QVGA for better framerate");
    }
  }

  // WiFi接続開始
  Serial.println("\nConnecting to WiFi...");
  Serial.printf("SSID: %s\n", WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(false);

  int connection_timeout = 30; // 30秒タイムアウト
  while (WiFi.status() != WL_CONNECTED && connection_timeout > 0) {
    delay(500);
    Serial.print(".");
    connection_timeout--;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected successfully!");
    Serial.println("==================================");
    Serial.print("Camera Ready! Use 'http://");
    Serial.print(WiFi.localIP());
    Serial.println("' to connect");
    Serial.println("==================================");
    Serial.println("\nAvailable URLs:");
    Serial.print("  Main page:   http://");
    Serial.println(WiFi.localIP());
    Serial.print("  Stream:      http://");
    Serial.print(WiFi.localIP());
    Serial.println("/stream");
    Serial.print("  Capture:     http://");
    Serial.print(WiFi.localIP());
    Serial.println("/capture");
    Serial.println("==================================");

    // カメラWebサーバーを起動
    startCameraServer();
  } else {
    Serial.println("WiFi connection failed!");
    Serial.println("Please check your WiFi credentials and try again");
    Serial.println("Restarting in 10 seconds...");
    delay(10000);
    ESP.restart();
  }
}

void loop() {
  // サーバーはバックグラウンドタスクで動作
  // 必要に応じてここにステータス表示などを追加可能
  delay(10000);

  // WiFi接続チェック
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection lost! Restarting...");
    delay(1000);
    ESP.restart();
  }
}
