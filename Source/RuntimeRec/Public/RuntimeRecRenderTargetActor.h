#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RuntimeRecRenderTargetActor.generated.h"

class URuntimeRecSubsystem;
class UTextureRenderTarget2D;

UCLASS(Blueprintable, ClassGroup = (RuntimeRec), meta = (DisplayName = "RuntimeRec Render Target"))
class RUNTIMEREC_API ARuntimeRecRenderTargetActor : public AActor
{
	GENERATED_BODY()

public:
	ARuntimeRecRenderTargetActor();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION(BlueprintCallable, Category = "UE_RuntimeRec")
	bool StartRecording(FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "UE_RuntimeRec")
	bool StopRecording(FString& OutSavedFilePath, FString& OutError);

	UFUNCTION(CallInEditor, Category = "UE_RuntimeRec|Editor", meta = (DisplayName = "Start Recording"))
	void StartRecordingEditor();

	UFUNCTION(CallInEditor, Category = "UE_RuntimeRec|Editor", meta = (DisplayName = "Stop Recording"))
	void StopRecordingEditor();

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec")
	bool IsRecording() const { return bRecording; }

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec")
	FString GetCurrentOutputPath() const { return CurrentOutputPath; }

	UFUNCTION(BlueprintPure, Category = "UE_RuntimeRec")
	FString GetLastError() const { return LastError; }

public:
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "UE_RuntimeRec")
	TObjectPtr<UTextureRenderTarget2D> TargetRenderTarget;

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
	URuntimeRecSubsystem* ResolveRuntimeRecSubsystem() const;
	void SetError(const FString& Error);

private:
	UPROPERTY(Transient)
	bool bRecording = false;

	FString CurrentSessionId;
	FString CurrentOutputPath;
	FString LastError;
};
