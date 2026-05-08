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

	TArray<FString> SessionIds;
	ActiveRenderTargetSessions.GetKeys(SessionIds);
	for (const FString& SessionId : SessionIds)
	{
		FString IgnoredPath;
		FString IgnoredError;
		StopRenderTargetRecordingInternal(SessionId, IgnoredPath, IgnoredError);
	}

	Super::Deinitialize();
}

void URuntimeRecSubsystem::Tick(float DeltaTime)
{
	if (bRecording && Encoder)
	{
		AccumulatedTime += DeltaTime;
		if (AccumulatedTime >= FrameInterval)
		{
			AccumulatedTime = FMath::Fmod(AccumulatedTime, FrameInterval);

			TArray<FColor> Pixels;
			int32 CapturedWidth = 0;
			int32 CapturedHeight = 0;
			if (CaptureViewportFrame(Pixels, CapturedWidth, CapturedHeight))
			{
				CropFrameToSize(Pixels, CapturedWidth, CapturedHeight, ActiveOptions.Width, ActiveOptions.Height);

				FString EncodeError;
				if (!Encoder->EnqueueFrame(MoveTemp(Pixels), EncodeError))
				{
					SetError(EncodeError);
				}
			}
		}
	}

	TickRenderTargetSessions(DeltaTime);
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

	if (bRecording || HasAnyRenderTargetSessions())
	{
		OutError = TEXT("Another recording is already active.");
		return false;
	}

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
	return StartRenderTargetRecordingInternal(RenderTarget, OutputDirectory, FileName, Options, OutSessionId, OutError);
}

bool URuntimeRecSubsystem::StopRecording(
	const FString& SessionId,
	FString& OutSavedFilePath,
	FString& OutError)
{
	if (bRecording && (SessionId.IsEmpty() || SessionId == CurrentSessionId))
	{
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
		CurrentSessionId.Reset();
		AccumulatedTime = 0.0;
		return true;
	}

	if (SessionId.IsEmpty())
	{
		if (ActiveRenderTargetSessions.Num() == 0)
		{
			OutError = TEXT("No active recording.");
			return false;
		}

		if (ActiveRenderTargetSessions.Num() > 1)
		{
			OutError = TEXT("Multiple render target recordings are active. SessionId is required.");
			return false;
		}

		const FString OnlySessionId = ActiveRenderTargetSessions.CreateConstIterator().Key();
		return StopRenderTargetRecordingInternal(OnlySessionId, OutSavedFilePath, OutError);
	}

	return StopRenderTargetRecordingInternal(SessionId, OutSavedFilePath, OutError);
}

FString URuntimeRecSubsystem::GetRecordingOutputPath(const FString& SessionId) const
{
	if (bRecording && (SessionId.IsEmpty() || SessionId == CurrentSessionId))
	{
		return CurrentOutputPath;
	}

	if (SessionId.IsEmpty())
	{
		if (ActiveRenderTargetSessions.Num() == 1)
		{
			return ActiveRenderTargetSessions.CreateConstIterator().Value().CurrentOutputPath;
		}

		return FString();
	}

	const FRuntimeRecRecordingSession* Session = ActiveRenderTargetSessions.Find(SessionId);
	return Session ? Session->CurrentOutputPath : FString();
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
	if (InputSource != ERuntimeRecInputSource::Viewport)
	{
		OutError = TEXT("This recording path is reserved for viewport recording.");
		return false;
	}

	if (bRecording || HasAnyRenderTargetSessions())
	{
		OutError = TEXT("Another recording is already active.");
		return false;
	}

	Options.FPS = FMath::Clamp(Options.FPS, 1, 240);
	Options.BitrateKbps = FMath::Max(Options.BitrateKbps, 1);

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

bool URuntimeRecSubsystem::CaptureRenderTargetFrame(UTextureRenderTarget2D* RenderTarget, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight)
{
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
	Flags.SetLinearToGamma(!RenderTarget->IsSRGB());

	const bool bRead = Resource->ReadPixels(OutPixels, Flags);
	if (!bRead || OutPixels.Num() != OutWidth * OutHeight)
	{
		SetError(TEXT("Failed to read render target pixels."));
		return false;
	}

	return true;
}

bool URuntimeRecSubsystem::StartRenderTargetRecordingInternal(
	UTextureRenderTarget2D* RenderTarget,
	const FString& OutputDirectory,
	const FString& FileName,
	const FRuntimeRecOptions& Options,
	FString& OutSessionId,
	FString& OutError)
{
	if (bRecording)
	{
		OutError = TEXT("Viewport recording is already active.");
		return false;
	}

	if (!RenderTarget)
	{
		OutError = TEXT("RenderTarget is null.");
		return false;
	}

	FRuntimeRecOptions LocalOptions = Options;
	LocalOptions.FPS = FMath::Clamp(LocalOptions.FPS, 1, 240);
	LocalOptions.BitrateKbps = FMath::Max(LocalOptions.BitrateKbps, 1);
	LocalOptions.Width = RenderTarget->SizeX;
	LocalOptions.Height = RenderTarget->SizeY;
	ForceEvenFrameSize(LocalOptions.Width, LocalOptions.Height);

	if (LocalOptions.Width <= 0 || LocalOptions.Height <= 0)
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

	const FString SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	FRuntimeRecRecordingSession Session;
	Session.SourceRenderTarget = RenderTarget;
	Session.ActiveOptions = LocalOptions;
	Session.CurrentOutputPath = MakeUniqueOutputPath(ResolvedOutputDirectory, FileName);
	Session.FrameInterval = 1.0 / static_cast<double>(LocalOptions.FPS);
	Session.AccumulatedTime = 0.0;

	Session.Encoder = new FRuntimeRecVideoEncoder();
	if (!Session.Encoder->Start(
		Session.CurrentOutputPath,
		Session.ActiveOptions.Width,
		Session.ActiveOptions.Height,
		Session.ActiveOptions.FPS,
		Session.ActiveOptions.BitrateKbps,
		Session.ActiveOptions.bPreferHardwareEncoder,
		Session.ActiveOptions.bAllowFrameDrop,
		OutError))
	{
		SetError(OutError);
		delete Session.Encoder;
		Session.Encoder = nullptr;
		return false;
	}

	ActiveRenderTargetSessions.Add(SessionId, MoveTemp(Session));
	OutSessionId = SessionId;
	return true;
}

bool URuntimeRecSubsystem::StopRenderTargetRecordingInternal(
	const FString& SessionId,
	FString& OutSavedFilePath,
	FString& OutError)
{
	FRuntimeRecRecordingSession* Session = ActiveRenderTargetSessions.Find(SessionId);
	if (!Session)
	{
		OutError = TEXT("SessionId does not match an active render target recording.");
		return false;
	}

	if (Session->Encoder)
	{
		if (!Session->Encoder->Stop(OutError))
		{
			SetError(OutError);
			return false;
		}

		delete Session->Encoder;
		Session->Encoder = nullptr;
	}

	OutSavedFilePath = Session->CurrentOutputPath;
	ClearRenderTargetSession(*Session);
	ActiveRenderTargetSessions.Remove(SessionId);
	return true;
}

void URuntimeRecSubsystem::TickRenderTargetSessions(float DeltaTime)
{
	TArray<FString> SessionIds;
	ActiveRenderTargetSessions.GetKeys(SessionIds);

	TArray<FString> SessionsToRemove;
	for (const FString& SessionId : SessionIds)
	{
		FRuntimeRecRecordingSession* Session = ActiveRenderTargetSessions.Find(SessionId);
		if (!Session || !Session->Encoder)
		{
			SessionsToRemove.Add(SessionId);
			continue;
		}

		Session->AccumulatedTime += DeltaTime;
		if (Session->AccumulatedTime < Session->FrameInterval)
		{
			continue;
		}

		Session->AccumulatedTime = FMath::Fmod(Session->AccumulatedTime, Session->FrameInterval);

		TArray<FColor> Pixels;
		int32 CapturedWidth = 0;
		int32 CapturedHeight = 0;
		if (!CaptureRenderTargetFrame(Session->SourceRenderTarget.Get(), Pixels, CapturedWidth, CapturedHeight))
		{
			SessionsToRemove.Add(SessionId);
			continue;
		}

		CropFrameToSize(Pixels, CapturedWidth, CapturedHeight, Session->ActiveOptions.Width, Session->ActiveOptions.Height);

		FString EncodeError;
		if (!Session->Encoder->EnqueueFrame(MoveTemp(Pixels), EncodeError))
		{
			Session->LastError = EncodeError;
			SetError(EncodeError);
			SessionsToRemove.Add(SessionId);
		}
	}

	for (const FString& SessionId : SessionsToRemove)
	{
		FString IgnoredPath;
		FString IgnoredError;
		StopRenderTargetRecordingInternal(SessionId, IgnoredPath, IgnoredError);
	}
}

bool URuntimeRecSubsystem::HasAnyRenderTargetSessions() const
{
	return ActiveRenderTargetSessions.Num() > 0;
}

void URuntimeRecSubsystem::ClearRenderTargetSession(FRuntimeRecRecordingSession& Session)
{
	if (Session.Encoder)
	{
		delete Session.Encoder;
		Session.Encoder = nullptr;
	}

	Session.SourceRenderTarget.Reset();
	Session.CurrentOutputPath.Reset();
	Session.LastError.Reset();
	Session.AccumulatedTime = 0.0;
	Session.FrameInterval = 1.0 / 30.0;
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
