# RuntimeRec

Unreal Engine の Runtime 実行中に、Viewport または `UTextureRenderTarget2D` の映像を MP4 として保存するための Runtime プラグインです。

## 対応環境

- Unreal Engine 5.7 でビルド確認済み
- Windows / Win64
- Development / Shipping ビルド対応
- 映像エンコードは Windows Media Foundation の H.264 / MP4 を使用

## 有効化

このプロジェクトでは、`RuntimeRecPlugin.uproject` に以下のプラグイン設定を追加済みです。

```json
{
  "Name": "RuntimeRec",
  "Enabled": true
}
```

別プロジェクトへ移す場合は、この `RuntimeRec` フォルダを対象プロジェクトの `Plugins` フォルダへ配置し、`.uproject` で有効化してください。

## Blueprint API

カテゴリは `UE_RuntimeRec` です。

### Viewport 録画開始

```cpp
StartViewportRecording(
    OutputDirectory,
    FileName,
    FPS,
    BitrateKbps,
    bIncludeUI,
    OutSessionId,
    OutError)
```

現在のゲーム Viewport を録画します。

- `OutputDirectory`: 出力先ディレクトリ。空文字の場合は既定保存先を使用します。
- `FileName`: 出力ファイル名。拡張子は不要です。空文字の場合は自動命名します。
- `FPS`: 録画FPS。
- `BitrateKbps`: 映像ビットレート。
- `bIncludeUI`: APIとして保持しています。現実装のViewport取得は画面出力を読むため、基本的に表示結果に準じます。
- `OutSessionId`: 停止時に使うセッションID。
- `OutError`: 失敗理由。

録画解像度は録画開始時点の Viewport サイズを自動的に使用します。

注意:

- H.264 の都合上、Viewport の幅と高さは偶数である必要があります。

### RenderTarget 録画開始

```cpp
StartRenderTargetRecording(
    RenderTarget,
    OutputDirectory,
    FileName,
    FPS,
    BitrateKbps,
    OutSessionId,
    OutError)
```

`UTextureRenderTarget2D` の内容を録画します。

- `RenderTarget`: 録画対象の `TextureRenderTarget2D`。
- `OutputDirectory`: 出力先ディレクトリ。空文字の場合は既定保存先を使用します。
- `FileName`: 出力ファイル名。拡張子は不要です。空文字の場合は自動命名します。
- `FPS`: 録画FPS。
- `BitrateKbps`: 映像ビットレート。
- `OutSessionId`: 停止時に使うセッションID。
- `OutError`: 失敗理由。

RenderTarget の `SizeX` / `SizeY` が録画解像度として使われます。

注意:

- H.264 の都合上、RenderTarget の幅と高さは偶数である必要があります。

### 録画停止

```cpp
StopRecording(SessionId, OutSavedFilePath, OutError)
```

現在の録画を停止し、MP4ファイルを確定します。

- `SessionId`: 開始時に返されたID。空文字でも現在の録画を停止できます。
- `OutSavedFilePath`: 保存されたMP4のフルパス。
- `OutError`: 失敗理由。

### 状態取得

```cpp
IsRecording()
GetCurrentOutputPath()
GetLastError()
```

- `IsRecording`: 録画中かを返します。
- `GetCurrentOutputPath`: 現在の出力予定パスを返します。
- `GetLastError`: 最後に発生したエラー文字列を返します。

## 既定保存先

`OutputDirectory` が空の場合、以下へ保存します。

```text
<ProjectSavedDir>/RuntimeRec/Recordings
```

ファイル名が空の場合は次の形式で自動生成します。

```text
RuntimeRec_YYYYMMDD_HHMMSS.mp4
```

同名ファイルが存在する場合は `_001`, `_002` のように連番を付与します。

## 推奨設定例

### Viewport

- `FPS`: 30
- `BitrateKbps`: 12000
- `bIncludeUI`: true

### RenderTarget

- RenderTarget解像度: 1920 x 1080
- `FPS`: 30
- `BitrateKbps`: 12000

## 実装上の注意

- フレーム取得はゲームスレッド上で `ReadPixels` を使います。
- エンコードは別スレッドで行います。
- フレームキューが詰まった場合、既定では古いフレームを破棄して遅延増加を抑えます。
- 音声録音は未対応です。
- 複数同時録画は未対応です。
- Windows以外のMP4エンコードは未対応です。

## 動作確認済みビルド

以下のビルドが成功することを確認済みです。

```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" RuntimeRecPluginEditor Win64 Development -Project="C:\Users\stu\sources\sugi-cho\UE_RuntimeRec\RuntimeRecPlugin\RuntimeRecPlugin.uproject" -WaitMutex -NoHotReloadFromIDE
& "C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" RuntimeRecPlugin Win64 Development -Project="C:\Users\stu\sources\sugi-cho\UE_RuntimeRec\RuntimeRecPlugin\RuntimeRecPlugin.uproject" -WaitMutex -NoHotReloadFromIDE
& "C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" RuntimeRecPlugin Win64 Shipping -Project="C:\Users\stu\sources\sugi-cho\UE_RuntimeRec\RuntimeRecPlugin\RuntimeRecPlugin.uproject" -WaitMutex -NoHotReloadFromIDE
```

Editor起動中に Live Coding が有効な場合、通常ビルドが止まることがあります。その場合は Editor 側で Live Coding を停止するか、上記のように `-NoHotReloadFromIDE` を指定してください。
