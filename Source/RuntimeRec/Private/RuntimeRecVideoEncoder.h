#pragma once

#include "CoreMinimal.h"

class FRuntimeRecVideoEncoder
{
public:
	FRuntimeRecVideoEncoder();
	~FRuntimeRecVideoEncoder();

	bool Start(
		const FString& OutputPath,
		int32 Width,
		int32 Height,
		int32 FPS,
		int32 BitrateKbps,
		bool bPreferHardwareEncoder,
		bool bAllowFrameDrop,
		FString& OutError);

	bool EnqueueFrame(TArray<FColor>&& FramePixels, FString& OutError);
	bool Stop(FString& OutError);
	bool IsStarted() const { return bStarted; }

private:
	struct FFrame
	{
		TArray<FColor> Pixels;
		int64 Index = 0;
	};

	void ThreadMain();
	bool InitializeWriter(FString& OutError);
	bool WriteFrame(const FFrame& Frame, FString& OutError);
	void ShutdownWriter();

private:
	FString OutputPath;
	int32 Width = 0;
	int32 Height = 0;
	int32 FPS = 30;
	int32 BitrateKbps = 12000;
	bool bPreferHardwareEncoder = true;
	bool bAllowFrameDrop = true;
	bool bStarted = false;
	bool bStopping = false;
	int64 NextFrameIndex = 0;
	FCriticalSection QueueCriticalSection;
	FEvent* QueueEvent = nullptr;
	TArray<FFrame> PendingFrames;
	TFuture<void> WorkerFuture;

#if PLATFORM_WINDOWS
	class FWindowsMediaFoundationState;
	TUniquePtr<FWindowsMediaFoundationState> State;
#endif
};
