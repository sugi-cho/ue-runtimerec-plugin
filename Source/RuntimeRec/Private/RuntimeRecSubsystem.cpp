#include "RuntimeRecSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RuntimeRecGpuVideoEncoder.h"
#include "RuntimeRecVideoEncoder.h"
#include "TextureResource.h"

namespace
{
	constexpr int32 RuntimeRecMaxPendingReadbacks = 3;
	constexpr int32 RuntimeRecMaxPendingGpuEncodes = 3;

	void CopyReadbackRowsToPixels(
		const void* SourceData,
		int32 SourceRowPitchInPixels,
		int32 Width,
		int32 Height,
		TArray<FColor>& OutPixels)
	{
		OutPixels.SetNumUninitialized(Width * Height);

		const FColor* SourcePixels = static_cast<const FColor*>(SourceData);
		FColor* DestinationPixels = OutPixels.GetData();

		if (SourceRowPitchInPixels == Width)
		{
			FMemory::Memcpy(DestinationPixels, SourcePixels, Width * Height * sizeof(FColor));
			return;
		}

		for (int32 RowIndex = 0; RowIndex < Height; ++RowIndex)
		{
			FMemory::Memcpy(
				DestinationPixels + RowIndex * Width,
				SourcePixels + RowIndex * SourceRowPitchInPixels,
				Width * sizeof(FColor));
		}
	}
}

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

	const bool bRenderTargetCanUseGpuEncoder =
		RenderTarget->GetFormat() == PF_B8G8R8A8 &&
		RenderTarget->GetSampleCount() == ETextureRenderTargetSampleCount::RTSC_1 &&
		RenderTarget->SizeX == LocalOptions.Width &&
		RenderTarget->SizeY == LocalOptions.Height;

	if (bRenderTargetCanUseGpuEncoder && FRuntimeRecGpuVideoEncoder::IsPreferred())
	{
		Session.GpuEncoder = MakeShared<FRuntimeRecGpuVideoEncoder, ESPMode::ThreadSafe>();
		FString GpuEncoderError;
		if (!Session.GpuEncoder->Start(
			Session.CurrentOutputPath,
			Session.ActiveOptions.Width,
			Session.ActiveOptions.Height,
			Session.ActiveOptions.FPS,
			Session.ActiveOptions.BitrateKbps,
			GpuEncoderError))
		{
			UE_LOG(LogTemp, Warning, TEXT("RuntimeRec: GPU encoder unavailable; falling back to async readback. %s"), *GpuEncoderError);
			Session.GpuEncoder.Reset();
		}
		else
		{
			UE_LOG(LogTemp, Display, TEXT("RuntimeRec: Using GPU video encoder for RenderTarget recording."));
		}
	}

	if (!Session.GpuEncoder.IsValid())
	{
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
	}

	if (!Session.Encoder && !Session.GpuEncoder.IsValid())
	{
		OutError = TEXT("Failed to start a recording encoder.");
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

	UE_LOG(
		LogTemp,
		Display,
		TEXT("RuntimeRec: StopRenderTargetRecordingInternal begin [Session=%s] Gpu=%d Cpu=%d PendingReadbacks=%d PendingGpuEncodes=%d"),
		*SessionId,
		Session->GpuEncoder.IsValid() ? 1 : 0,
		Session->Encoder ? 1 : 0,
		Session->PendingReadbacks.Num(),
		Session->PendingGpuEncodes.Num());

	if (Session->GpuEncoder.IsValid())
	{
		FlushRenderingCommands();
		UE_LOG(LogTemp, Display, TEXT("RuntimeRec: Stopping GPU encoder [Session=%s]."), *SessionId);
		if (!Session->GpuEncoder->Stop(OutError))
		{
			SetError(OutError);
			return false;
		}
		UE_LOG(LogTemp, Display, TEXT("RuntimeRec: GPU encoder stopped [Session=%s]."), *SessionId);
		Session->GpuEncoder.Reset();
		FlushRenderingCommands();
	}

	if (Session->Encoder)
	{
		UE_LOG(LogTemp, Display, TEXT("RuntimeRec: Stopping CPU encoder [Session=%s]."), *SessionId);
		if (!Session->Encoder->Stop(OutError))
		{
			SetError(OutError);
			return false;
		}

		delete Session->Encoder;
		Session->Encoder = nullptr;
		UE_LOG(LogTemp, Display, TEXT("RuntimeRec: CPU encoder stopped [Session=%s]."), *SessionId);
	}

	OutSavedFilePath = Session->CurrentOutputPath;
	ClearRenderTargetSession(*Session);
	ActiveRenderTargetSessions.Remove(SessionId);
	UE_LOG(LogTemp, Display, TEXT("RuntimeRec: StopRenderTargetRecordingInternal end [Session=%s]."), *SessionId);
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
		if (!Session || (!Session->Encoder && !Session->GpuEncoder.IsValid()))
		{
			SessionsToRemove.Add(SessionId);
			continue;
		}

		PollRenderTargetGpuEncodes(SessionId, *Session, SessionsToRemove);
		if (SessionsToRemove.Contains(SessionId))
		{
			continue;
		}

		PollRenderTargetReadbacks(SessionId, *Session, SessionsToRemove);
		if (SessionsToRemove.Contains(SessionId))
		{
			continue;
		}

		Session->AccumulatedTime += DeltaTime;
		if (Session->AccumulatedTime < Session->FrameInterval)
		{
			continue;
		}

		Session->AccumulatedTime = FMath::Fmod(Session->AccumulatedTime, Session->FrameInterval);

		FString CaptureError;
		bool bCanUseReadPixelsFallback = false;
		bool bQueuedFrame = false;
		if (Session->GpuEncoder.IsValid())
		{
			bQueuedFrame = QueueRenderTargetGpuEncode(*Session, CaptureError, bCanUseReadPixelsFallback);
		}
		else
		{
			bQueuedFrame = QueueRenderTargetReadback(*Session, CaptureError, bCanUseReadPixelsFallback);
		}

		if (!bQueuedFrame)
		{
			if (!bCanUseReadPixelsFallback || !CaptureRenderTargetFrameFallback(*Session, CaptureError))
			{
				Session->LastError = CaptureError;
				SetError(CaptureError);
				SessionsToRemove.Add(SessionId);
			}
		}
	}

	for (const FString& SessionId : SessionsToRemove)
	{
		FString IgnoredPath;
		FString IgnoredError;
		StopRenderTargetRecordingInternal(SessionId, IgnoredPath, IgnoredError);
	}
}

void URuntimeRecSubsystem::PollRenderTargetReadbacks(
	const FString& SessionId,
	FRuntimeRecRecordingSession& Session,
	TArray<FString>& SessionsToRemove)
{
	for (int32 RequestIndex = 0; RequestIndex < Session.PendingReadbacks.Num();)
	{
		TSharedPtr<FRuntimeRecReadbackRequest, ESPMode::ThreadSafe> Request = Session.PendingReadbacks[RequestIndex];
		if (!Request.IsValid() || Request->bCancelled)
		{
			Session.PendingReadbacks.RemoveAt(RequestIndex);
			continue;
		}

		if (Request->bSkipped)
		{
			Session.PendingReadbacks.RemoveAt(RequestIndex);
			continue;
		}

		if (Request->bHadError)
		{
			Session.LastError = Request->Error.IsEmpty() ? TEXT("Async render target readback failed.") : Request->Error;
			SetError(Session.LastError);
			SessionsToRemove.AddUnique(SessionId);
			return;
		}

		if (Request->bReadyForEncode)
		{
			FString EncodeError;
			if (!Session.Encoder->EnqueueFrame(MoveTemp(Request->Pixels), EncodeError))
			{
				Session.LastError = EncodeError;
				SetError(EncodeError);
				SessionsToRemove.AddUnique(SessionId);
				return;
			}

			Session.PendingReadbacks.RemoveAt(RequestIndex);
			continue;
		}

		if (!Request->bCheckQueued)
		{
			Request->bCheckQueued = true;
			ENQUEUE_RENDER_COMMAND(RuntimeRecPollRenderTargetReadback)(
				[Request](FRHICommandListImmediate& RHICmdList)
				{
					if (Request->bCancelled)
					{
						Request->bCheckQueued = false;
						return;
					}

					if (!Request->Readback.IsValid())
					{
						Request->Error = TEXT("Async readback object is not available.");
						Request->bHadError = true;
						Request->bCheckQueued = false;
						return;
					}

					if (!Request->Readback->IsReady())
					{
						Request->bCheckQueued = false;
						return;
					}

					int32 RowPitchInPixels = 0;
					int32 BufferHeight = 0;
					void* SourceData = Request->Readback->Lock(RowPitchInPixels, &BufferHeight);
					if (!SourceData || RowPitchInPixels < Request->Width || BufferHeight < Request->Height)
					{
						if (SourceData)
						{
							Request->Readback->Unlock();
						}

						Request->Error = TEXT("Async readback returned an invalid buffer.");
						Request->bHadError = true;
						Request->bCheckQueued = false;
						return;
					}

					CopyReadbackRowsToPixels(
						SourceData,
						RowPitchInPixels,
						Request->Width,
						Request->Height,
						Request->Pixels);

					Request->Readback->Unlock();
					Request->Readback.Reset();
					Request->bReadyForEncode = true;
					Request->bCheckQueued = false;
				});
		}

		++RequestIndex;
	}
}

void URuntimeRecSubsystem::PollRenderTargetGpuEncodes(
	const FString& SessionId,
	FRuntimeRecRecordingSession& Session,
	TArray<FString>& SessionsToRemove)
{
	for (int32 RequestIndex = 0; RequestIndex < Session.PendingGpuEncodes.Num();)
	{
		TSharedPtr<FRuntimeRecGpuEncodeRequest, ESPMode::ThreadSafe> Request = Session.PendingGpuEncodes[RequestIndex];
		if (!Request.IsValid() || Request->bCancelled)
		{
			Session.PendingGpuEncodes.RemoveAt(RequestIndex);
			continue;
		}

		if (Request->bSkipped || Request->bDone)
		{
			Session.PendingGpuEncodes.RemoveAt(RequestIndex);
			continue;
		}

		if (Request->bHadError)
		{
			const FString GpuError = Request->Error.IsEmpty() ? TEXT("GPU render target encode failed.") : Request->Error;
			FString FallbackError;
			if (!FallbackRenderTargetSessionToReadback(Session, GpuError, FallbackError))
			{
				Session.LastError = FallbackError;
				SetError(Session.LastError);
				SessionsToRemove.AddUnique(SessionId);
			}
			return;
		}

		++RequestIndex;
	}
}

bool URuntimeRecSubsystem::QueueRenderTargetGpuEncode(
	FRuntimeRecRecordingSession& Session,
	FString& OutError,
	bool& bOutCanUseReadPixelsFallback)
{
	bOutCanUseReadPixelsFallback = false;

	UTextureRenderTarget2D* RenderTarget = Session.SourceRenderTarget.Get();
	if (!RenderTarget)
	{
		OutError = TEXT("RenderTarget is no longer valid.");
		return false;
	}

	if (!Session.GpuEncoder.IsValid())
	{
		OutError = TEXT("GPU encoder is not available.");
		return false;
	}

	FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
	if (!Resource)
	{
		OutError = TEXT("RenderTarget resource is not available.");
		return false;
	}

	if (Session.PendingGpuEncodes.Num() >= RuntimeRecMaxPendingGpuEncodes)
	{
		if (!Session.ActiveOptions.bAllowFrameDrop)
		{
			OutError = TEXT("GPU encode queue is full.");
			return false;
		}

		if (TSharedPtr<FRuntimeRecGpuEncodeRequest, ESPMode::ThreadSafe> DroppedRequest = Session.PendingGpuEncodes[0])
		{
			DroppedRequest->bCancelled = true;
		}
		Session.PendingGpuEncodes.RemoveAt(0);
	}

	TSharedPtr<FRuntimeRecGpuEncodeRequest, ESPMode::ThreadSafe> Request = MakeShared<FRuntimeRecGpuEncodeRequest, ESPMode::ThreadSafe>();
	TSharedPtr<FRuntimeRecGpuVideoEncoder, ESPMode::ThreadSafe> GpuEncoder = Session.GpuEncoder;
	const int64 FrameIndex = Session.NextCaptureFrameIndex++;

	Session.PendingGpuEncodes.Add(Request);

	ENQUEUE_RENDER_COMMAND(RuntimeRecQueueRenderTargetGpuEncode)(
		[Request, GpuEncoder, Resource, FrameIndex](FRHICommandListImmediate& RHICmdList)
		{
			if (Request->bCancelled)
			{
				return;
			}

			if (!GpuEncoder.IsValid() || !GpuEncoder->IsStarted())
			{
				Request->bSkipped = true;
				return;
			}

			const FTextureRHIRef SourceTexture = Resource->GetRenderTargetTexture();
			if (!SourceTexture.IsValid())
			{
				Request->bSkipped = true;
				return;
			}

			FString EncodeError;
			if (!GpuEncoder->EncodeTexture_RenderThread(RHICmdList, SourceTexture, FrameIndex, EncodeError))
			{
				Request->Error = EncodeError;
				Request->bHadError = true;
				return;
			}

			Request->bDone = true;
		});

	return true;
}

bool URuntimeRecSubsystem::FallbackRenderTargetSessionToReadback(
	FRuntimeRecRecordingSession& Session,
	const FString& Reason,
	FString& OutError)
{
	if (Session.Encoder)
	{
		return true;
	}

	for (const TSharedPtr<FRuntimeRecGpuEncodeRequest, ESPMode::ThreadSafe>& Request : Session.PendingGpuEncodes)
	{
		if (Request.IsValid())
		{
			Request->bCancelled = true;
		}
	}
	Session.PendingGpuEncodes.Reset();

	if (Session.GpuEncoder.IsValid())
	{
		FlushRenderingCommands();

		FString IgnoredGpuStopError;
		Session.GpuEncoder->Stop(IgnoredGpuStopError);
		Session.GpuEncoder.Reset();
		FlushRenderingCommands();
	}

	if (!Session.CurrentOutputPath.IsEmpty() && IFileManager::Get().FileExists(*Session.CurrentOutputPath))
	{
		IFileManager::Get().Delete(*Session.CurrentOutputPath, false, true);
	}

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
		delete Session.Encoder;
		Session.Encoder = nullptr;
		return false;
	}

	UE_LOG(LogTemp, Warning, TEXT("RuntimeRec: GPU encode failed; falling back to async readback. %s"), *Reason);
	return true;
}

bool URuntimeRecSubsystem::QueueRenderTargetReadback(
	FRuntimeRecRecordingSession& Session,
	FString& OutError,
	bool& bOutCanUseReadPixelsFallback)
{
	bOutCanUseReadPixelsFallback = false;

	UTextureRenderTarget2D* RenderTarget = Session.SourceRenderTarget.Get();
	if (!RenderTarget)
	{
		OutError = TEXT("RenderTarget is no longer valid.");
		return false;
	}

	FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
	if (!Resource)
	{
		OutError = TEXT("RenderTarget resource is not available.");
		return false;
	}

	if (RenderTarget->GetFormat() != PF_B8G8R8A8 || RenderTarget->GetSampleCount() != ETextureRenderTargetSampleCount::RTSC_1)
	{
		OutError = TEXT("RenderTarget format is not supported by async readback; falling back to ReadPixels.");
		bOutCanUseReadPixelsFallback = true;
		return false;
	}

	if (Session.PendingReadbacks.Num() >= RuntimeRecMaxPendingReadbacks)
	{
		if (!Session.ActiveOptions.bAllowFrameDrop)
		{
			OutError = TEXT("Async readback queue is full.");
			return false;
		}

		if (TSharedPtr<FRuntimeRecReadbackRequest, ESPMode::ThreadSafe> DroppedRequest = Session.PendingReadbacks[0])
		{
			DroppedRequest->bCancelled = true;
		}
		Session.PendingReadbacks.RemoveAt(0);
	}

	TSharedPtr<FRuntimeRecReadbackRequest, ESPMode::ThreadSafe> Request = MakeShared<FRuntimeRecReadbackRequest, ESPMode::ThreadSafe>();
	Request->Readback = MakeShared<FRHIGPUTextureReadback, ESPMode::ThreadSafe>(TEXT("RuntimeRecRenderTargetReadback"));
	Request->Width = Session.ActiveOptions.Width;
	Request->Height = Session.ActiveOptions.Height;
	Request->CaptureFrameIndex = Session.NextCaptureFrameIndex++;

	Session.PendingReadbacks.Add(Request);

	ENQUEUE_RENDER_COMMAND(RuntimeRecQueueRenderTargetReadback)(
		[Request, Resource](FRHICommandListImmediate& RHICmdList)
		{
			if (Request->bCancelled)
			{
				return;
			}

			const FTextureRHIRef SourceTexture = Resource->GetRenderTargetTexture();
			if (!SourceTexture.IsValid())
			{
				Request->bSkipped = true;
				return;
			}

			const FRHITextureDesc& TextureDesc = SourceTexture->GetDesc();
			if (TextureDesc.Format != PF_B8G8R8A8 || TextureDesc.IsMultisample())
			{
				Request->Error = TEXT("RenderTarget RHI texture is not compatible with async readback.");
				Request->bHadError = true;
				return;
			}

			RHICmdList.Transition(FRHITransitionInfo(SourceTexture, ERHIAccess::Unknown, ERHIAccess::CopySrc));
			Request->Readback->EnqueueCopy(
				RHICmdList,
				SourceTexture,
				FIntVector(0, 0, 0),
				0,
				FIntVector(Request->Width, Request->Height, 1));
		});

	return true;
}

bool URuntimeRecSubsystem::CaptureRenderTargetFrameFallback(FRuntimeRecRecordingSession& Session, FString& OutError)
{
	TArray<FColor> Pixels;
	int32 CapturedWidth = 0;
	int32 CapturedHeight = 0;
	if (!CaptureRenderTargetFrame(Session.SourceRenderTarget.Get(), Pixels, CapturedWidth, CapturedHeight))
	{
		OutError = LastError.IsEmpty() ? TEXT("Failed to read render target pixels.") : LastError;
		return false;
	}

	CropFrameToSize(Pixels, CapturedWidth, CapturedHeight, Session.ActiveOptions.Width, Session.ActiveOptions.Height);
	if (Pixels.Num() != Session.ActiveOptions.Width * Session.ActiveOptions.Height)
	{
		OutError = TEXT("Captured render target frame size is invalid.");
		return false;
	}

	FString EncodeError;
	if (!Session.Encoder)
	{
		OutError = TEXT("CPU encoder is not available for ReadPixels fallback.");
		return false;
	}

	if (!Session.Encoder->EnqueueFrame(MoveTemp(Pixels), EncodeError))
	{
		OutError = EncodeError;
		return false;
	}

	return true;
}

bool URuntimeRecSubsystem::HasAnyRenderTargetSessions() const
{
	return ActiveRenderTargetSessions.Num() > 0;
}

void URuntimeRecSubsystem::ClearRenderTargetSession(FRuntimeRecRecordingSession& Session)
{
	for (const TSharedPtr<FRuntimeRecReadbackRequest, ESPMode::ThreadSafe>& Request : Session.PendingReadbacks)
	{
		if (Request.IsValid())
		{
			Request->bCancelled = true;
		}
	}
	Session.PendingReadbacks.Reset();

	for (const TSharedPtr<FRuntimeRecGpuEncodeRequest, ESPMode::ThreadSafe>& Request : Session.PendingGpuEncodes)
	{
		if (Request.IsValid())
		{
			Request->bCancelled = true;
		}
	}
	Session.PendingGpuEncodes.Reset();

	if (Session.GpuEncoder.IsValid())
	{
		FString IgnoredError;
		FlushRenderingCommands();
		Session.GpuEncoder->Stop(IgnoredError);
		Session.GpuEncoder.Reset();
		FlushRenderingCommands();
	}

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
	Session.NextCaptureFrameIndex = 0;
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
