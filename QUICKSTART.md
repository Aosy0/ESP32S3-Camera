# クイックスタートガイド

## 必要な準備

### 1. ハードウェア
- [x] XIAO ESP32S3 Sense
- [x] XIAO ESP32S3 拡張基板
- [x] microSDカード (FAT32フォーマット)
- [x] WiFiルーター (2.4GHz)

### 2. ソフトウェア
- [x] WiFi設定ファイル (`src/config.h`)

## 5分でセットアップ

### ステップ1: WiFi設定

```bash
# config.h.exampleをコピー
copy src\config.h.example src\config.h

# config.hを編集してWiFi情報を入力
notepad src\config.h
```

### ステップ2: SDカード準備

1. microSDカードをFAT32でフォーマット
2. XIAO拡張基板に挿入

### ステップ3: 書き込み

```bash
pio run --target upload
```

### ステップ4: 動作確認

```bash
pio device monitor
```

以下のメッセージが表示されればOK:
```
PSRAM found - using high quality settings
Camera initialized successfully!
SD Card Size: 32000MB
WiFi connected successfully!
System ready!
Capture interval: 10 minutes
Image saved: /20260211_153000.jpg
```

## デフォルト設定

| 項目 | 値 |
|------|------|
| 撮影間隔 | 10分 |
| 解像度 | VGA (640x480) |
| JPEG品質 | 10 (高品質) |
| ファイル名 | YYYYmmdd_HHMMSS.jpg |
| 保存先 | SDカードルート |

## よくある質問

### Q: WiFiなしで使えますか？
A: はい。WiFiがなくても撮影・保存は可能です。ただしファイル名が連番（image_0000.jpg）になります。

### Q: 撮影間隔を変更できますか？
A: `src/main.cpp`の`CAPTURE_INTERVAL`を変更してください。

### Q: どのくらいの枚数が保存できますか？
A: 32GBのSDカードで約800,000枚（VGA画質）保存可能です。

### Q: バッテリー動作は可能ですか？
A: 可能ですが、安定した電源供給（5V 500mA以上）が推奨されます。

## トラブルシューティング

### SDカードエラー
```
SD card mount failed!
```
→ SDカードがFAT32でフォーマットされているか確認

### WiFi接続失敗
```
WiFi connection failed!
```
→ SSID/パスワードを確認。ただし撮影・保存は継続されます

### カメラエラー
```
Camera init failed with error 0x...
```
→ カメラモジュールの接続を確認

## 次のステップ

1. 撮影間隔のカスタマイズ
2. 画質・解像度の調整
3. 長期運用のテスト

詳細は `README.md` を参照してください。
