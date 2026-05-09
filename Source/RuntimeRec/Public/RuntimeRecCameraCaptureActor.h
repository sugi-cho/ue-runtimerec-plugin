#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RuntimeRecCameraCaptureActor.generated.h"

class ACameraActor;
class APawn;
class UCameraComponent;
class URuntimeRecSubsystem;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;

UCLASS(Blueprintable, ClassGroup = (RuntimeRec), meta = (DisplayName = "RuntimeRec Camera Capture"))
class RUNTIMEREC_API ARuntimeRecCameraCaptureActor : public AActor
{
	GENERATED_BODY()

public:
	ARuntimeRecCameraCaptureActor();

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
#if WITH_EDITOR
	virtual bool ShouldTickIfViewportsOnly() const override;
#endif

	UFUNCTION(BlueprintCallable, Category = "UE_RuntimeRec")
	bool RefreshCaptureConfiguration();

	UFUNCTION(BlueprintCallable, Category = "UE_RuntimeRec")
	bool StartRecording(FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "UE_RuntimeRec")
	bool StopRecording(FString& OutSavedFilePath, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "UE_RuntimeRec")
	void SetCaptureActive(bool bNewActive);

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec")
	bool IsCaptureActive() const { return bCaptureActive; }

	UFUNCTION(CallInEditor, Category = "UE_RuntimeRec|Editor", meta = (DisplayName = "Start Recording"))
	void StartRecordingEditor();

	UFUNCTION(CallInEditor, Category = "UE_RuntimeRec|Editor", meta = (DisplayName = "Stop Recording"))
	void StopRecordingEditor();

	UFUNCTION(BlueprintCallable, Category = "UE_RuntimeRec")
	UTextureRenderTarget2D* GetOrCreateRenderTarget();

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec")
	UTextureRenderTarget2D* GetEffectiveRenderTarget() const;

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec")
	bool IsRecording() const { return bRecording; }

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec")
	FString GetCurrentOutputPath() const { return CurrentOutputPath; }

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec")
	FString GetLastError() const { return LastError; }

public:
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec")
	TObjectPtr<ACameraActor> SourceCamera;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec")
	TObjectPtr<APawn> SourcePawn;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec")
	bool bAutoAssignSourcePawnFromPlayerController = true;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec|Lumen")
	bool bForceLumenForSceneCapture = true;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec|Lumen", meta = (ClampMin = "0.1", ClampMax = "1.0"))
	float LumenSurfaceCacheResolution = 0.5f;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec")
	TObjectPtr<UTextureRenderTarget2D> TargetRenderTarget;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec", meta = (ClampMin = "1"))
	int32 RenderTargetWidth = 1920;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec", meta = (ClampMin = "1"))
	int32 RenderTargetHeight = 1080;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec")
	bool bAutoCreateRenderTarget = true;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec")
	bool bIncludeCameraPostProcess = true;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec")
	bool bCaptureEveryTick = true;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec")
	bool bCaptureActive = true;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec|Recording")
	FString OutputDirectory;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec|Recording")
	FString FileName;

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
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UE_RuntimeRec", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USceneCaptureComponent2D> SceneCaptureComponent;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> GeneratedRenderTarget;

	UPROPERTY(Transient)
	bool bRecording = false;

	FString LastError;
	FString CurrentSessionId;
	FString CurrentOutputPath;

	UCameraComponent* ResolveSourceCameraComponent(bool bAllowPendingAutoSource) const;
	UCameraComponent* ResolvePawnCameraComponent(bool bAllowPendingAutoSource) const;
	void UpdateCaptureConfiguration(bool bAllowPendingAutoSource);
	void ApplyCaptureActivity();
	void ApplySourceCameraSettings(UCameraComponent* CameraComponent);
	URuntimeRecSubsystem* ResolveRuntimeRecSubsystem() const;
	APawn* ResolveAutoSourcePawn() const;
	static int32 MakeEvenDimension(int32 Value);
	void SetError(const FString& Error);
};
