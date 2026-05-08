# RuntimeRec

Unreal Engine の Runtime 実行中に、Viewport または `UTextureRenderTarget2D` の映像を MP4 として保存するための Runtime プラグインです。

## 対応環境

- Unreal Engine 5.7 以降
- Windows / Win64
- Development / Shipping ビルド対応
- 映像エンコードは Windows Media Foundation の H.264 / MP4 を使用

## 有効化

対象プロジェクトの `.uproject` に以下のプラグイン設定を追加してください。

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
    OutSessionId,
    OutError)
```

現在のゲーム Viewport を録画します。

- `OutputDirectory`: 出力先ディレクトリ。空文字の場合は既定保存先を使用します。
- `FileName`: 出力ファイル名。拡張子は不要です。空文字の場合は自動命名します。
- `FPS`: 録画FPS。
- `BitrateKbps`: 映像ビットレート。
- `OutSessionId`: 停止時に使うセッションID。
- `OutError`: 失敗理由。

録画解像度は録画開始時点の Viewport サイズを自動的に使用します。

注意:

- H.264 の都合上、Viewport の幅と高さは偶数である必要があります。
- Viewport 録画では Slate UI や UMG オーバーレイは録画されません。

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

### Camera から RenderTarget を自動生成するアクター

`RuntimeRecCameraCaptureActor` をレベルに配置すると、指定した `CameraActor` の映像を `TextureRenderTarget2D` に書き込みできます。

使い方:

1. `RuntimeRecCameraCaptureActor` をレベルに配置します。
2. `SourceCamera` に録画したい `CameraActor` または `CineCameraActor` を設定します。
3. `TargetRenderTarget` を設定するか、空のままにして自動生成を使います。
4. 必要なら `RenderTargetWidth` / `RenderTargetHeight` を設定します。
5. `bIncludeCameraPostProcess` でカメラの PostProcess を反映するか切り替えます。
6. 生成された `RenderTarget` を `StartRenderTargetRecording` に渡して録画します。
7. PIE を開始しなくても、レベルビューポート表示中に自動更新します。

補足:

- `TargetRenderTarget` が未設定の場合、`RenderTargetWidth` / `RenderTargetHeight` から自動生成します。
- 自動生成された `RenderTarget` はランタイム用の transient オブジェクトです。
- 自動生成された `RenderTarget` は LDR / sRGB 前提で作成します。暗く見える場合は、既存の `RenderTarget` の `sRGB` 設定も確認してください。
- `bIncludeCameraPostProcess=true` の場合は、カメラの PostProcessSettings を反映します。
- `bIncludeCameraPostProcess=false` の場合は、PostProcess を無効化した映像になります。
- UI はこの経路でも録画されません。

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

### RenderTarget

- RenderTarget解像度: 1920 x 1080
- `FPS`: 30
- `BitrateKbps`: 12000
- 既存の RenderTarget を使う場合は `sRGB` 設定を確認してください

### Camera Capture Actor

- `RenderTargetWidth`: 1920
- `RenderTargetHeight`: 1080
- `bIncludeCameraPostProcess`: true

## 実装上の注意

- フレーム取得はゲームスレッド上で `ReadPixels` を使います。
- エンコードは別スレッドで行います。
- フレームキューが詰まった場合、既定では古いフレームを破棄して遅延増加を抑えます。
- 音声録音は未対応です。
- 複数同時録画は未対応です。
- Windows以外のMP4エンコードは未対応です。
