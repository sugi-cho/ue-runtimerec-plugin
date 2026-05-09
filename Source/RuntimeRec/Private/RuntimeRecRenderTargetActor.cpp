#include "RuntimeRecRenderTargetActor.h"

#include "Engine/GameInstance.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RuntimeRecSubsystem.h"

ARuntimeRecRenderTargetActor::ARuntimeRecRenderTargetActor()
{
	PrimaryActorTick.bCanEverTick = false;
}

void ARuntimeRecRenderTargetActor::BeginPlay()
{
	Super::BeginPlay();

	if (bAutoStartRecording)
	{
		FString IgnoredError;
		StartRecording(IgnoredError);
	}
}

void ARuntimeRecRenderTargetActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bRecording)
	{
		FString IgnoredSavedFilePath;
		FString IgnoredError;
		StopRecording(IgnoredSavedFilePath, IgnoredError);
	}

	Super::EndPlay(EndPlayReason);
}

bool ARuntimeRecRenderTargetActor::StartRecording(FString& OutError)
{
	OutError.Reset();
	LastError.Reset();

	if (bRecording)
	{
		OutError = TEXT("Recording is already active.");
		LastError = OutError;
		return false;
	}

	if (!TargetRenderTarget)
	{
		OutError = TEXT("TargetRenderTarget is not set.");
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

	FRuntimeRecOptions Options;
	Options.FPS = FPS;
	Options.BitrateKbps = BitrateKbps;
	Options.bAllowFrameDrop = bAllowFrameDrop;
	Options.bPreferHardwareEncoder = bPreferHardwareEncoder;

	FString SessionId;
	if (!Subsystem->StartRenderTargetRecording(TargetRenderTarget, OutputDirectory, FileName, Options, SessionId, OutError))
	{
		SetError(OutError);
		return false;
	}

	bRecording = true;
	CurrentSessionId = SessionId;
	CurrentOutputPath = Subsystem->GetCurrentOutputPath();
	LastError.Reset();
	return true;
}

void ARuntimeRecRenderTargetActor::StartRecordingEditor()
{
	FString OutError;
	if (!StartRecording(OutError) && !OutError.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: %s"), *GetName(), *OutError);
	}
}

bool ARuntimeRecRenderTargetActor::StopRecording(FString& OutSavedFilePath, FString& OutError)
{
	if (!bRecording)
	{
		OutError = TEXT("No active recording.");
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

	if (!Subsystem->StopRecording(CurrentSessionId, OutSavedFilePath, OutError))
	{
		SetError(OutError);
		return false;
	}

	bRecording = false;
	CurrentSessionId.Reset();
	CurrentOutputPath = OutSavedFilePath;
	LastError.Reset();
	return true;
}

void ARuntimeRecRenderTargetActor::StopRecordingEditor()
{
	FString OutSavedFilePath;
	FString OutError;
	if (!StopRecording(OutSavedFilePath, OutError) && !OutError.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: %s"), *GetName(), *OutError);
	}
}

URuntimeRecSubsystem* ARuntimeRecRenderTargetActor::ResolveRuntimeRecSubsystem() const
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

void ARuntimeRecRenderTargetActor::SetError(const FString& Error)
{
	if (LastError == Error)
	{
		return;
	}

	LastError = Error;
	UE_LOG(LogTemp, Warning, TEXT("RuntimeRecRenderTargetActor: %s"), *Error);
}
