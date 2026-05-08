#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RuntimeRecCameraCaptureActor.generated.h"

class ACameraActor;
class UCameraComponent;
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
	virtual void Tick(float DeltaSeconds) override;
#if WITH_EDITOR
	virtual bool ShouldTickIfViewportsOnly() const override;
#endif

	UFUNCTION(BlueprintCallable, Category = "UE_RuntimeRec")
	bool RefreshCaptureConfiguration();

	UFUNCTION(BlueprintCallable, Category = "UE_RuntimeRec")
	UTextureRenderTarget2D* GetOrCreateRenderTarget();

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec")
	UTextureRenderTarget2D* GetEffectiveRenderTarget() const;

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec")
	FString GetLastError() const { return LastError; }

public:
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec")
	TObjectPtr<ACameraActor> SourceCamera;

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

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UE_RuntimeRec", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USceneCaptureComponent2D> SceneCaptureComponent;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> GeneratedRenderTarget;

	FString LastError;

	UCameraComponent* ResolveSourceCameraComponent() const;
	void UpdateCaptureConfiguration();
	void ApplySourceCameraSettings(UCameraComponent* CameraComponent);
	static int32 MakeEvenDimension(int32 Value);
	void SetError(const FString& Error);
};
