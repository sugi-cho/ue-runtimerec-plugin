#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RuntimeRecRecordingGroupActor.generated.h"

class ARuntimeRecCameraCaptureActor;
class UTextureRenderTarget2D;
class URuntimeRecSubsystem;

USTRUCT()
struct FRuntimeRecGroupRecordingHandle
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> RenderTarget = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<ARuntimeRecCameraCaptureActor> CameraCaptureActor = nullptr;

	FString SessionId;
	FString OutputPath;
};

UCLASS(Blueprintable, ClassGroup = (RuntimeRec), meta = (DisplayName = "RuntimeRec Recording Group"))
class RUNTIMEREC_API ARuntimeRecRecordingGroupActor : public AActor
{
	GENERATED_BODY()

public:
	ARuntimeRecRecordingGroupActor();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION(BlueprintCallable, Category = "UE_RuntimeRec")
	bool StartRecording(FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "UE_RuntimeRec")
	bool StopRecording(TArray<FString>& OutSavedFilePaths, FString& OutError);

	UFUNCTION(CallInEditor, Category = "UE_RuntimeRec|Editor", meta = (DisplayName = "Start Recording"))
	void StartRecordingEditor();

	UFUNCTION(CallInEditor, Category = "UE_RuntimeRec|Editor", meta = (DisplayName = "Stop Recording"))
	void StopRecordingEditor();

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec")
	bool IsRecording() const { return bRecording; }

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec")
	FString GetLastError() const { return LastError; }

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec")
	TArray<FString> GetCurrentOutputPaths() const;

public:
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec|Targets")
	TArray<TObjectPtr<UTextureRenderTarget2D>> TargetRenderTargets;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec|Targets")
	TArray<TObjectPtr<ARuntimeRecCameraCaptureActor>> TargetCameraCaptureActors;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec|Recording")
	FString OutputDirectory;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec|Recording")
	FString FileNamePrefix;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec|Recording", meta = (ClampMin = "1", ClampMax = "240"))
	int32 FPS = 30;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec|Recording", meta = (ClampMin = "1"))
	int32 BitrateKbps = 12000;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec|Recording")
	bool bAllowFrameDrop = true;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec|Recording")
	bool bPreferHardwareEncoder = true;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec|Recording")
	bool bAutoStartRecording = false;

private:
	URuntimeRecSubsystem* ResolveRuntimeRecSubsystem() const;

	bool StartRecordingForRenderTarget(
		UTextureRenderTarget2D* RenderTarget,
		const FString& FileName,
		URuntimeRecSubsystem* Subsystem,
		FString& OutSessionId,
		FString& OutOutputPath,
		FString& OutError);

	void ResetHandles();
	FString BuildRecordingStem() const;
	static FString BuildTargetSuffix(int32 Index);
	static FString BuildCameraSuffix(const ARuntimeRecCameraCaptureActor* CameraCaptureActor, int32 Index);

private:
	UPROPERTY(Transient)
	TArray<FRuntimeRecGroupRecordingHandle> ActiveHandles;

	UPROPERTY(Transient)
	bool bRecording = false;

	UPROPERTY(Transient)
	FString LastError;
};
