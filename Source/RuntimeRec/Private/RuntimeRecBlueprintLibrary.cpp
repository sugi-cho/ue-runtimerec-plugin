#include "RuntimeRecBlueprintLibrary.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "RuntimeRecSubsystem.h"

namespace RuntimeRecBlueprintLibrary
{
	URuntimeRecSubsystem* GetSubsystem(const UObject* WorldContextObject)
	{
		if (!GEngine)
		{
			return nullptr;
		}

		UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
		if (!World || !World->GetGameInstance())
		{
			return nullptr;
		}

		return World->GetGameInstance()->GetSubsystem<URuntimeRecSubsystem>();
	}
}

bool URuntimeRecBlueprintLibrary::StartViewportRecording(
	const UObject* WorldContextObject,
	const FString& OutputDirectory,
	const FString& FileName,
	int32 FPS,
	int32 BitrateKbps,
	bool bIncludeUI,
	FString& OutSessionId,
	FString& OutError)
{
	FRuntimeRecOptions Options;
	Options.FPS = FPS;
	Options.BitrateKbps = BitrateKbps;
	Options.bIncludeUI = bIncludeUI;

	URuntimeRecSubsystem* Subsystem = RuntimeRecBlueprintLibrary::GetSubsystem(WorldContextObject);
	if (!Subsystem)
	{
		OutError = TEXT("RuntimeRec subsystem is not available.");
		return false;
	}

	return Subsystem->StartViewportRecording(OutputDirectory, FileName, Options, OutSessionId, OutError);
}

bool URuntimeRecBlueprintLibrary::StartRenderTargetRecording(
	const UObject* WorldContextObject,
	UTextureRenderTarget2D* RenderTarget,
	const FString& OutputDirectory,
	const FString& FileName,
	int32 FPS,
	int32 BitrateKbps,
	FString& OutSessionId,
	FString& OutError)
{
	FRuntimeRecOptions Options;
	Options.FPS = FPS;
	Options.BitrateKbps = BitrateKbps;

	URuntimeRecSubsystem* Subsystem = RuntimeRecBlueprintLibrary::GetSubsystem(WorldContextObject);
	if (!Subsystem)
	{
		OutError = TEXT("RuntimeRec subsystem is not available.");
		return false;
	}

	return Subsystem->StartRenderTargetRecording(RenderTarget, OutputDirectory, FileName, Options, OutSessionId, OutError);
}

bool URuntimeRecBlueprintLibrary::StopRecording(
	const UObject* WorldContextObject,
	const FString& SessionId,
	FString& OutSavedFilePath,
	FString& OutError)
{
	URuntimeRecSubsystem* Subsystem = RuntimeRecBlueprintLibrary::GetSubsystem(WorldContextObject);
	if (!Subsystem)
	{
		OutError = TEXT("RuntimeRec subsystem is not available.");
		return false;
	}

	return Subsystem->StopRecording(SessionId, OutSavedFilePath, OutError);
}

bool URuntimeRecBlueprintLibrary::IsRecording(const UObject* WorldContextObject)
{
	URuntimeRecSubsystem* Subsystem = RuntimeRecBlueprintLibrary::GetSubsystem(WorldContextObject);
	return Subsystem ? Subsystem->IsRecording() : false;
}

FString URuntimeRecBlueprintLibrary::GetCurrentOutputPath(const UObject* WorldContextObject)
{
	URuntimeRecSubsystem* Subsystem = RuntimeRecBlueprintLibrary::GetSubsystem(WorldContextObject);
	return Subsystem ? Subsystem->GetCurrentOutputPath() : FString();
}

FString URuntimeRecBlueprintLibrary::GetLastError(const UObject* WorldContextObject)
{
	URuntimeRecSubsystem* Subsystem = RuntimeRecBlueprintLibrary::GetSubsystem(WorldContextObject);
	return Subsystem ? Subsystem->GetLastError() : FString();
}
