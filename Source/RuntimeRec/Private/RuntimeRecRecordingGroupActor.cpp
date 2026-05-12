#include "RuntimeRecRecordingGroupActor.h"

#include "Engine/GameInstance.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "RuntimeRecCameraCaptureActor.h"
#include "RuntimeRecSubsystem.h"

ARuntimeRecRecordingGroupActor::ARuntimeRecRecordingGroupActor()
{
	PrimaryActorTick.bCanEverTick = false;
}

void ARuntimeRecRecordingGroupActor::BeginPlay()
{
	Super::BeginPlay();

	if (bAutoStartRecording)
	{
		FString IgnoredError;
		StartRecording(IgnoredError);
	}
}

void ARuntimeRecRecordingGroupActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bRecording)
	{
		TArray<FString> IgnoredSavedFilePaths;
		FString IgnoredError;
		StopRecording(IgnoredSavedFilePaths, IgnoredError);
	}

	Super::EndPlay(EndPlayReason);
}

bool ARuntimeRecRecordingGroupActor::StartRecording(FString& OutError)
{
	OutError.Reset();
	LastError.Reset();

	if (bRecording)
	{
		OutError = TEXT("Recording is already active.");
		LastError = OutError;
		return false;
	}

	URuntimeRecSubsystem* Subsystem = ResolveRuntimeRecSubsystem();
	if (!Subsystem)
	{
		OutError = TEXT("RuntimeRec subsystem is not available.");
		LastError = OutError;
		return false;
	}

	const FString RecordingStem = BuildRecordingStem();

	TArray<FRuntimeRecGroupRecordingHandle> PendingHandles;
	FString LocalError;

	for (int32 Index = 0; Index < TargetRenderTargets.Num(); ++Index)
	{
		UTextureRenderTarget2D* RenderTarget = TargetRenderTargets[Index];
		if (!RenderTarget)
		{
			OutError = FString::Printf(TEXT("TargetRenderTargets[%d] is null."), Index);
			LocalError = OutError;
			goto Fail;
		}

		FRuntimeRecGroupRecordingHandle Handle;
		Handle.RenderTarget = RenderTarget;

			const FString FileName = FString::Printf(TEXT("%s_%s"), *RecordingStem, *BuildTargetSuffix(Index));
		if (!StartRecordingForRenderTarget(RenderTarget, FileName, Subsystem, Handle.SessionId, Handle.OutputPath, LocalError))
		{
			OutError = FString::Printf(TEXT("Failed to start render target recording %d: %s"), Index, *LocalError);
			goto Fail;
		}

		PendingHandles.Add(MoveTemp(Handle));
	}

	for (int32 Index = 0; Index < TargetCameraCaptureActors.Num(); ++Index)
	{
		ARuntimeRecCameraCaptureActor* CameraCaptureActor = TargetCameraCaptureActors[Index];
		if (!CameraCaptureActor)
		{
			OutError = FString::Printf(TEXT("TargetCameraCaptureActors[%d] is null."), Index);
			LocalError = OutError;
			goto Fail;
		}

		CameraCaptureActor->SetCaptureActive(true);
		if (!CameraCaptureActor->RefreshCaptureConfiguration())
		{
			OutError = FString::Printf(TEXT("Failed to refresh camera capture actor %d."), Index);
			LocalError = OutError;
			goto Fail;
		}

		UTextureRenderTarget2D* RenderTarget = CameraCaptureActor->GetEffectiveRenderTarget();
		if (!RenderTarget)
		{
			OutError = FString::Printf(TEXT("Camera capture actor %d has no render target."), Index);
			LocalError = OutError;
			goto Fail;
		}

		FRuntimeRecGroupRecordingHandle Handle;
		Handle.RenderTarget = RenderTarget;
		Handle.CameraCaptureActor = CameraCaptureActor;

		const FString FileName = FString::Printf(TEXT("%s_%s"), *RecordingStem, *BuildCameraSuffix(CameraCaptureActor, Index));
		if (!StartRecordingForRenderTarget(RenderTarget, FileName, Subsystem, Handle.SessionId, Handle.OutputPath, LocalError))
		{
			OutError = FString::Printf(TEXT("Failed to start camera capture recording %d: %s"), Index, *LocalError);
			goto Fail;
		}

		PendingHandles.Add(MoveTemp(Handle));
	}

	ActiveHandles = MoveTemp(PendingHandles);
	bRecording = ActiveHandles.Num() > 0;
	return bRecording;

Fail:
	OutError = LocalError.IsEmpty() ? OutError : LocalError;
	for (const FRuntimeRecGroupRecordingHandle& Handle : PendingHandles)
	{
		if (Handle.SessionId.IsEmpty())
		{
			continue;
		}

		FString IgnoredSavedFilePath;
		FString IgnoredStopError;
		Subsystem->StopRecording(Handle.SessionId, IgnoredSavedFilePath, IgnoredStopError);
	}

	ResetHandles();
	LastError = OutError;
	return false;
}

void ARuntimeRecRecordingGroupActor::StartRecordingEditor()
{
	FString OutError;
	if (!StartRecording(OutError) && !OutError.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: %s"), *GetName(), *OutError);
	}
}

bool ARuntimeRecRecordingGroupActor::StopRecording(TArray<FString>& OutSavedFilePaths, FString& OutError)
{
	OutSavedFilePaths.Reset();
	OutError.Reset();
	LastError.Reset();

	if (!bRecording || ActiveHandles.Num() == 0)
	{
		OutError = TEXT("No active recording.");
		LastError = OutError;
		return false;
	}

	URuntimeRecSubsystem* Subsystem = nullptr;
	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GameInstance = World->GetGameInstance())
		{
			Subsystem = GameInstance->GetSubsystem<URuntimeRecSubsystem>();
		}
	}

	if (!Subsystem)
	{
		OutError = TEXT("RuntimeRec subsystem is not available.");
		LastError = OutError;
		return false;
	}

	bool bAllSucceeded = true;
	for (const FRuntimeRecGroupRecordingHandle& Handle : ActiveHandles)
	{
		if (Handle.SessionId.IsEmpty())
		{
			continue;
		}

		FString SavedFilePath;
		FString StopError;
		if (!Subsystem->StopRecording(Handle.SessionId, SavedFilePath, StopError))
		{
			bAllSucceeded = false;
			if (OutError.IsEmpty())
			{
				OutError = StopError;
			}
			continue;
		}

		OutSavedFilePaths.Add(MoveTemp(SavedFilePath));
	}

	ResetHandles();
	if (!bAllSucceeded)
	{
		LastError = OutError;
		return false;
	}

	return true;
}

void ARuntimeRecRecordingGroupActor::StopRecordingEditor()
{
	TArray<FString> OutSavedFilePaths;
	FString OutError;
	if (!StopRecording(OutSavedFilePaths, OutError) && !OutError.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: %s"), *GetName(), *OutError);
	}
}

TArray<FString> ARuntimeRecRecordingGroupActor::GetCurrentOutputPaths() const
{
	TArray<FString> OutputPaths;
	OutputPaths.Reserve(ActiveHandles.Num());

	for (const FRuntimeRecGroupRecordingHandle& Handle : ActiveHandles)
	{
		if (!Handle.OutputPath.IsEmpty())
		{
			OutputPaths.Add(Handle.OutputPath);
		}
	}

	return OutputPaths;
}

bool ARuntimeRecRecordingGroupActor::StartRecordingForRenderTarget(
	UTextureRenderTarget2D* RenderTarget,
	const FString& FileName,
	URuntimeRecSubsystem* Subsystem,
	FString& OutSessionId,
	FString& OutOutputPath,
	FString& OutError)
{
	if (!Subsystem)
	{
		OutError = TEXT("RuntimeRec subsystem is not available.");
		return false;
	}

	FRuntimeRecOptions Options;
	Options.FPS = FMath::Clamp(FPS, 1, 240);
	Options.BitrateKbps = FMath::Max(BitrateKbps, 1);
	Options.bAllowFrameDrop = bAllowFrameDrop;
	Options.bPreferHardwareEncoder = bPreferHardwareEncoder;

	if (!Subsystem->StartRenderTargetRecording(RenderTarget, OutputDirectory, FileName, Options, OutSessionId, OutError))
	{
		return false;
	}

	OutOutputPath = Subsystem->GetRecordingOutputPath(OutSessionId);
	return true;
}

void ARuntimeRecRecordingGroupActor::ResetHandles()
{
	ActiveHandles.Reset();
	bRecording = false;
}

FString ARuntimeRecRecordingGroupActor::BuildRecordingStem() const
{
	if (!FileNamePrefix.IsEmpty())
	{
		return FileNamePrefix;
	}

	return FString::Printf(TEXT("RuntimeRec_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
}

FString ARuntimeRecRecordingGroupActor::BuildTargetSuffix(int32 Index)
{
	return FString::Printf(TEXT("RT%02d"), Index + 1);
}

FString ARuntimeRecRecordingGroupActor::BuildCameraSuffix(const ARuntimeRecCameraCaptureActor* CameraCaptureActor, int32 Index)
{
	if (CameraCaptureActor)
	{
		return FString::Printf(TEXT("Cam_%s"), *CameraCaptureActor->GetName());
	}

	return FString::Printf(TEXT("Cam%02d"), Index + 1);
}

URuntimeRecSubsystem* ARuntimeRecRecordingGroupActor::ResolveRuntimeRecSubsystem() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	UGameInstance* GameInstance = World->GetGameInstance();
	if (!GameInstance)
	{
		return nullptr;
	}

	return GameInstance->GetSubsystem<URuntimeRecSubsystem>();
}
