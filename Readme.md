# RuntimeRec

Unreal Engine の Runtime 実行中に、Viewport または `UTextureRenderTarget2D` の映像を MP4 として保存するための Runtime プラグインです。

## 対応環境

- Unreal Engine 5.7 以降
- Windows / Win64
- Development / Shipping ビルド対応
- RenderTarget 録画は可能な場合、Direct NVENC の GPU H.264 エンコードを使用
- GPU エンコードが使えない場合は Windows Media Foundation の H.264 / MP4 経路にフォールバック

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
- `FPS`: 録画FPS。既定値は `30` です。
- `BitrateKbps`: 映像ビットレート。既定値は `12000` です。
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
- `FPS`: 録画FPS。既定値は `30` です。
- `BitrateKbps`: 映像ビットレート。既定値は `12000` です。
- `OutSessionId`: 停止時に使うセッションID。
- `OutError`: 失敗理由。

RenderTarget の `SizeX` / `SizeY` が録画解像度として使われます。

注意:

- H.264 の都合上、RenderTarget の幅と高さは偶数である必要があります。

### Camera から RenderTarget を書き込むアクター

`RuntimeRecCameraCaptureActor` をレベルに配置すると、指定した `CameraActor` の映像を `TextureRenderTarget2D` に書き込みます。通常の運用では録画そのものは行わず、`RuntimeRec Recording Group` からその RenderTarget をまとめて録画します。

使い方:

1. `RuntimeRecCameraCaptureActor` をレベルに配置します。
2. `SourceCamera` に録画したい `CameraActor` または `CineCameraActor` を設定します。
3. `TargetRenderTarget` を設定するか、空のままにして自動生成を使います。
4. 必要なら `RenderTargetWidth` / `RenderTargetHeight` を設定します。
5. `bIncludeCameraPostProcess` でカメラの PostProcess を反映するか切り替えます。
6. 必要に応じて `OutputDirectory` / `FileName` を設定します。
7. PIE を開始しなくても、レベルビューポート表示中に自動更新します。

補足:

- `TargetRenderTarget` が未設定の場合、`RenderTargetWidth` / `RenderTargetHeight` から自動生成します。
- 自動生成された `RenderTarget` はランタイム用の transient オブジェクトです。
- 自動生成された `RenderTarget` は LDR / sRGB 前提で作成します。暗く見える場合は、既存の `RenderTarget` の `sRGB` 設定も確認してください。
- `bIncludeCameraPostProcess=true` の場合は、カメラの PostProcessSettings を反映します。
- `bIncludeCameraPostProcess=false` の場合は、PostProcess を無効化した映像になります。
- UI はこの経路でも録画されません。

### 同時録画用 Actor

`RuntimeRec Recording Group` をレベルに配置すると、複数の `RenderTarget` と `RuntimeRecCameraCaptureActor` をまとめて同時録画できます。

使い方:

1. `RuntimeRec Recording Group` をレベルに配置します。
2. `TargetRenderTargets` に直接録画したい `RenderTarget` を設定します。
3. `TargetCameraCaptureActors` に `RuntimeRecCameraCaptureActor` を設定します。
4. `OutputDirectory` と `FileNamePrefix` を設定します。
5. `FPS` と `BitrateKbps` を設定します。
6. `StartRecording` を呼ぶと、設定済みの対象をすべて同時開始します。
7. `StopRecording` を呼ぶと、設定済みの対象をすべて同時停止します。

補足:

- `FPS` の既定値は `30`、`BitrateKbps` の既定値は `12000` です。
- 同時録画はこの Actor からのみ実行する前提です。
- 1つの Group 内では、同じ `FPS` と `BitrateKbps` を全対象に上書きします。
- `RuntimeRecCameraCaptureActor` は録画前段の RenderTarget 生成・更新担当です。

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
- GPU エンコードを使う場合は D3D12 / NVIDIA GPU / `PF_B8G8R8A8` / 非MSAAを推奨します

### Camera Capture Actor

- `RenderTargetWidth`: 1920
- `RenderTargetHeight`: 1080
- `bIncludeCameraPostProcess`: true

### Recording Group Actor

- `FPS`: 30
- `BitrateKbps`: 12000
- `TargetRenderTargets`: 必要な分だけ設定
- `TargetCameraCaptureActors`: 必要な分だけ設定

## 実装上の注意

- RenderTarget 録画は D3D12 / `PF_B8G8R8A8` / 非MSAA の場合、Direct NVENC による GPU エンコードを優先します。これにより Raw FullHD フレームの GPU -> CPU 転送を避け、CPU 側へ戻るデータを圧縮済み H.264 packet に抑えます。
- GPU エンコードが使えない場合、RenderTarget 録画は `FRHIGPUTextureReadback` による非同期 readback を使います。未対応形式などの場合は既存の `ReadPixels` 経路にフォールバックします。
- GPU エンコードは `RuntimeRec.RenderTarget.GpuVideoEncoder 0` で無効化できます。
- GPU エンコードの同時利用数は `RuntimeRec.RenderTarget.MaxGpuVideoEncoders` で制限できます。既定値は `8` で、上限を超えた RenderTarget 録画は async readback 経路へ自動フォールバックします。
- Viewport 録画は現在も `ReadPixels` を使います。
- エンコードは別スレッドで行います。
- フレームキューが詰まった場合、既定では古いフレームを破棄して遅延増加を抑えます。
- 音声録音は未対応です。
- Viewport 録画と同時に複数録画を行うことはできません。
- 複数同時録画は RenderTarget 系のみ対応です。
- Windows以外のMP4エンコードは未対応です。
