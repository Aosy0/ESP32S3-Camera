# ESP32-S3 Camera

XIAO ESP32S3 Sense 用のカメラアプリです。10分ごとに自動撮影してSDカードに保存しつつ、ブラウザからライブストリームや画像管理が行えます。

## 必要なハードウェア

- Seeed Studio XIAO ESP32S3 Sense
- XIAO ESP32S3 Sense 拡張基板
- **外部WiFiアンテナ（必須）**
- microSDカード（FAT32フォーマット、32GB以下推奨）
- WiFi環境（2.4GHz）

> [!IMPORTANT]
> 外部アンテナを接続しないと、WiFi接続が非常に不安定になります。必ずIPEXコネクタにアンテナを取り付けてください。

## セットアップ

### 1. WiFi設定

`src/config.h.example` を `src/config.h` にコピーして編集します。

```cpp
const char* WIFI_SSID     = "あなたのWiFi名";
const char* WIFI_PASSWORD = "あなたのWiFiパスワード";

// クラウドアップロード（不要なら空欄のままでOK）
const char* UPLOAD_URL = "";  // Google Cloud Functions のURL
const char* API_KEY    = "";  // X-API-Key ヘッダーの値
```

### 2. ハードウェア準備

1. 外部アンテナを XIAO ESP32S3 に接続（IPEXコネクタ）
2. microSDカードを FAT32 でフォーマット
3. 拡張基板のSDカードスロットに差し込む
4. 拡張基板に XIAO ESP32S3 を装着

### 3. 書き込み

PlatformIO でビルド＆書き込みを行います。

```bash
pio run --target upload
```

### 4. IPアドレスの確認

書き込み後、シリアルモニタ（115200 baud）を開いて IP アドレスを確認してください。

```
ESP32-S3 Camera
Camera: OK
SD card: OK (32000MB)
WiFi: OK
IP: 192.168.1.100        ← このアドレスをメモ
NTP: 2026-02-19 18:30:00
Ready
```

> [!NOTE]
> IPアドレスは環境によって異なります。シリアルモニタで必ず確認してください。
> PlatformIO の場合は `pio device monitor` コマンド、または IDE のシリアルモニタ機能を使用します。

## 使い方

ブラウザで `http://<IPアドレス>` を開きます（シリアルモニタで確認したアドレス）。

### Live タブ
- リアルタイムストリーム表示（ストリームは内部でポート81を使用）
- 解像度変更（QVGA / CIF / VGA / SVGA / XGA）
- グレースケール切り替え
- **Capture**: 手動撮影してSDカードに保存
- **Upload to cloud**: ON にしてからCapture すると、クラウドへもアップロード
- **Reload**: ストリームを再接続

### Gallery タブ
- SDカード内の画像を一覧表示
- サムネイルクリックで拡大表示・ダウンロード・削除
- **Select** → 複数選択して一括ダウンロード / ZIP ダウンロード / 削除

## 機能概要

| 機能 | 詳細 |
|------|------|
| 自動撮影 | 10分間隔（`CAPTURE_INTERVAL` で変更可） |
| ファイル名 | `YYYYMMDD_HHMMSS.jpg`（NTP同期時） / `image_NNNN.jpg`（未同期時） |
| デフォルト解像度 | VGA (640×480)、JPEG品質 10〜12 |
| クラウドアップロード | HTTPS POST (multipart/form-data)、8KBチャンク送信 |
| WiFi再接続 | 切断を検知して1分ごとに自動再接続 |

### 撮影間隔の変更

`src/main.cpp` の以下を編集：

```cpp
const unsigned long CAPTURE_INTERVAL = 10 * 60 * 1000;  // 10分（ミリ秒）
```

## SDカード容量目安

VGA画質（1枚あたり約40KB）の場合：

| 容量 | 枚数 |
|------|------|
| 1GB  | 約 25,000枚 |
| 32GB | 約 800,000枚 |

10分間隔で撮影した場合、32GBのSDカードで **約15年分** 保存できます。

## ピン配置

### SDカード（SD_MMC 1ビットモード）

| 機能 | GPIO |
|------|------|
| CLK  | 7    |
| CMD  | 9    |
| D0   | 8    |

### Webサーバー

| ポート | 用途 |
|--------|------|
| 80     | WebUI・API・画像配信 |
| 81     | ライブストリーム専用 |

## ライセンス

MIT License
