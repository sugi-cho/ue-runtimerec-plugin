#include "RuntimeRecSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "RuntimeRecVideoEncoder.h"

void URuntimeRecSubsystem::Deinitialize()
{
	if (bRecording)
	{
		FString IgnoredPath;
		FString IgnoredError;
		StopRecording(CurrentSessionId, IgnoredPath, IgnoredError);
	}

	Super::Deinitialize();
}

void URuntimeRecSubsystem::Tick(float DeltaTime)
{
	if (!bRecording || !Encoder)
	{
		return;
	}

	AccumulatedTime += DeltaTime;
	if (AccumulatedTime < FrameInterval)
	{
		return;
	}

	AccumulatedTime = FMath::Fmod(AccumulatedTime, FrameInterval);

	TArray<FColor> Pixels;
	int32 CapturedWidth = 0;
	int32 CapturedHeight = 0;
	const bool bCaptured = Source == ERuntimeRecInputSource::RenderTarget
		? CaptureRenderTargetFrame(Pixels, CapturedWidth, CapturedHeight)
		: CaptureViewportFrame(Pixels, CapturedWidth, CapturedHeight);

	if (!bCaptured)
	{
		return;
	}

	CropFrameToSize(Pixels, CapturedWidth, CapturedHeight, ActiveOptions.Width, ActiveOptions.Height);

	FString EncodeError;
	if (!Encoder->EnqueueFrame(MoveTemp(Pixels), EncodeError))
	{
		SetError(EncodeError);
	}
}

TStatId URuntimeRecSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(URuntimeRecSubsystem, STATGROUP_Tickables);
}

bool URuntimeRecSubsystem::IsTickable() const
{
	return !IsTemplate();
}

bool URuntimeRecSubsystem::StartViewportRecording(
	const FString& OutputDirectory,
	const FString& FileName,
	const FRuntimeRecOptions& Options,
	FString& OutSessionId,
	FString& OutError)
{
	FRuntimeRecOptions ViewportOptions = Options;

	if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
	{
		OutError = TEXT("Game viewport is not available.");
		return false;
	}

	const FIntPoint ViewportSize = GEngine->GameViewport->Viewport->GetSizeXY();
	ViewportOptions.Width = ViewportSize.X;
	ViewportOptions.Height = ViewportSize.Y;
	ForceEvenFrameSize(ViewportOptions.Width, ViewportOptions.Height);

	return StartRecordingInternal(ERuntimeRecInputSource::Viewport, nullptr, OutputDirectory, FileName, ViewportOptions, OutSessionId, OutError);
}

bool URuntimeRecSubsystem::StartRenderTargetRecording(
	UTextureRenderTarget2D* RenderTarget,
	const FString& OutputDirectory,
	const FString& FileName,
	const FRuntimeRecOptions& Options,
	FString& OutSessionId,
	FString& OutError)
{
	return StartRecordingInternal(ERuntimeRecInputSource::RenderTarget, RenderTarget, OutputDirectory, FileName, Options, OutSessionId, OutError);
}

bool URuntimeRecSubsystem::StopRecording(
	const FString& SessionId,
	FString& OutSavedFilePath,
	FString& OutError)
{
	if (!bRecording)
	{
		OutError = TEXT("No active recording.");
		return false;
	}

	if (!SessionId.IsEmpty() && SessionId != CurrentSessionId)
	{
		OutError = TEXT("SessionId does not match the active recording.");
		return false;
	}

	if (Encoder)
	{
		if (!Encoder->Stop(OutError))
		{
			SetError(OutError);
			return false;
		}
		delete Encoder;
		Encoder = nullptr;
	}

	OutSavedFilePath = CurrentOutputPath;
	bRecording = false;
	SourceRenderTarget.Reset();
	CurrentSessionId.Reset();
	AccumulatedTime = 0.0;
	return true;
}

bool URuntimeRecSubsystem::StartRecordingInternal(
	ERuntimeRecInputSource InputSource,
	UTextureRenderTarget2D* RenderTarget,
	const FString& OutputDirectory,
	const FString& FileName,
	FRuntimeRecOptions Options,
	FString& OutSessionId,
	FString& OutError)
{
	if (bRecording)
	{
		OutError = TEXT("Recording is already active.");
		return false;
	}

	Options.FPS = FMath::Clamp(Options.FPS, 1, 240);
	Options.BitrateKbps = FMath::Max(Options.BitrateKbps, 1);

	if (InputSource == ERuntimeRecInputSource::RenderTarget)
	{
		if (!RenderTarget)
		{
			OutError = TEXT("RenderTarget is null.");
			return false;
		}

		Options.Width = RenderTarget->SizeX;
		Options.Height = RenderTarget->SizeY;
	}

	ForceEvenFrameSize(Options.Width, Options.Height);

	if (Options.Width <= 0 || Options.Height <= 0)
	{
		OutError = TEXT("Recording width and height must be greater than zero.");
		return false;
	}

	const FString ResolvedOutputDirectory = ResolveOutputDirectory(OutputDirectory);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.CreateDirectoryTree(*ResolvedOutputDirectory))
	{
		OutError = FString::Printf(TEXT("Failed to create output directory: %s"), *ResolvedOutputDirectory);
		return false;
	}

	CurrentOutputPath = MakeUniqueOutputPath(ResolvedOutputDirectory, FileName);
	CurrentSessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	LastError.Reset();
	Source = InputSource;
	SourceRenderTarget = RenderTarget;
	ActiveOptions = Options;
	FrameInterval = 1.0 / static_cast<double>(Options.FPS);
	AccumulatedTime = 0.0;

	Encoder = new FRuntimeRecVideoEncoder();
	if (!Encoder->Start(
		CurrentOutputPath,
		ActiveOptions.Width,
		ActiveOptions.Height,
		ActiveOptions.FPS,
		ActiveOptions.BitrateKbps,
		ActiveOptions.bPreferHardwareEncoder,
		ActiveOptions.bAllowFrameDrop,
		OutError))
	{
		SetError(OutError);
		delete Encoder;
		Encoder = nullptr;
		CurrentOutputPath.Reset();
		CurrentSessionId.Reset();
		SourceRenderTarget.Reset();
		return false;
	}

	bRecording = true;
	OutSessionId = CurrentSessionId;
	return true;
}

bool URuntimeRecSubsystem::CaptureViewportFrame(TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight)
{
	if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
	{
		SetError(TEXT("Game viewport is not available."));
		return false;
	}

	FViewport* Viewport = GEngine->GameViewport->Viewport;
	OutWidth = Viewport->GetSizeXY().X;
	OutHeight = Viewport->GetSizeXY().Y;

	FReadSurfaceDataFlags Flags(RCM_UNorm);
	Flags.SetLinearToGamma(false);
	const bool bRead = Viewport->ReadPixels(OutPixels, Flags);
	if (!bRead || OutPixels.Num() != OutWidth * OutHeight)
	{
		SetError(TEXT("Failed to read viewport pixels."));
		return false;
	}

	return true;
}

bool URuntimeRecSubsystem::CaptureRenderTargetFrame(TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight)
{
	UTextureRenderTarget2D* RenderTarget = SourceRenderTarget.Get();
	if (!RenderTarget)
	{
		SetError(TEXT("RenderTarget is no longer valid."));
		return false;
	}

	FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
	if (!Resource)
	{
		SetError(TEXT("RenderTarget resource is not available."));
		return false;
	}

	OutWidth = RenderTarget->SizeX;
	OutHeight = RenderTarget->SizeY;
	FReadSurfaceDataFlags Flags(RCM_UNorm);
	Flags.SetLinearToGamma(false);

	const bool bRead = Resource->ReadPixels(OutPixels, Flags);
	if (!bRead || OutPixels.Num() != OutWidth * OutHeight)
	{
		SetError(TEXT("Failed to read render target pixels."));
		return false;
	}

	return true;
}

void URuntimeRecSubsystem::SetError(const FString& Error)
{
	LastError = Error;
	UE_LOG(LogTemp, Warning, TEXT("RuntimeRec: %s"), *Error);
}

FString URuntimeRecSubsystem::ResolveOutputDirectory(const FString& OutputDirectory)
{
	if (!OutputDirectory.IsEmpty())
	{
		return FPaths::ConvertRelativePathToFull(OutputDirectory);
	}

	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RuntimeRec"), TEXT("Recordings"));
}

FString URuntimeRecSubsystem::MakeUniqueOutputPath(const FString& OutputDirectory, const FString& FileName)
{
	const FString BaseFileName = SanitizeBaseFileName(FileName);
	FString Candidate = FPaths::Combine(OutputDirectory, BaseFileName + TEXT(".mp4"));

	if (!FPaths::FileExists(Candidate))
	{
		return Candidate;
	}

	for (int32 Index = 1; Index < TNumericLimits<int32>::Max(); ++Index)
	{
		Candidate = FPaths::Combine(OutputDirectory, FString::Printf(TEXT("%s_%03d.mp4"), *BaseFileName, Index));
		if (!FPaths::FileExists(Candidate))
		{
			return Candidate;
		}
	}

	return Candidate;
}

FString URuntimeRecSubsystem::SanitizeBaseFileName(const FString& FileName)
{
	FString BaseFileName = FileName;
	if (BaseFileName.IsEmpty())
	{
		BaseFileName = FString::Printf(TEXT("RuntimeRec_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}

	BaseFileName = FPaths::GetBaseFilename(BaseFileName);
	const TCHAR InvalidChars[] = TEXT("\\/:*?\"<>|");
	for (const TCHAR InvalidChar : InvalidChars)
	{
		if (InvalidChar == TEXT('\0'))
		{
			break;
		}

		BaseFileName.ReplaceCharInline(InvalidChar, TEXT('_'));
	}

	return BaseFileName;
}

void URuntimeRecSubsystem::ForceEvenFrameSize(int32& Width, int32& Height)
{
	Width = FMath::Max(Width & ~1, 2);
	Height = FMath::Max(Height & ~1, 2);
}

void URuntimeRecSubsystem::CropFrameToSize(TArray<FColor>& Pixels, int32 SourceWidth, int32 SourceHeight, int32 TargetWidth, int32 TargetHeight)
{
	if (SourceWidth == TargetWidth && SourceHeight == TargetHeight)
	{
		return;
	}

	if (SourceWidth < TargetWidth || SourceHeight < TargetHeight)
	{
		Pixels.Reset();
		return;
	}

	TArray<FColor> CroppedPixels;
	CroppedPixels.SetNumUninitialized(TargetWidth * TargetHeight);

	for (int32 Y = 0; Y < TargetHeight; ++Y)
	{
		const int32 SourceOffset = Y * SourceWidth;
		const int32 TargetOffset = Y * TargetWidth;
		FMemory::Memcpy(
			CroppedPixels.GetData() + TargetOffset,
			Pixels.GetData() + SourceOffset,
			TargetWidth * sizeof(FColor));
	}

	Pixels = MoveTemp(CroppedPixels);
}
