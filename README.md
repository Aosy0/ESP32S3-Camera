# ESP32-S3 Camera Auto Save to SD Card

XIAO ESP32S3 Sense用の自動撮影・SDカード保存プログラムです。10分ごとに自動撮影し、SDカードに保存します。

## 機能

- **自動撮影**: 10分間隔で自動的に撮影
- **SDカード保存**: タイムスタンプ付きファイル名で保存
- **WiFi + NTP**: 正確な日時情報を取得
- **安定動作**: VGA解像度で安定した長時間動作
- **状態監視**: シリアルモニタでステータス確認可能

## 必要なハードウェア

- Seeed Studio XIAO ESP32S3 Sense
- XIAO ESP32S3 Sense 拡張基板
- microSDカード（FAT32フォーマット推奨）
- WiFi環境（NTP時刻同期用、オプション）

## セットアップ

### 1. WiFi設定ファイルの作成

`src/config.h.example` をコピーして `src/config.h` を作成し、WiFi情報を入力：

```cpp
const char* WIFI_SSID = "あなたのWiFi名";
const char* WIFI_PASSWORD = "あなたのWiFiパスワード";
```

> [!NOTE]
> WiFiに接続できない場合でも動作しますが、ファイル名が連番になります。

### 2. SDカードの準備

1. microSDカードをFAT32でフォーマット
2. XIAO拡張基板のSDカードスロットに挿入
3. 拡張基板にXIAO ESP32S3を装着

### 3. 書き込みと実行

```bash
pio run --target upload
pio device monitor
```

## 動作確認

シリアルモニタで以下の出力を確認してください：

```
PSRAM found - using high quality settings
Camera initialized successfully!
SD Card Type: SDHC
SD Card Size: 32000MB
WiFi connected successfully!
IP address: 192.168.1.100
Current time: 2026-02-11 15:30:00
System ready!
Capture interval: 10 minutes
```

## 保存される画像

### ファイル名形式

WiFi接続成功時（NTP時刻同期あり）:
```
/20260211_153000.jpg
/20260211_154000.jpg
/20260211_155000.jpg
```

WiFi接続失敗時:
```
/image_0000.jpg
/image_0001.jpg
/image_0002.jpg
```

### 画像仕様

- 解像度: VGA (640x480)
- 形式: JPEG
- 品質: 10 (PSRAMあり) / 12 (PSRAMなし)
- 平均ファイルサイズ: 30-50KB

## 撮影間隔の変更

`src/main.cpp`の以下の行を編集：

```cpp
const unsigned long CAPTURE_INTERVAL = 10 * 60 * 1000;  // ミリ秒単位
```

例：
- 5分間隔: `5 * 60 * 1000`
- 30分間隔: `30 * 60 * 1000`
- 1時間間隔: `60 * 60 * 1000`

## 画質・解像度の変更

```cpp
config.frame_size = FRAMESIZE_VGA;   // 解像度
config.jpeg_quality = 10;            // 品質 (0-63, 小さいほど高品質)
```

解像度の選択肢:
- `FRAMESIZE_QVGA` - 320x240 (最小)
- `FRAMESIZE_VGA` - 640x480 (推奨)
- `FRAMESIZE_SVGA` - 800x600
- `FRAMESIZE_HD` - 1280x720
- `FRAMESIZE_UXGA` - 1600x1200 (最大、不安定になる可能性あり)

## トラブルシューティング

### SDカードが認識されない

1. SDカードがFAT32でフォーマットされているか確認
2. SDカードが正しく挿入されているか確認
3. 拡張基板とXIAOの接続を確認
4. シリアルモニタで「SD card mount failed!」を確認

### WiFiに接続できない

- SSID/パスワードが正しいか確認
- 2.4GHz WiFiに接続しているか確認（5GHzは非対応）
- WiFi接続なしでも撮影・保存は可能（ファイル名が連番になる）

### メモリ不足で動作が不安定

解像度を下げてください：
```cpp
config.frame_size = FRAMESIZE_QVGA;  // 320x240
```

### SDカードの容量不足

保存された画像数を確認：
```
VGA (640x480) 1枚あたり約40KB
1GB SDカード = 約25,000枚
32GB SDカード = 約800,000枚
```

10分間隔で撮影した場合：
- 1日: 144枚 (約5.7MB)
- 1週間: 1,008枚 (約40MB)
- 1ヶ月: 4,320枚 (約170MB)

## ピン配置

XIAO ESP32S3 Sense 拡張基板使用時のSDカード接続：

| 機能 | GPIO | 説明 |
|------|------|------|
| CS   | 21   | チップセレクト |
| MOSI | 9    | データ出力 |
| MISO | 8    | データ入力 |
| SCK  | 7    | クロック |

> [!IMPORTANT]
> カメラとSDカードは異なるピンを使用しているため、同時使用可能です。

## 消費電力について

- 待機時: 約80-100mA
- 撮影時: 約200-300mA (瞬間的)
- WiFi接続時: 約100-150mA

長時間動作させる場合は、安定した電源供給が必要です。
