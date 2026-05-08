#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "RuntimeRecTypes.h"
#include "RuntimeRecSubsystem.generated.h"

class FRuntimeRecVideoEncoder;
class UTextureRenderTarget2D;

UCLASS()
class RUNTIMEREC_API URuntimeRecSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	virtual void Deinitialize() override;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;

	bool StartViewportRecording(
		const FString& OutputDirectory,
		const FString& FileName,
		const FRuntimeRecOptions& Options,
		FString& OutSessionId,
		FString& OutError);

	bool StartRenderTargetRecording(
		UTextureRenderTarget2D* RenderTarget,
		const FString& OutputDirectory,
		const FString& FileName,
		const FRuntimeRecOptions& Options,
		FString& OutSessionId,
		FString& OutError);

	bool StopRecording(
		const FString& SessionId,
		FString& OutSavedFilePath,
		FString& OutError);

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec")
	bool IsRecording() const { return bRecording; }

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec")
	FString GetCurrentOutputPath() const { return CurrentOutputPath; }

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec")
	FString GetLastError() const { return LastError; }

private:
	bool StartRecordingInternal(
		ERuntimeRecInputSource InputSource,
		UTextureRenderTarget2D* RenderTarget,
		const FString& OutputDirectory,
		const FString& FileName,
		FRuntimeRecOptions Options,
		FString& OutSessionId,
		FString& OutError);

	bool CaptureViewportFrame(TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight);
	bool CaptureRenderTargetFrame(TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight);
	static void ForceEvenFrameSize(int32& Width, int32& Height);
	static void CropFrameToSize(TArray<FColor>& Pixels, int32 SourceWidth, int32 SourceHeight, int32 TargetWidth, int32 TargetHeight);
	void SetError(const FString& Error);

	static FString ResolveOutputDirectory(const FString& OutputDirectory);
	static FString MakeUniqueOutputPath(const FString& OutputDirectory, const FString& FileName);
	static FString SanitizeBaseFileName(const FString& FileName);

private:
	bool bRecording = false;
	ERuntimeRecInputSource Source = ERuntimeRecInputSource::Viewport;
	TWeakObjectPtr<UTextureRenderTarget2D> SourceRenderTarget;
	FRuntimeRecOptions ActiveOptions;
	FString CurrentSessionId;
	FString CurrentOutputPath;
	FString LastError;
	double AccumulatedTime = 0.0;
	double FrameInterval = 1.0 / 30.0;
	FRuntimeRecVideoEncoder* Encoder = nullptr;
};
