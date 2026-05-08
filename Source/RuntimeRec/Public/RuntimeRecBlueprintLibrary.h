#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "RuntimeRecBlueprintLibrary.generated.h"

class UTextureRenderTarget2D;

UCLASS()
class RUNTIMEREC_API URuntimeRecBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "UE_RuntimeRec", meta = (WorldContext = "WorldContextObject", CPP_Default_FPS = "30", CPP_Default_BitrateKbps = "12000"))
	static bool StartViewportRecording(
		const UObject* WorldContextObject,
		const FString& OutputDirectory,
		const FString& FileName,
		int32 FPS,
		int32 BitrateKbps,
		FString& OutSessionId,
		FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "UE_RuntimeRec", meta = (WorldContext = "WorldContextObject", CPP_Default_FPS = "30", CPP_Default_BitrateKbps = "12000"))
	static bool StartRenderTargetRecording(
		const UObject* WorldContextObject,
		UTextureRenderTarget2D* RenderTarget,
		const FString& OutputDirectory,
		const FString& FileName,
		int32 FPS,
		int32 BitrateKbps,
		FString& OutSessionId,
		FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "UE_RuntimeRec", meta = (WorldContext = "WorldContextObject"))
	static bool StopRecording(
		const UObject* WorldContextObject,
		const FString& SessionId,
		FString& OutSavedFilePath,
		FString& OutError);

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec", meta = (WorldContext = "WorldContextObject"))
	static bool IsRecording(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec", meta = (WorldContext = "WorldContextObject"))
	static FString GetCurrentOutputPath(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec", meta = (WorldContext = "WorldContextObject"))
	static FString GetLastError(const UObject* WorldContextObject);
};
