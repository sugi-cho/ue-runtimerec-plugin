#include "RuntimeRecCameraCaptureActor.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/TextureRenderTarget.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "RuntimeRecSubsystem.h"

ARuntimeRecCameraCaptureActor::ARuntimeRecCameraCaptureActor()
{
	PrimaryActorTick.bCanEverTick = true;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	SceneCaptureComponent = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCaptureComponent"));
	SceneCaptureComponent->SetupAttachment(RootComponent);
	SceneCaptureComponent->bCaptureEveryFrame = false;
	SceneCaptureComponent->bCaptureOnMovement = false;
	SceneCaptureComponent->bAlwaysPersistRenderingState = true;
	SceneCaptureComponent->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
	SceneCaptureComponent->CompositeMode = ESceneCaptureCompositeMode::SCCM_Overwrite;
	SceneCaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
}

void ARuntimeRecCameraCaptureActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	UpdateCaptureConfiguration(true);
}

void ARuntimeRecCameraCaptureActor::BeginPlay()
{
	Super::BeginPlay();
	UpdateCaptureConfiguration(true);

	if (bAutoStartRecording)
	{
		FString IgnoredError;
		StartRecording(IgnoredError);
	}
}

void ARuntimeRecCameraCaptureActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bRecording)
	{
		FString IgnoredSavedPath;
		FString IgnoredError;
		StopRecording(IgnoredSavedPath, IgnoredError);
	}

	Super::EndPlay(EndPlayReason);
}

#if WITH_EDITOR
bool ARuntimeRecCameraCaptureActor::ShouldTickIfViewportsOnly() const
{
	return true;
}
#endif

void ARuntimeRecCameraCaptureActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bAutoAssignSourcePawnFromPlayerController && !SourcePawn)
	{
		UpdateCaptureConfiguration(true);
	}

	if (!bCaptureEveryTick && !bRecording)
	{
		return;
	}

	if (!RefreshCaptureConfiguration())
	{
		return;
	}

	if (SceneCaptureComponent)
	{
		SceneCaptureComponent->CaptureScene();
	}
}

bool ARuntimeRecCameraCaptureActor::RefreshCaptureConfiguration()
{
	UpdateCaptureConfiguration(false);
	return ResolveSourceCameraComponent(false) != nullptr && GetEffectiveRenderTarget() != nullptr;
}

bool ARuntimeRecCameraCaptureActor::StartRecording(FString& OutError)
{
	if (bRecording)
	{
		OutError = TEXT("Recording is already active.");
		return false;
	}

	if (!RefreshCaptureConfiguration())
	{
		OutError = LastError.IsEmpty() ? TEXT("Failed to configure capture.") : LastError;
		return false;
	}

	URuntimeRecSubsystem* Subsystem = ResolveRuntimeRecSubsystem();
	if (!Subsystem)
	{
		OutError = TEXT("RuntimeRec subsystem is not available.");
		return false;
	}

	UTextureRenderTarget2D* EffectiveRenderTarget = GetEffectiveRenderTarget();
	if (!EffectiveRenderTarget)
	{
		OutError = TEXT("RenderTarget is not available.");
		return false;
	}

	FRuntimeRecOptions Options;
	Options.FPS = FPS;
	Options.BitrateKbps = BitrateKbps;
	Options.bAllowFrameDrop = bAllowFrameDrop;
	Options.bPreferHardwareEncoder = bPreferHardwareEncoder;

	FString SessionId;
	if (!Subsystem->StartRenderTargetRecording(EffectiveRenderTarget, OutputDirectory, FileName, Options, SessionId, OutError))
	{
		SetError(OutError);
		return false;
	}

	bRecording = true;
	CurrentSessionId = SessionId;
	CurrentOutputPath = Subsystem->GetCurrentOutputPath();
	if (SceneCaptureComponent)
	{
		SceneCaptureComponent->CaptureScene();
	}
	LastError.Reset();
	return true;
}

void ARuntimeRecCameraCaptureActor::StartRecordingEditor()
{
	FString OutError;
	if (!StartRecording(OutError) && !OutError.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: %s"), *GetName(), *OutError);
	}
}

bool ARuntimeRecCameraCaptureActor::StopRecording(FString& OutSavedFilePath, FString& OutError)
{
	if (!bRecording)
	{
		OutError = TEXT("No active recording.");
		return false;
	}

	URuntimeRecSubsystem* Subsystem = ResolveRuntimeRecSubsystem();
	if (!Subsystem)
	{
		OutError = TEXT("RuntimeRec subsystem is not available.");
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

void ARuntimeRecCameraCaptureActor::StopRecordingEditor()
{
	FString OutSavedFilePath;
	FString OutError;
	if (!StopRecording(OutSavedFilePath, OutError) && !OutError.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: %s"), *GetName(), *OutError);
	}
}

UTextureRenderTarget2D* ARuntimeRecCameraCaptureActor::GetOrCreateRenderTarget()
{
	if (TargetRenderTarget)
	{
		return TargetRenderTarget;
	}

	if (!bAutoCreateRenderTarget)
	{
		return nullptr;
	}

	const int32 Width = MakeEvenDimension(RenderTargetWidth);
	const int32 Height = MakeEvenDimension(RenderTargetHeight);

	if (!GeneratedRenderTarget || GeneratedRenderTarget->SizeX != Width || GeneratedRenderTarget->SizeY != Height)
	{
		GeneratedRenderTarget = NewObject<UTextureRenderTarget2D>(this, NAME_None, RF_Transient);
		GeneratedRenderTarget->ClearColor = FLinearColor::Black;
		GeneratedRenderTarget->TargetGamma = 2.2f;
		GeneratedRenderTarget->InitCustomFormat(Width, Height, PF_B8G8R8A8, false);
		GeneratedRenderTarget->UpdateResourceImmediate(true);
	}

	return GeneratedRenderTarget;
}

UTextureRenderTarget2D* ARuntimeRecCameraCaptureActor::GetEffectiveRenderTarget() const
{
	if (TargetRenderTarget)
	{
		return TargetRenderTarget;
	}

	return GeneratedRenderTarget;
}

UCameraComponent* ARuntimeRecCameraCaptureActor::ResolveSourceCameraComponent(bool bAllowPendingAutoSource) const
{
	if (!SourceCamera)
	{
		return ResolvePawnCameraComponent(bAllowPendingAutoSource);
	}

	if (UCameraComponent* CameraComponent = SourceCamera->GetCameraComponent())
	{
		return CameraComponent;
	}

	return ResolvePawnCameraComponent(bAllowPendingAutoSource);
}

UCameraComponent* ARuntimeRecCameraCaptureActor::ResolvePawnCameraComponent(bool bAllowPendingAutoSource) const
{
	if (SourcePawn)
	{
		return SourcePawn->FindComponentByClass<UCameraComponent>();
	}

	if (!bAutoAssignSourcePawnFromPlayerController)
	{
		return nullptr;
	}

	if (APawn* AutoSourcePawn = ResolveAutoSourcePawn())
	{
		const_cast<ARuntimeRecCameraCaptureActor*>(this)->SourcePawn = AutoSourcePawn;
		return AutoSourcePawn->FindComponentByClass<UCameraComponent>();
	}

	return nullptr;
}

void ARuntimeRecCameraCaptureActor::UpdateCaptureConfiguration(bool bAllowPendingAutoSource)
{
	if (!SceneCaptureComponent)
	{
		return;
	}

	UTextureRenderTarget2D* EffectiveRenderTarget = GetOrCreateRenderTarget();
	if (!EffectiveRenderTarget)
	{
		SetError(TEXT("Target render target is not set."));
		return;
	}

	SceneCaptureComponent->TextureTarget = EffectiveRenderTarget;
	SceneCaptureComponent->bCaptureEveryFrame = false;
	SceneCaptureComponent->bCaptureOnMovement = false;
	SceneCaptureComponent->bAlwaysPersistRenderingState = true;
	SceneCaptureComponent->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
	SceneCaptureComponent->CompositeMode = ESceneCaptureCompositeMode::SCCM_Overwrite;
	SceneCaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	SceneCaptureComponent->ShowFlags.SetPostProcessing(bIncludeCameraPostProcess);
	SceneCaptureComponent->ShowFlags.SetLumenGlobalIllumination(true);
	SceneCaptureComponent->ShowFlags.SetLumenReflections(true);

	UCameraComponent* CameraComponent = ResolveSourceCameraComponent(bAllowPendingAutoSource);
	if (!CameraComponent)
	{
		if (bAllowPendingAutoSource && bAutoAssignSourcePawnFromPlayerController)
		{
			return;
		}

		SetError(TEXT("Source camera is not set."));
		return;
	}

	ApplySourceCameraSettings(CameraComponent);

	if (bForceLumenForSceneCapture)
	{
		SceneCaptureComponent->PostProcessSettings.bOverride_DynamicGlobalIlluminationMethod = true;
		SceneCaptureComponent->PostProcessSettings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::Lumen;
		SceneCaptureComponent->PostProcessSettings.bOverride_ReflectionMethod = true;
		SceneCaptureComponent->PostProcessSettings.ReflectionMethod = EReflectionMethod::Lumen;
		SceneCaptureComponent->PostProcessSettings.bOverride_LumenSurfaceCacheResolution = true;
		SceneCaptureComponent->PostProcessSettings.LumenSurfaceCacheResolution = LumenSurfaceCacheResolution;
	}

	LastError.Reset();
}

void ARuntimeRecCameraCaptureActor::ApplySourceCameraSettings(UCameraComponent* CameraComponent)
{
	if (!CameraComponent || !SceneCaptureComponent)
	{
		return;
	}

	FMinimalViewInfo ViewInfo;
	CameraComponent->GetCameraView(0.0f, ViewInfo);

	SetActorLocationAndRotation(ViewInfo.Location, ViewInfo.Rotation);

	SceneCaptureComponent->ProjectionType = ViewInfo.ProjectionMode;
	SceneCaptureComponent->FOVAngle = ViewInfo.FOV;
	SceneCaptureComponent->OrthoWidth = ViewInfo.OrthoWidth;
	SceneCaptureComponent->bOverride_CustomNearClippingPlane = ViewInfo.PerspectiveNearClipPlane > 0.0f;
	SceneCaptureComponent->CustomNearClippingPlane = ViewInfo.PerspectiveNearClipPlane > 0.0f ? ViewInfo.PerspectiveNearClipPlane : 0.0f;

	if (bIncludeCameraPostProcess)
	{
		SceneCaptureComponent->PostProcessSettings = ViewInfo.PostProcessSettings;
		SceneCaptureComponent->PostProcessBlendWeight = ViewInfo.PostProcessBlendWeight;
	}
	else
	{
		SceneCaptureComponent->PostProcessSettings = FPostProcessSettings();
		SceneCaptureComponent->PostProcessBlendWeight = 0.0f;
	}
}

APawn* ARuntimeRecCameraCaptureActor::ResolveAutoSourcePawn() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	APlayerController* PlayerController = World->GetFirstPlayerController();
	if (!PlayerController)
	{
		return nullptr;
	}

	return PlayerController->GetPawn();
}

URuntimeRecSubsystem* ARuntimeRecCameraCaptureActor::ResolveRuntimeRecSubsystem() const
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

int32 ARuntimeRecCameraCaptureActor::MakeEvenDimension(int32 Value)
{
	Value = FMath::Max(Value, 2);
	return (Value / 2) * 2;
}

void ARuntimeRecCameraCaptureActor::SetError(const FString& Error)
{
	if (LastError == Error)
	{
		return;
	}

	LastError = Error;
	UE_LOG(LogTemp, Warning, TEXT("RuntimeRecCameraCaptureActor: %s"), *Error);
}
