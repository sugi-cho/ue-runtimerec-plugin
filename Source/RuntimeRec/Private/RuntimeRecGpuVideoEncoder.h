#pragma once

#include "CoreMinimal.h"
#include "RHIFwd.h"

class FRHICommandListImmediate;

class FRuntimeRecGpuVideoEncoder : public TSharedFromThis<FRuntimeRecGpuVideoEncoder, ESPMode::ThreadSafe>
{
public:
	FRuntimeRecGpuVideoEncoder();
	~FRuntimeRecGpuVideoEncoder();

	static bool IsAvailable(FString& OutReason);
	static bool IsPreferred();
	static void ShutdownReusableStates();

	bool Start(
		const FString& OutputPath,
		int32 Width,
		int32 Height,
		int32 FPS,
		int32 BitrateKbps,
		FString& OutError);

	bool EncodeTexture_RenderThread(
		FRHICommandListImmediate& RHICmdList,
		FTextureRHIRef SourceTexture,
		int64 FrameIndex,
		FString& OutError);

	bool Stop(FString& OutError);
	bool IsStarted() const { return bStarted; }

private:
	class FState;

	bool InitializeWriter(FString& OutError);
	bool WritePacket(const uint8* Data, uint64 DataSize, uint64 Timestamp, bool bIsKeyframe, FString& OutError);
	bool DrainPackets(FString& OutError, bool bWaitForAll = false);
	bool RetireStateForReuse();
	void ShutdownWriter();
	static void DestroyStateResources(FState& InState);
	static FCriticalSection& GetReusableStateCriticalSection();
	static TArray<TUniquePtr<FState>>& GetReusableStates();
	static TUniquePtr<FState> AcquireReusableState(int32 Width, int32 Height, int32 FPS, int32 BitrateKbps, int32 OutputSlotCount);

private:
	FString OutputPath;
	int32 Width = 0;
	int32 Height = 0;
	int32 FPS = 30;
	int32 BitrateKbps = 12000;
	bool bStarted = false;
	bool bStopping = false;
	bool bReservedGpuEncoderSlot = false;
	FCriticalSection CriticalSection;

	TUniquePtr<FState> State;
};
