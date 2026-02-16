# SDカード認識トラブルシューティングガイド

## 症状: SDカードが認識されない

シリアルモニタで以下のメッセージが表示される場合:
```
✗ All attempts failed
```

## チェック項目

### 1. SDカードのフォーマット確認

**必須**: SDカードはFAT32でフォーマットする必要があります。

#### Windows
1. エクスプローラーでSDカードを右クリック
2. 「フォーマット」を選択
3. ファイルシステム: **FAT32** を選択
4. アロケーションユニットサイズ: デフォルト
5. 「開始」をクリック

> **注意**: 32GB以下のSDカードを推奨。64GB以上の場合、exFATになる可能性があるため、専用ツールでFAT32化が必要。

### 2. 物理的な接続確認

#### XIAO ESP32S3の装着
- 拡張基板に**しっかりと押し込まれているか**確認
- ピンが曲がっていないか確認
- 両端が均等に挿入されているか確認

#### SDカードの挿入
- カチッと音がするまで**しっかり押し込む**
- 半挿しになっていないか確認
- 金属接点が見えないことを確認

### 3. SDカードの接点清掃

- SDカードの金属接点を**柔らかい布**で拭く
- 消しゴムで軽く拭いても可（削りカスは必ず除去）
- 拡張基板のSDカードスロット内も確認

### 4. 別のSDカードで試す

- 別のSDカード（特に容量が小さいもの: 2GB, 4GB, 8GB）で試す
- 新しいSDカードで試す
- ブランド品のSDカードを使用（NoName品は相性問題がある可能性）

### 5. シリアルモニタの出力確認

正常な場合の出力例:
```
========================================
Initializing SD card...
========================================
SD Card Pins:
  CS:   GPIO21
  MOSI: GPIO9
  MISO: GPIO8
  SCK:  GPIO7

Initializing SPI bus...
Attempt 1: Default settings
✓ SD card mounted (default)

--- SD Card Information ---
Card Type: SDHC (SD High Capacity)
Card Size: 32000 MB
Total Space: 31000 MB
Used Space: 0 MB
Free Space: 31000 MB

--- Testing FS Access ---
✓ Root directory accessible
✓ Found 0 existing files

========================================
SD card initialized successfully!
========================================
```

異常な場合の出力例と対処法:

#### ケース1: すべての試行が失敗
```
Attempt 1: Default settings
✗ Failed with default settings

Attempt 2: Low speed (4MHz)
✗ Failed with 4MHz

Attempt 3: Very low speed (1MHz)
✗ All attempts failed
```
→ **物理接続の問題**: SDカードの再挿入、XIAO本体の再装着を試す

#### ケース2: カードタイプが検出されない
```
✓ SD card mounted (default)
✗ No SD card detected
```
→ **SDカードの互換性問題**: 別のSDカードを試す

## 推奨SDカード

### 動作確認済み
- **SanDisk Ultra 16GB/32GB** (SDHC)
- **Samsung EVO Plus 32GB** (SDHC)
- **Transcend 8GB/16GB** (SDHC)

### 推奨スペック
- 容量: **2GB～32GB**（FAT32対応範囲）
- 規格: **SDHC** (SD High Capacity)
- クラス: **Class 10** 以上
- フォーマット: **FAT32**

### 非推奨
- ❌ 64GB以上（exFAT形式になりやすい）
- ❌ ノーブランド品（相性問題が発生しやすい）
- ❌ 極端に古いSDカード（接触不良の可能性）
- ❌ microSDHC UHS-I U3など高速規格（互換性問題の可能性）

## デバッグコマンド

### シリアルモニタでの詳細確認

プログラムがSDカード初期化時に以下の情報を表示します：
1. 使用ピン情報
2. 3段階の初期化試行
3. SDカードタイプと容量
4. ファイルシステムアクセステスト
5. 既存ファイル数

この情報を確認することで問題箇所を特定できます。

### 手動テストコード

以下のコードをsetup()に追加して、SDカードの動作を個別テスト可能：

```cpp
// SDカード書き込みテスト
if (sdCardAvailable) {
  File testFile = SD.open("/test.txt", FILE_WRITE);
  if (testFile) {
    testFile.println("SD Card Test OK");
    testFile.close();
    Serial.println("Test file created successfully");
  } else {
    Serial.println("Failed to create test file");
  }
}
```

## よくある質問

### Q: WiFiなしでもSDカードは動作しますか？
A: はい。WiFiとSDカードは独立しています。WiFi未接続でもSDカード保存は動作します。

### Q: SDカードなしでもカメラは動作しますか？
A: はい。SDカードが認識されなくても、カメラとWebサーバーは正常に動作します。自動保存のみ無効化されます。

### Q: 既存のファイルがあるSDカードを使えますか？
A: はい。ただし、ルートディレクトリに大量のファイルがあると初期化が遅くなる可能性があります。

### Q: 書き込みエラーが頻発します
A: SDカードの書き込み禁止スイッチ（ロック）を確認してください。また、SDカードの寿命の可能性もあります。

## さらにサポートが必要な場合

1. シリアルモニタの**完全な出力**をコピー
2. 使用しているSDカードの**メーカー・容量・型番**を確認
3. フォーマット形式（FAT32 / exFAT / NTFS）を確認
4. 写真を撮ってハードウェアの接続状態を確認
