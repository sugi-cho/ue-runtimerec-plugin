#pragma once

#include "CoreMinimal.h"
#include "RuntimeRecTypes.generated.h"

UENUM(BlueprintType)
enum class ERuntimeRecInputSource : uint8
{
	Viewport UMETA(DisplayName = "Viewport"),
	RenderTarget UMETA(DisplayName = "Render Target")
};

USTRUCT(BlueprintType)
struct RUNTIMEREC_API FRuntimeRecOptions
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UE_RuntimeRec")
	int32 Width = 1920;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UE_RuntimeRec")
	int32 Height = 1080;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UE_RuntimeRec", meta = (ClampMin = "1", ClampMax = "240"))
	int32 FPS = 30;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UE_RuntimeRec", meta = (ClampMin = "1"))
	int32 BitrateKbps = 12000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UE_RuntimeRec")
	bool bIncludeUI = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UE_RuntimeRec")
	bool bAllowFrameDrop = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UE_RuntimeRec")
	bool bPreferHardwareEncoder = true;
};
