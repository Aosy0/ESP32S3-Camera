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
  config.pixel_format = PIXFORMAT_JPEG; // ストリーミング用
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;

  // 安定性重視の設定（ESP32S3の負荷を軽減）
  config.frame_size = FRAMESIZE_VGA; // 640x480 (UXGAより大幅に軽量)
  config.jpeg_quality = 15;          // 12より圧縮率を上げて処理を軽く
  config.fb_count = 1;               // メモリ節約のためシングルバッファ

  // PSRAMがある場合の最適化設定
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 18; // 品質をやや下げて安定性向上
      config.fb_count = 2;      // ダブルバッファで滑らかに
      config.grab_mode = CAMERA_GRAB_LATEST;
      Serial.println("PSRAM found - using optimized settings for stability");
    } else {
      // PSRAMがない場合はさらに軽量化
      config.frame_size = FRAMESIZE_QVGA; // 320x240
      config.fb_location = CAMERA_FB_IN_DRAM;
      config.jpeg_quality = 20;
      Serial.println("No PSRAM - using minimal settings");
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

    // フレームサイズはVGAのまま維持（安定性重視）
    Serial.printf("  Frame size: VGA (640x480)\n");
  }

  // WiFi接続開始
  Serial.println("\nConnecting to WiFi...");
  Serial.printf("SSID: %s\n", WIFI_SSID);

  WiFi.mode(WIFI_STA); // ステーションモードに明示的に設定
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(false); // WiFiスリープ無効で安定性向上

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
  // ウォッチドッグタイマー対策のためyield()を呼ぶ
  yield();

  delay(5000); // 5秒ごとにチェック

  // WiFi接続チェック
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection lost! Restarting...");
    delay(1000);
    ESP.restart();
  }

  // メモリ監視（デバッグ用、必要に応じてコメントアウト）
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck > 30000) { // 30秒ごと
    lastCheck = millis();
    Serial.printf("Free heap: %d bytes, Min free heap: %d bytes\n",
                  ESP.getFreeHeap(), ESP.getMinFreeHeap());
  }
}
