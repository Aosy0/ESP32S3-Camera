# ESP32-S3 Camera Streaming Server

XIAO ESP32S3 Sense用のシンプルなカメラストリーミングサーバーです。

## 機能

- **ライブストリーミング**: MJPEG形式でのリアルタイム映像配信
- **静止画キャプチャ**: ブラウザからの写真撮影機能
- **Webインターフェース**: PCやスマホからアクセス可能な操作画面
- **PSRAM対応**: 高解像度・高フレームレートのためのメモリ最適化
- **セキュリティ管理**: WiFi設定（SSID/パスワード）の外部ファイル分離

## 使い方

### 1. WiFi設定の準備

SSIDやパスワードを安全に管理するため、専用の設定ファイルを作成します。

1. `src/config.h.example` をコピーして `src/config.h` を作成します。
2. `src/config.h` を開き、使用するWiFiの情報を入力します：
   ```cpp
   const char* WIFI_SSID = "あなたのWiFi名";
   const char* WIFI_PASSWORD = "あなたのWiFiパスワード";
   ```

> [!IMPORTANT]
> `src/config.h` は `.gitignore` に登録されているため、GitHub等に公開されることはありません。個人情報は安全に保護されます。

### 2. 書き込みと実行

1. ESP32S3をPCに接続します。
2. PlatformIOでビルドと書き込みを実行します：
   ```bash
   pio run --target upload
   ```
3. シリアルモニタを起動してIPアドレスを確認します：
   ```bash
   pio device monitor
   ```

### 3. ブラウザでアクセス

同じWiFiに接続したデバイスから、表示されたIPアドレスにアクセスしてください。
例: `http://192.168.1.100`
