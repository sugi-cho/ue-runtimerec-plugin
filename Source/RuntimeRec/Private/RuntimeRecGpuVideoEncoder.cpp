#include "RuntimeRecGpuVideoEncoder.h"

#include "AVDevice.h"
#include "DynamicRHI.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "ID3D12DynamicRHI.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "NVENC.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "Templates/RefCounting.h"
#include "Video/Resources/D3D/VideoResourceD3D.h"
#include "Video/Resources/VideoResourceRHI.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
#include <d3d12.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
THIRD_PARTY_INCLUDES_END
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	TAutoConsoleVariable<int32> CVarRuntimeRecGpuVideoEncoder(
		TEXT("RuntimeRec.RenderTarget.GpuVideoEncoder"),
		1,
		TEXT("Use Direct NVENC D3D12 GPU encoding for RenderTarget recording when available."));

	TAutoConsoleVariable<int32> CVarRuntimeRecMaxGpuVideoEncoders(
		TEXT("RuntimeRec.RenderTarget.MaxGpuVideoEncoders"),
		8,
		TEXT("Maximum simultaneous RenderTarget recordings that may use Direct NVENC GPU encoding. Remaining recordings fall back to async readback."));

	TAutoConsoleVariable<int32> CVarRuntimeRecGpuVideoEncoderPendingPacketCount(
		TEXT("RuntimeRec.RenderTarget.GpuVideoEncoderPendingPacketCount"),
		4,
		TEXT("Maximum number of Direct NVENC output packets to keep pending before draining."));

	TAutoConsoleVariable<float> CVarRuntimeRecGpuVideoEncoderReusableIdleSeconds(
		TEXT("RuntimeRec.RenderTarget.GpuVideoEncoderReusableIdleSeconds"),
		30.0f,
		TEXT("How long idle reusable NVENC sessions may stay in the pool before being pruned. Set to 0 or below to disable reuse retention."));

#if WITH_EDITOR
	TAutoConsoleVariable<float> CVarRuntimeRecGpuVideoEncoderReusableIdleSecondsEditor(
		TEXT("RuntimeRec.RenderTarget.GpuVideoEncoderReusableIdleSecondsEditor"),
		600.0f,
		TEXT("Editor-only TTL for idle reusable NVENC sessions before ticker pruning. Set to 0 or below to disable age-based editor pruning."));
#endif

	TAutoConsoleVariable<int32> CVarRuntimeRecMaxReusableGpuVideoEncoders(
		TEXT("RuntimeRec.RenderTarget.MaxReusableGpuVideoEncoders"),
		8,
		TEXT("Maximum number of idle reusable NVENC sessions to keep in the pool. Oldest idle sessions are pruned first."));

	TAutoConsoleVariable<float> CVarRuntimeRecGpuVideoEncoderOpenCooldownSeconds(
		TEXT("RuntimeRec.RenderTarget.GpuVideoEncoderOpenCooldownSeconds"),
		1.5f,
		TEXT("Minimum cooldown after destroying reusable NVENC sessions before attempting a fresh open."));

	FCriticalSection GpuEncoderSlotCriticalSection;
	int32 ActiveGpuEncoderSlots = 0;
	FCriticalSection ReusablePoolStatsCriticalSection;
	double LastNvencDestroyTimeSeconds = -1.0;
	double LastNvencPruneTimeSeconds = -1.0;
	int32 LastNvencDestroyCount = 0;
	int32 LastNvencPruneCount = 0;
	constexpr int32 RuntimeRecGpuEncoderInputBufferCount = 3;

	float GetReusableIdleSecondsCVar()
	{
#if WITH_EDITOR
		return CVarRuntimeRecGpuVideoEncoderReusableIdleSecondsEditor.GetValueOnAnyThread();
#else
		return CVarRuntimeRecGpuVideoEncoderReusableIdleSeconds.GetValueOnAnyThread();
#endif
	}

	void RecordNvencDestroyEvent(int32 DestroyCount)
	{
		FScopeLock Lock(&ReusablePoolStatsCriticalSection);
		LastNvencDestroyTimeSeconds = FPlatformTime::Seconds();
		LastNvencDestroyCount = DestroyCount;
	}

	void RecordNvencPruneEvent(int32 PrunedCount)
	{
		FScopeLock Lock(&ReusablePoolStatsCriticalSection);
		LastNvencPruneTimeSeconds = FPlatformTime::Seconds();
		LastNvencPruneCount = PrunedCount;
	}

	double GetLastNvencDestroyAgeSeconds()
	{
		FScopeLock Lock(&ReusablePoolStatsCriticalSection);
		if (LastNvencDestroyTimeSeconds < 0.0)
		{
			return -1.0;
		}
		return FPlatformTime::Seconds() - LastNvencDestroyTimeSeconds;
	}

	int32 GetLastNvencPruneCount()
	{
		FScopeLock Lock(&ReusablePoolStatsCriticalSection);
		return LastNvencPruneCount;
	}

	int32 GetLastNvencDestroyCount()
	{
		FScopeLock Lock(&ReusablePoolStatsCriticalSection);
		return LastNvencDestroyCount;
	}

	bool TryReserveGpuEncoderSlot(FString& OutReason)
	{
		const int32 MaxGpuEncoders = FMath::Max(0, CVarRuntimeRecMaxGpuVideoEncoders.GetValueOnAnyThread());
		if (MaxGpuEncoders == 0)
		{
			OutReason = TEXT("GPU video encoder slots are disabled by RuntimeRec.RenderTarget.MaxGpuVideoEncoders.");
			return false;
		}

		FScopeLock Lock(&GpuEncoderSlotCriticalSection);
		if (ActiveGpuEncoderSlots >= MaxGpuEncoders)
		{
			OutReason = FString::Printf(
				TEXT("GPU video encoder slot limit reached (%d active, max %d)."),
				ActiveGpuEncoderSlots,
				MaxGpuEncoders);
			return false;
		}

		++ActiveGpuEncoderSlots;
		return true;
	}

	void ReleaseGpuEncoderSlot()
	{
		FScopeLock Lock(&GpuEncoderSlotCriticalSection);
		ActiveGpuEncoderSlots = FMath::Max(0, ActiveGpuEncoderSlots - 1);
	}

#if PLATFORM_WINDOWS
	FString HResultToString(HRESULT Result)
	{
		return FString::Printf(TEXT("HRESULT 0x%08X"), static_cast<uint32>(Result));
	}

	bool CheckHr(HRESULT Result, const TCHAR* Context, FString& OutError)
	{
		if (SUCCEEDED(Result))
		{
			return true;
		}

		OutError = FString::Printf(TEXT("%s failed: %s"), Context, *HResultToString(Result));
		return false;
	}

	FString NvEncErrorToString(void* Encoder, NVENCSTATUS Result)
	{
		const FString Detail = FAPI::Get<FNVENC>().GetErrorString(Encoder, Result);
		return Detail.IsEmpty()
			? FString::Printf(TEXT("NVENC failed: %d"), static_cast<int32>(Result))
			: Detail;
	}

	bool CheckNvEnc(NVENCSTATUS Result, void* Encoder, const TCHAR* Context, FString& OutError)
	{
		if (Result == NV_ENC_SUCCESS)
		{
			return true;
		}

		OutError = FString::Printf(TEXT("%s failed: %s [%d]"), Context, *NvEncErrorToString(Encoder, Result), static_cast<int32>(Result));
		return false;
	}

	void LogNvEncCallResult(const TCHAR* Context, void* Encoder, NVENCSTATUS Result)
	{
		UE_LOG(
			LogTemp,
			Display,
			TEXT("%s [Encoder=%p] Result=%d (%s)"),
			Context,
			Encoder,
			static_cast<int32>(Result),
			*NvEncErrorToString(Encoder, Result));
	}
#endif
}

class FRuntimeRecGpuVideoEncoder::FState
{
public:
	TSharedPtr<FVideoResourceRHI> StagingResource;

#if PLATFORM_WINDOWS
	struct FInputSlot
	{
		TRefCountPtr<ID3D12Resource> TextureResource;
		FTextureRHIRef TextureRHI;
		NV_ENC_REGISTERED_PTR RegisteredResource = nullptr;
	};

	struct FOutputSlot
	{
		TRefCountPtr<ID3D12Resource> BitstreamResource;
		NV_ENC_REGISTERED_PTR RegisteredResource = nullptr;
	};

	struct FPendingPacket
	{
		int32 OutputSlotIndex = INDEX_NONE;
		NV_ENC_OUTPUT_RESOURCE_D3D12 OutputResource = {};
		NV_ENC_MAP_INPUT_RESOURCE OutputMapResource = {};
		uint64 Timestamp = 0;
		bool bIsKeyframe = false;
		int64 OutputFenceValue = 0;
	};

	TRefCountPtr<ID3D12Device> D3D12Device;
	TRefCountPtr<ID3D12Fence> InputFence;
	TRefCountPtr<ID3D12Fence> OutputFence;
	int32 DeviceRemovedReason = 0;
	TStaticArray<FInputSlot, RuntimeRecGpuEncoderInputBufferCount> InputSlots;
	TArray<FOutputSlot> OutputSlots;
	TArray<int32> FreeOutputSlots;
	TArray<FPendingPacket> PendingPackets;
	int64 InputFenceValue = 0;
	int64 OutputFenceValue = 0;
	double IdleSinceSeconds = 0.0;

	void* Encoder = nullptr;
	int32 ReuseWidth = 0;
	int32 ReuseHeight = 0;
	int32 ReuseFPS = 0;
	int32 ReuseBitrateKbps = 0;
	void* HardwareDeviceIdentity = nullptr;
	void* VideoContextIdentity = nullptr;
	void* D3D12DeviceIdentity = nullptr;
	bool bForceNextFrameIdr = true;
	NV_ENC_CONFIG EncodeConfig = {};
	NV_ENC_INITIALIZE_PARAMS InitializeParams = {};

	IMFSinkWriter* SinkWriter = nullptr;
	DWORD StreamIndex = 0;
	bool bMfStarted = false;
#endif
};

#if PLATFORM_WINDOWS
void FRuntimeRecGpuVideoEncoder::DestroyStateResources(FState& InState)
{
	const int32 PendingPacketCount = InState.PendingPackets.Num();
	int32 MappedPacketCount = 0;
	for (const FState::FPendingPacket& PendingPacket : InState.PendingPackets)
	{
		if (PendingPacket.OutputMapResource.mappedResource)
		{
			++MappedPacketCount;
		}
	}

	int32 RegisteredInputCount = 0;
	for (const FState::FInputSlot& InputSlot : InState.InputSlots)
	{
		if (InputSlot.RegisteredResource)
		{
			++RegisteredInputCount;
		}
	}

	int32 RegisteredOutputCount = 0;
	for (const FState::FOutputSlot& OutputSlot : InState.OutputSlots)
	{
		if (OutputSlot.RegisteredResource)
		{
			++RegisteredOutputCount;
		}
	}

	UE_LOG(
		LogTemp,
		Display,
		TEXT("RuntimeRec GPU encoder destroy begin [State=%p] PendingPackets=%d MappedPackets=%d RegisteredInputs=%d RegisteredOutputs=%d FreeOutputSlots=%d OutputSlots=%d Encoder=%d SinkWriter=%d MfStarted=%d"),
		&InState,
		PendingPacketCount,
		MappedPacketCount,
		RegisteredInputCount,
		RegisteredOutputCount,
		InState.FreeOutputSlots.Num(),
		InState.OutputSlots.Num(),
		InState.Encoder ? 1 : 0,
		InState.SinkWriter ? 1 : 0,
		InState.bMfStarted ? 1 : 0);

	for (FState::FPendingPacket& PendingPacket : InState.PendingPackets)
	{
		if (PendingPacket.OutputMapResource.mappedResource && InState.Encoder)
		{
			const NVENCSTATUS UnmapResult = FAPI::Get<FNVENC>().nvEncUnmapInputResource(InState.Encoder, PendingPacket.OutputMapResource.mappedResource);
			LogNvEncCallResult(TEXT("RuntimeRec GPU encoder nvEncUnmapInputResource pending"), InState.Encoder, UnmapResult);
			PendingPacket.OutputMapResource.mappedResource = nullptr;
		}
	}

	InState.PendingPackets.Reset();
	InState.FreeOutputSlots.Reset();

	if (InState.Encoder)
	{
		for (FState::FInputSlot& InputSlot : InState.InputSlots)
		{
			if (InputSlot.RegisteredResource)
			{
				const NVENCSTATUS UnregisterResult = FAPI::Get<FNVENC>().nvEncUnregisterResource(InState.Encoder, InputSlot.RegisteredResource);
				LogNvEncCallResult(TEXT("RuntimeRec GPU encoder nvEncUnregisterResource input"), InState.Encoder, UnregisterResult);
				InputSlot.RegisteredResource = nullptr;
			}
		}

		for (FState::FOutputSlot& OutputSlot : InState.OutputSlots)
		{
			if (OutputSlot.RegisteredResource)
			{
				const NVENCSTATUS UnregisterResult = FAPI::Get<FNVENC>().nvEncUnregisterResource(InState.Encoder, OutputSlot.RegisteredResource);
				LogNvEncCallResult(TEXT("RuntimeRec GPU encoder nvEncUnregisterResource output"), InState.Encoder, UnregisterResult);
				OutputSlot.RegisteredResource = nullptr;
			}
		}

		const NVENCSTATUS DestroyResult = FAPI::Get<FNVENC>().nvEncDestroyEncoder(InState.Encoder);
		LogNvEncCallResult(TEXT("RuntimeRec GPU encoder nvEncDestroyEncoder"), InState.Encoder, DestroyResult);
		InState.Encoder = nullptr;
	}

	InState.StagingResource.Reset();
	for (FState::FInputSlot& InputSlot : InState.InputSlots)
	{
		InputSlot.TextureRHI.SafeRelease();
		InputSlot.TextureResource.SafeRelease();
	}
	for (FState::FOutputSlot& OutputSlot : InState.OutputSlots)
	{
		OutputSlot.BitstreamResource.SafeRelease();
	}
	InState.OutputSlots.Reset();

	UE_LOG(LogTemp, Display, TEXT("RuntimeRec GPU encoder destroy end."));
}
#endif

FRuntimeRecGpuVideoEncoder::FRuntimeRecGpuVideoEncoder()
{
}

FRuntimeRecGpuVideoEncoder::~FRuntimeRecGpuVideoEncoder()
{
	FString IgnoredError;
	Stop(IgnoredError);
}

FCriticalSection& FRuntimeRecGpuVideoEncoder::GetReusableStateCriticalSection()
{
	static FCriticalSection CriticalSection;
	return CriticalSection;
}

TArray<TUniquePtr<FRuntimeRecGpuVideoEncoder::FState>>& FRuntimeRecGpuVideoEncoder::GetReusableStates()
{
	static TArray<TUniquePtr<FState>> ReusableStates;
	return ReusableStates;
}

int32 GetReusablePoolCount()
{
	FScopeLock Lock(&FRuntimeRecGpuVideoEncoder::GetReusableStateCriticalSection());
	return FRuntimeRecGpuVideoEncoder::GetReusableStates().Num();
}

TUniquePtr<FRuntimeRecGpuVideoEncoder::FState> FRuntimeRecGpuVideoEncoder::AcquireReusableState(
	int32 InWidth,
	int32 InHeight,
	int32 InFPS,
	int32 InBitrateKbps,
	int32 OutputSlotCount,
	void* HardwareDeviceIdentity,
	void* VideoContextIdentity,
	void* D3D12DeviceIdentity,
	int32 DeviceRemovedReason)
{
#if PLATFORM_WINDOWS
	TUniquePtr<FState> ReusedState;
	TArray<TUniquePtr<FState>> StaleStatesToDestroy;

	{
		FScopeLock Lock(&GetReusableStateCriticalSection());
		TArray<TUniquePtr<FState>>& ReusableStates = GetReusableStates();
		for (int32 StateIndex = 0; StateIndex < ReusableStates.Num();)
		{
			const TUniquePtr<FState>& Candidate = ReusableStates[StateIndex];
			if (!Candidate.IsValid())
			{
				ReusableStates.RemoveAtSwap(StateIndex, 1, EAllowShrinking::No);
				ReleaseGpuEncoderSlot();
				continue;
			}

			const bool bIdentityMatches =
				Candidate->HardwareDeviceIdentity == HardwareDeviceIdentity &&
				Candidate->VideoContextIdentity == VideoContextIdentity &&
				Candidate->D3D12DeviceIdentity == D3D12DeviceIdentity &&
				Candidate->DeviceRemovedReason == DeviceRemovedReason;

			const bool bStateIsBroken =
				!Candidate->Encoder ||
				Candidate->SinkWriter ||
				Candidate->bMfStarted ||
				Candidate->PendingPackets.Num() > 0;

			if (bStateIsBroken || !bIdentityMatches)
			{
				int32 RegisteredInputCount = 0;
				for (const FState::FInputSlot& InputSlot : Candidate->InputSlots)
				{
					if (InputSlot.RegisteredResource)
					{
						++RegisteredInputCount;
					}
				}

				int32 RegisteredOutputCount = 0;
				for (const FState::FOutputSlot& OutputSlot : Candidate->OutputSlots)
				{
					if (OutputSlot.RegisteredResource)
					{
						++RegisteredOutputCount;
					}
				}

				const TCHAR* Reason = bStateIsBroken ? TEXT("Broken") : TEXT("DeviceMismatch");
				UE_LOG(
					LogTemp,
					Display,
					TEXT("RuntimeRec GPU encoder reusable state rejected [Reason=%s] Encoder=%p Device=%p DeviceRemovedReason=%s RegisteredInputs=%d RegisteredOutputs=%d"),
					Reason,
					Candidate->Encoder,
					Candidate->D3D12Device.GetReference(),
					*HResultToString(static_cast<HRESULT>(Candidate->DeviceRemovedReason)),
					RegisteredInputCount,
					RegisteredOutputCount);
				StaleStatesToDestroy.Add(MoveTemp(ReusableStates[StateIndex]));
				ReusableStates.RemoveAtSwap(StateIndex, 1, EAllowShrinking::No);
				continue;
			}

			if (Candidate->ReuseWidth != InWidth ||
				Candidate->ReuseHeight != InHeight ||
				Candidate->ReuseFPS != InFPS ||
				Candidate->ReuseBitrateKbps != InBitrateKbps ||
				Candidate->OutputSlots.Num() != OutputSlotCount)
			{
				++StateIndex;
				continue;
			}

			ReusedState = MoveTemp(ReusableStates[StateIndex]);
			ReusableStates.RemoveAtSwap(StateIndex, 1, EAllowShrinking::No);
			break;
		}
	}

	for (TUniquePtr<FState>& StaleState : StaleStatesToDestroy)
	{
		if (StaleState.IsValid())
		{
			DestroyStateResources(*StaleState);
			ReleaseGpuEncoderSlot();
		}
	}

	if (StaleStatesToDestroy.Num() > 0)
	{
		RecordNvencDestroyEvent(StaleStatesToDestroy.Num());
	}

	return ReusedState;
#endif
}

bool FRuntimeRecGpuVideoEncoder::IsPreferred()
{
	return CVarRuntimeRecGpuVideoEncoder.GetValueOnAnyThread() != 0;
}

bool FRuntimeRecGpuVideoEncoder::IsAvailable(FString& OutReason)
{
#if PLATFORM_WINDOWS
	if (!IsPreferred())
	{
		OutReason = TEXT("GPU video encoder is disabled by RuntimeRec.RenderTarget.GpuVideoEncoder.");
		return false;
	}

	if (!GDynamicRHI || GDynamicRHI->GetInterfaceType() != ERHIInterfaceType::D3D12)
	{
		OutReason = TEXT("GPU video encoder currently requires D3D12 RHI.");
		return false;
	}

	if (!FModuleManager::Get().LoadModulePtr<IModuleInterface>(TEXT("AVCodecsCore")) ||
		!FModuleManager::Get().LoadModulePtr<IModuleInterface>(TEXT("AVCodecsCoreRHI")) ||
		!FModuleManager::Get().LoadModulePtr<IModuleInterface>(TEXT("NVCodecs")) ||
		!FModuleManager::Get().LoadModulePtr<IModuleInterface>(TEXT("NVCodecsRHI")) ||
		!FModuleManager::Get().LoadModulePtr<IModuleInterface>(TEXT("NVENC")))
	{
		OutReason = TEXT("AVCodecs/NVENC modules are not available.");
		return false;
	}

	if (!FAVDevice::GetHardwareDevice()->HasContext<FVideoContextD3D12>())
	{
		OutReason = TEXT("AVCodecs D3D12 device context is not available.");
		return false;
	}

	if (!FAPI::Get<FNVENC>().IsValid())
	{
		OutReason = TEXT("NVENC API is not available.");
		return false;
	}

	return true;
#else
	OutReason = TEXT("GPU video encoder is currently implemented for Windows only.");
	return false;
#endif
}

bool FRuntimeRecGpuVideoEncoder::Start(
	const FString& InOutputPath,
	int32 InWidth,
	int32 InHeight,
	int32 InFPS,
	int32 InBitrateKbps,
	FString& OutError)
{
	UE_LOG(
		LogTemp,
		Display,
		TEXT("RuntimeRec GPU encoder start begin [Output=%s] Size=%dx%d FPS=%d BitrateKbps=%d Started=%d Stopping=%d Reserved=%d"),
		*InOutputPath,
		InWidth,
		InHeight,
		InFPS,
		InBitrateKbps,
		bStarted ? 1 : 0,
		bStopping ? 1 : 0,
		bReservedGpuEncoderSlot ? 1 : 0);

	if (bStarted)
	{
		OutError = TEXT("GPU encoder is already started.");
		UE_LOG(LogTemp, Warning, TEXT("RuntimeRec GPU encoder start rejected: already started [Output=%s]."), *InOutputPath);
		return false;
	}

	if (InWidth <= 0 || InHeight <= 0 || InFPS <= 0 || InBitrateKbps <= 0)
	{
		OutError = TEXT("Invalid GPU encoder settings.");
		UE_LOG(LogTemp, Warning, TEXT("RuntimeRec GPU encoder start rejected: invalid settings [Output=%s]."), *InOutputPath);
		return false;
	}

	FString UnavailableReason;
	if (!IsAvailable(UnavailableReason))
	{
		OutError = UnavailableReason;
		UE_LOG(LogTemp, Warning, TEXT("RuntimeRec GPU encoder start rejected: unavailable [Output=%s] %s"), *InOutputPath, *UnavailableReason);
		return false;
	}

#if PLATFORM_WINDOWS
	{
		auto HardwareDevice = FAVDevice::GetHardwareDevice();
		const auto VideoContext = HardwareDevice->GetContext<FVideoContextD3D12>();
		const FAVDevice* HardwareDevicePtr = &HardwareDevice.Get();
		const FVideoContextD3D12* VideoContextPtr = VideoContext.Get();
		const int32 DeviceRemovedReason = static_cast<int32>(VideoContext.IsValid() && VideoContext->Device ? VideoContext->Device->GetDeviceRemovedReason() : S_OK);
		UE_LOG(
			LogTemp,
			Display,
			TEXT("RuntimeRec GPU encoder availability snapshot [Output=%s] RHI=%d GDynamicRHI=%p HardwareDevice=%p HasD3D12Context=%d VideoContext=%p NVENCValid=%d DeviceRemovedReason=%s"),
			*InOutputPath,
			GDynamicRHI ? static_cast<int32>(GDynamicRHI->GetInterfaceType()) : -1,
			GDynamicRHI,
			HardwareDevicePtr,
			VideoContext.IsValid() ? 1 : 0,
			VideoContextPtr,
			FAPI::Get<FNVENC>().IsValid() ? 1 : 0,
			*HResultToString(DeviceRemovedReason));
	}
#endif

	OutputPath = InOutputPath;
	Width = InWidth;
	Height = InHeight;
	FPS = InFPS;
	BitrateKbps = InBitrateKbps;
	bStopping = false;

	const int32 OutputSlotCount = FMath::Max(1, CVarRuntimeRecGpuVideoEncoderPendingPacketCount.GetValueOnAnyThread());
	const int32 ReusablePoolCountBeforeStart = GetReusablePoolCount();
	const float OpenCooldownSeconds = FMath::Max(0.0f, CVarRuntimeRecGpuVideoEncoderOpenCooldownSeconds.GetValueOnAnyThread());
	const double LastNvencDestroyAgeSeconds = GetLastNvencDestroyAgeSeconds();

#if PLATFORM_WINDOWS
	auto HardwareDevice = FAVDevice::GetHardwareDevice();
	const auto VideoContext = HardwareDevice->GetContext<FVideoContextD3D12>();
	const FAVDevice* HardwareDevicePtr = &HardwareDevice.Get();
	const FVideoContextD3D12* VideoContextPtr = VideoContext.Get();
	void* CurrentHardwareDeviceIdentity = const_cast<FAVDevice*>(HardwareDevicePtr);
	void* CurrentVideoContextIdentity = const_cast<FVideoContextD3D12*>(VideoContextPtr);
	void* CurrentD3D12DeviceIdentity = VideoContext.IsValid() && VideoContext->Device ? VideoContext->Device.GetReference() : nullptr;
	const int32 CurrentDeviceRemovedReason = static_cast<int32>(VideoContext.IsValid() && VideoContext->Device ? VideoContext->Device->GetDeviceRemovedReason() : S_OK);

	State = AcquireReusableState(
		Width,
		Height,
		FPS,
		BitrateKbps,
		OutputSlotCount,
		CurrentHardwareDeviceIdentity,
		CurrentVideoContextIdentity,
		CurrentD3D12DeviceIdentity,
		CurrentDeviceRemovedReason);
	if (State.IsValid())
	{
		bReservedGpuEncoderSlot = true;
		State->bForceNextFrameIdr = true;
		State->FreeOutputSlots.Reset();
		for (int32 SlotIndex = 0; SlotIndex < State->OutputSlots.Num(); ++SlotIndex)
		{
			State->FreeOutputSlots.Add(SlotIndex);
		}

		UE_LOG(
			LogTemp,
			Display,
			TEXT("RuntimeRec GPU encoder reused idle NVENC session [Output=%s] Encoder=%p OutputSlots=%d Device=%p DeviceRemovedReason=%s"),
			*OutputPath,
			State->Encoder,
			State->OutputSlots.Num(),
			State->D3D12Device.GetReference(),
			*HResultToString(static_cast<HRESULT>(State->DeviceRemovedReason)));
		if (!InitializeWriter(OutError))
		{
			UE_LOG(LogTemp, Warning, TEXT("RuntimeRec GPU encoder writer initialization failed for reused session [Output=%s]: %s"), *OutputPath, *OutError);
			DestroyActiveState(OutError);
			State.Reset();
			if (bReservedGpuEncoderSlot)
			{
				ReleaseGpuEncoderSlot();
				bReservedGpuEncoderSlot = false;
			}
			return false;
		}

		bStarted = true;
		UE_LOG(LogTemp, Display, TEXT("RuntimeRec GPU encoder start end [Output=%s] Encoder=%p Reused=1 Device=%p"), *OutputPath, State ? State->Encoder : nullptr, State ? State->D3D12Device.GetReference() : nullptr);
		return true;
	}
#endif

	const int32 ReusablePoolCountAfterAcquire = GetReusablePoolCount();
	if (ReusablePoolCountAfterAcquire == 0 &&
		LastNvencDestroyAgeSeconds >= 0.0 &&
		LastNvencDestroyAgeSeconds < static_cast<double>(OpenCooldownSeconds))
	{
		const double RemainingSeconds = static_cast<double>(OpenCooldownSeconds) - LastNvencDestroyAgeSeconds;
		UE_LOG(
			LogTemp,
			Display,
			TEXT("RuntimeRec GPU encoder start cooldown active [Output=%s] RemainingSeconds=%.3f LastNvencDestroyAgeSeconds=%.3f"),
			*InOutputPath,
			RemainingSeconds,
			LastNvencDestroyAgeSeconds);
		FPlatformProcess::Sleep(static_cast<float>(RemainingSeconds));
	}

	if (!TryReserveGpuEncoderSlot(UnavailableReason))
	{
		OutError = UnavailableReason;
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("RuntimeRec GPU encoder start rejected: slot reservation failed [Output=%s] %s ReusablePoolCountBeforeStart=%d ActiveGpuEncoderSlots=%d MaxGpuEncoderSlots=%d MaxReusableGpuVideoEncoders=%d ReusableIdleSeconds=%.3f LastNvencDestroyAgeSeconds=%.3f LastPrunedCount=%d"),
			*InOutputPath,
			*UnavailableReason,
			ReusablePoolCountBeforeStart,
			ActiveGpuEncoderSlots,
			FMath::Max(0, CVarRuntimeRecMaxGpuVideoEncoders.GetValueOnAnyThread()),
			FMath::Max(0, CVarRuntimeRecMaxReusableGpuVideoEncoders.GetValueOnAnyThread()),
			GetReusableIdleSecondsCVar(),
			LastNvencDestroyAgeSeconds,
			GetLastNvencPruneCount());
		return false;
	}
	bReservedGpuEncoderSlot = true;
	UE_LOG(LogTemp, Display, TEXT("RuntimeRec GPU encoder slot reserved [Output=%s]."), *InOutputPath);

	State = MakeUnique<FState>();

#if PLATFORM_WINDOWS
	State->HardwareDeviceIdentity = CurrentHardwareDeviceIdentity;
	State->VideoContextIdentity = CurrentVideoContextIdentity;
	State->D3D12DeviceIdentity = CurrentD3D12DeviceIdentity;
	State->D3D12Device = VideoContext->Device;
	State->DeviceRemovedReason = CurrentDeviceRemovedReason;
	if (!State->D3D12Device.IsValid())
	{
		OutError = TEXT("D3D12 device is not available for Direct NVENC.");
		UE_LOG(LogTemp, Warning, TEXT("RuntimeRec GPU encoder start failed: D3D12 device unavailable [Output=%s]."), *OutputPath);
		DestroyActiveState(OutError);
		State.Reset();
		if (bReservedGpuEncoderSlot)
		{
			ReleaseGpuEncoderSlot();
			bReservedGpuEncoderSlot = false;
		}
		return false;
	}

	UE_LOG(
		LogTemp,
		Display,
		TEXT("RuntimeRec GPU encoder D3D12 device acquired [Output=%s] HardwareDevice=%p VideoContext=%p Device=%p DeviceRemovedReason=%s"),
		*OutputPath,
		&FAVDevice::GetHardwareDevice().Get(),
		FAVDevice::GetHardwareDevice()->GetContext<FVideoContextD3D12>().Get(),
		State->D3D12Device.GetReference(),
		*HResultToString(State->D3D12Device->GetDeviceRemovedReason()));

	NV_ENC_STRUCT(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS, SessionParams);
	SessionParams.apiVersion = NVENCAPI_VERSION;
	SessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
	SessionParams.device = State->D3D12Device.GetReference();
	UE_LOG(
		LogTemp,
		Display,
		TEXT("RuntimeRec GPU encoder NVENC open params [Output=%s] StructSize=%d ApiVersion=0x%08X NVENCAPI_VERSION=0x%08X DeviceType=%d Device=%p"),
		*OutputPath,
		static_cast<int32>(sizeof(SessionParams)),
		static_cast<uint32>(SessionParams.apiVersion),
		static_cast<uint32>(NVENCAPI_VERSION),
		static_cast<int32>(SessionParams.deviceType),
		SessionParams.device);
	UE_LOG(LogTemp, Display, TEXT("RuntimeRec GPU encoder opening NVENC session [Output=%s] Device=%p"), *OutputPath, State->D3D12Device.GetReference());

	void* OpenedEncoder = nullptr;
	FString OpenSessionError;
	bool bOpenedSession = false;
	constexpr int32 MaxOpenAttempts = 2;
	for (int32 AttemptIndex = 1; AttemptIndex <= MaxOpenAttempts; ++AttemptIndex)
	{
		OpenedEncoder = nullptr;
		const NVENCSTATUS OpenResult = FAPI::Get<FNVENC>().nvEncOpenEncodeSessionEx(&SessionParams, &OpenedEncoder);
		if (CheckNvEnc(OpenResult, OpenedEncoder, TEXT("nvEncOpenEncodeSessionEx"), OpenSessionError))
		{
			State->Encoder = OpenedEncoder;
			bOpenedSession = true;
			if (AttemptIndex > 1)
			{
				UE_LOG(LogTemp, Display, TEXT("RuntimeRec GPU encoder open session recovered on retry [Output=%s] Attempt=%d"), *OutputPath, AttemptIndex);
			}
			break;
		}

		UE_LOG(LogTemp, Warning, TEXT("RuntimeRec GPU encoder open session attempt failed [Output=%s] Attempt=%d/%d: %s"), *OutputPath, AttemptIndex, MaxOpenAttempts, *OpenSessionError);
		if (AttemptIndex < MaxOpenAttempts)
		{
			UE_LOG(LogTemp, Display, TEXT("RuntimeRec GPU encoder will retry session open after a short delay [Output=%s]."), *OutputPath);
			FPlatformProcess::Sleep(1.0f);
		}
	}

	if (!bOpenedSession)
	{
		OutError = OpenSessionError;
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("RuntimeRec GPU encoder open session failed [Output=%s]: %s ReusablePoolCountBeforeStart=%d LastNvencDestroyAgeSeconds=%.3f LastNvencDestroyCount=%d LastPrunedCount=%d ActiveGpuEncoderSlots=%d MaxGpuEncoderSlots=%d MaxReusableGpuVideoEncoders=%d ReusableIdleSeconds=%.3f OpenCooldownSeconds=%.3f"),
			*OutputPath,
			*OutError,
			ReusablePoolCountBeforeStart,
			LastNvencDestroyAgeSeconds,
			GetLastNvencDestroyCount(),
			GetLastNvencPruneCount(),
			ActiveGpuEncoderSlots,
			FMath::Max(0, CVarRuntimeRecMaxGpuVideoEncoders.GetValueOnAnyThread()),
			FMath::Max(0, CVarRuntimeRecMaxReusableGpuVideoEncoders.GetValueOnAnyThread()),
			GetReusableIdleSecondsCVar(),
			OpenCooldownSeconds);
		DestroyActiveState(OutError);
		State.Reset();
		if (bReservedGpuEncoderSlot)
		{
			ReleaseGpuEncoderSlot();
			bReservedGpuEncoderSlot = false;
		}
		return false;
	}

	NV_ENC_PRESET_CONFIG PresetConfig = {};
	PresetConfig.version = NV_ENC_PRESET_CONFIG_VER;
	PresetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
	if (FAPI::Get<FNVENC>().nvEncGetEncodePresetConfigEx(
		State->Encoder,
		NV_ENC_CODEC_H264_GUID,
		NV_ENC_PRESET_P1_GUID,
		NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY,
		&PresetConfig) == NV_ENC_SUCCESS)
	{
		State->EncodeConfig = PresetConfig.presetCfg;
	}
	else
	{
		FMemory::Memzero(State->EncodeConfig);
		State->EncodeConfig.version = NV_ENC_CONFIG_VER;
	}

	State->EncodeConfig.version = NV_ENC_CONFIG_VER;
	State->EncodeConfig.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
	State->EncodeConfig.gopLength = static_cast<uint32>(FPS * 2);
	State->EncodeConfig.frameIntervalP = 1;
	State->EncodeConfig.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
	State->EncodeConfig.mvPrecision = NV_ENC_MV_PRECISION_QUARTER_PEL;
	State->EncodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
	State->EncodeConfig.rcParams.averageBitRate = static_cast<uint32>(BitrateKbps * 1000);
	State->EncodeConfig.rcParams.maxBitRate = static_cast<uint32>(BitrateKbps * 1000);
	State->EncodeConfig.rcParams.vbvBufferSize = static_cast<uint32>((BitrateKbps * 1000) / FMath::Max(FPS, 1));
	State->EncodeConfig.rcParams.vbvInitialDelay = State->EncodeConfig.rcParams.vbvBufferSize;
	State->EncodeConfig.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
	State->EncodeConfig.encodeCodecConfig.h264Config.idrPeriod = static_cast<uint32>(FPS * 2);
	State->EncodeConfig.encodeCodecConfig.h264Config.repeatSPSPPS = 1;

	FMemory::Memzero(State->InitializeParams);
	State->InitializeParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
	State->InitializeParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
	State->InitializeParams.presetGUID = NV_ENC_PRESET_P1_GUID;
	State->InitializeParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
	State->InitializeParams.encodeWidth = static_cast<uint32>(Width);
	State->InitializeParams.encodeHeight = static_cast<uint32>(Height);
	State->InitializeParams.darWidth = static_cast<uint32>(Width);
	State->InitializeParams.darHeight = static_cast<uint32>(Height);
	State->InitializeParams.maxEncodeWidth = static_cast<uint32>(Width);
	State->InitializeParams.maxEncodeHeight = static_cast<uint32>(Height);
	State->InitializeParams.frameRateNum = static_cast<uint32>(FPS);
	State->InitializeParams.frameRateDen = 1;
	State->InitializeParams.enablePTD = 1;
	State->InitializeParams.enableEncodeAsync = 0;
	State->InitializeParams.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
	State->InitializeParams.encodeConfig = &State->EncodeConfig;

	if (!CheckNvEnc(FAPI::Get<FNVENC>().nvEncInitializeEncoder(State->Encoder, &State->InitializeParams), State->Encoder, TEXT("nvEncInitializeEncoder"), OutError))
	{
		UE_LOG(LogTemp, Warning, TEXT("RuntimeRec GPU encoder initialize failed [Output=%s]: %s"), *OutputPath, *OutError);
		DestroyActiveState(OutError);
		State.Reset();
		if (bReservedGpuEncoderSlot)
		{
			ReleaseGpuEncoderSlot();
			bReservedGpuEncoderSlot = false;
		}
		return false;
	}

	if (!CheckHr(State->D3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(State->InputFence.GetInitReference())), TEXT("ID3D12Device::CreateFence input"), OutError))
	{
		UE_LOG(LogTemp, Warning, TEXT("RuntimeRec GPU encoder input fence creation failed [Output=%s]: %s"), *OutputPath, *OutError);
		DestroyActiveState(OutError);
		State.Reset();
		if (bReservedGpuEncoderSlot)
		{
			ReleaseGpuEncoderSlot();
			bReservedGpuEncoderSlot = false;
		}
		return false;
	}

	if (!CheckHr(State->D3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(State->OutputFence.GetInitReference())), TEXT("ID3D12Device::CreateFence output"), OutError))
	{
		UE_LOG(LogTemp, Warning, TEXT("RuntimeRec GPU encoder output fence creation failed [Output=%s]: %s"), *OutputPath, *OutError);
		DestroyActiveState(OutError);
		State.Reset();
		if (bReservedGpuEncoderSlot)
		{
			ReleaseGpuEncoderSlot();
			bReservedGpuEncoderSlot = false;
		}
		return false;
	}

	UE_LOG(LogTemp, Display, TEXT("RuntimeRec GPU encoder allocating output slots [Output=%s] Count=%d"), *OutputPath, OutputSlotCount);
	State->OutputSlots.SetNum(OutputSlotCount);
	State->FreeOutputSlots.Reserve(OutputSlotCount);

	D3D12_HEAP_PROPERTIES HeapProps = {};
	HeapProps.Type = D3D12_HEAP_TYPE_READBACK;

	D3D12_RESOURCE_DESC ResourceDesc = {};
	ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	ResourceDesc.Width = Align(static_cast<uint64>(Width) * static_cast<uint64>(Height) * 8, 4);
	ResourceDesc.Height = 1;
	ResourceDesc.DepthOrArraySize = 1;
	ResourceDesc.MipLevels = 1;
	ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
	ResourceDesc.SampleDesc.Count = 1;
	ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	for (int32 SlotIndex = 0; SlotIndex < OutputSlotCount; ++SlotIndex)
	{
		FState::FOutputSlot& OutputSlot = State->OutputSlots[SlotIndex];
		if (!CheckHr(State->D3D12Device->CreateCommittedResource(
			&HeapProps,
			D3D12_HEAP_FLAG_NONE,
			&ResourceDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(OutputSlot.BitstreamResource.GetInitReference())),
			TEXT("ID3D12Device::CreateCommittedResource output bitstream"),
			OutError))
		{
			UE_LOG(LogTemp, Warning, TEXT("RuntimeRec GPU encoder output slot resource creation failed [Output=%s] Slot=%d: %s"), *OutputPath, SlotIndex, *OutError);
			DestroyActiveState(OutError);
			State.Reset();
			if (bReservedGpuEncoderSlot)
			{
				ReleaseGpuEncoderSlot();
				bReservedGpuEncoderSlot = false;
			}
			return false;
		}

		NV_ENC_STRUCT(NV_ENC_REGISTER_RESOURCE, OutputRegisterResource);
		OutputRegisterResource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
		OutputRegisterResource.resourceToRegister = OutputSlot.BitstreamResource.GetReference();
		OutputRegisterResource.width = Align(static_cast<uint32>(Width * Height * 8), 4u);
		OutputRegisterResource.height = 1;
		OutputRegisterResource.pitch = 0;
		OutputRegisterResource.bufferFormat = NV_ENC_BUFFER_FORMAT_U8;
		OutputRegisterResource.bufferUsage = NV_ENC_OUTPUT_BITSTREAM;

		if (!CheckNvEnc(FAPI::Get<FNVENC>().nvEncRegisterResource(State->Encoder, &OutputRegisterResource), State->Encoder, TEXT("nvEncRegisterResource output"), OutError))
		{
			UE_LOG(LogTemp, Warning, TEXT("RuntimeRec GPU encoder output slot registration failed [Output=%s] Slot=%d: %s"), *OutputPath, SlotIndex, *OutError);
			DestroyActiveState(OutError);
			State.Reset();
			if (bReservedGpuEncoderSlot)
			{
				ReleaseGpuEncoderSlot();
				bReservedGpuEncoderSlot = false;
			}
			return false;
		}

		OutputSlot.RegisteredResource = OutputRegisterResource.registeredResource;
		State->FreeOutputSlots.Add(SlotIndex);
	}

	State->ReuseWidth = Width;
	State->ReuseHeight = Height;
	State->ReuseFPS = FPS;
	State->ReuseBitrateKbps = BitrateKbps;
#endif

	if (!InitializeWriter(OutError))
	{
		UE_LOG(LogTemp, Warning, TEXT("RuntimeRec GPU encoder writer initialization failed [Output=%s]: %s"), *OutputPath, *OutError);
		DestroyActiveState(OutError);
		State.Reset();
		if (bReservedGpuEncoderSlot)
		{
			ReleaseGpuEncoderSlot();
			bReservedGpuEncoderSlot = false;
		}
		return false;
	}

	bStarted = true;
	UE_LOG(LogTemp, Display, TEXT("RuntimeRec GPU encoder start end [Output=%s] Encoder=%p Reused=0 Device=%p"), *OutputPath, State ? State->Encoder : nullptr, State ? State->D3D12Device.GetReference() : nullptr);
	return true;
}

bool FRuntimeRecGpuVideoEncoder::EncodeTexture_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	FTextureRHIRef SourceTexture,
	int64 FrameIndex,
	FString& OutError)
{
	if (!bStarted || bStopping)
	{
		OutError = TEXT("GPU encoder is not accepting frames.");
		return false;
	}

	if (!SourceTexture.IsValid())
	{
		OutError = TEXT("GPU encoder source texture is invalid.");
		return false;
	}

	const FRHITextureDesc& TextureDesc = SourceTexture->GetDesc();
	if (TextureDesc.Format != PF_B8G8R8A8 || TextureDesc.IsMultisample())
	{
		OutError = TEXT("GPU encoder source texture must be non-MSAA PF_B8G8R8A8.");
		return false;
	}

	FScopeLock Lock(&CriticalSection);
	if (!State || !State->Encoder)
	{
		OutError = TEXT("GPU encoder is not initialized.");
		return false;
	}

	const int32 InputSlotIndex = static_cast<int32>(FrameIndex % RuntimeRecGpuEncoderInputBufferCount);
	FState::FInputSlot& InputSlot = State->InputSlots[InputSlotIndex];

	if (!InputSlot.TextureResource.IsValid() || !InputSlot.TextureRHI.IsValid())
	{
		D3D12_HEAP_PROPERTIES HeapProps = {};
		HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_RESOURCE_DESC ResourceDesc = {};
		ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		ResourceDesc.Width = static_cast<UINT64>(Width);
		ResourceDesc.Height = static_cast<UINT>(Height);
		ResourceDesc.DepthOrArraySize = 1;
		ResourceDesc.MipLevels = 1;
		ResourceDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		ResourceDesc.SampleDesc.Count = 1;
		ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		if (!CheckHr(State->D3D12Device->CreateCommittedResource(
			&HeapProps,
			D3D12_HEAP_FLAG_NONE,
			&ResourceDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(InputSlot.TextureResource.GetInitReference())),
			TEXT("ID3D12Device::CreateCommittedResource input texture"),
			OutError))
		{
			return false;
		}

		InputSlot.TextureRHI = GetID3D12DynamicRHI()->RHICreateTexture2DFromResource(
			PF_B8G8R8A8,
			ETextureCreateFlags::Shared,
			FClearValueBinding::None,
			InputSlot.TextureResource.GetReference());
		if (!InputSlot.TextureRHI.IsValid())
		{
			OutError = TEXT("Failed to wrap Direct NVENC input texture as RHI texture.");
			return false;
		}
	}

	RHICmdList.Transition(FRHITransitionInfo(SourceTexture, ERHIAccess::Unknown, ERHIAccess::CopySrc));
	RHICmdList.Transition(FRHITransitionInfo(InputSlot.TextureRHI, ERHIAccess::Unknown, ERHIAccess::CopyDest));
	RHICmdList.CopyTexture(SourceTexture, InputSlot.TextureRHI, FRHICopyTextureInfo());
	RHICmdList.Transition(FRHITransitionInfo(InputSlot.TextureRHI, ERHIAccess::CopyDest, ERHIAccess::SRVGraphics));

	const int64 InputFenceValue = ++State->InputFenceValue;
	RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread, ERHISubmitFlags::SubmitToGPU);

	HRESULT InputFenceSignalResult = S_OK;
	TRefCountPtr<ID3D12Fence> InputFence = State->InputFence;
	GetID3D12DynamicRHI()->RHIRunOnQueue(
		ED3D12RHIRunOnQueueType::Graphics,
		[InputFence, InputFenceValue, &InputFenceSignalResult](ID3D12CommandQueue* Queue)
		{
			InputFenceSignalResult = Queue->Signal(InputFence.GetReference(), static_cast<UINT64>(InputFenceValue));
		},
		true);
	if (!CheckHr(InputFenceSignalResult, TEXT("ID3D12CommandQueue::Signal input fence"), OutError))
	{
		return false;
	}

	if (!InputSlot.RegisteredResource)
	{
		NV_ENC_STRUCT(NV_ENC_REGISTER_RESOURCE, InputRegisterResource);
		InputRegisterResource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
		InputRegisterResource.resourceToRegister = InputSlot.TextureResource.GetReference();
		InputRegisterResource.width = static_cast<uint32>(Width);
		InputRegisterResource.height = static_cast<uint32>(Height);
		InputRegisterResource.pitch = 0;
		InputRegisterResource.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
		InputRegisterResource.bufferUsage = NV_ENC_INPUT_IMAGE;

		if (!CheckNvEnc(FAPI::Get<FNVENC>().nvEncRegisterResource(State->Encoder, &InputRegisterResource), State->Encoder, TEXT("nvEncRegisterResource input"), OutError))
		{
			return false;
		}

		InputSlot.RegisteredResource = InputRegisterResource.registeredResource;
	}

	NV_ENC_STRUCT(NV_ENC_MAP_INPUT_RESOURCE, InputMapResource);
	InputMapResource.registeredResource = InputSlot.RegisteredResource;
	if (!CheckNvEnc(FAPI::Get<FNVENC>().nvEncMapInputResource(State->Encoder, &InputMapResource), State->Encoder, TEXT("nvEncMapInputResource input"), OutError))
	{
		return false;
	}

	if (!DrainPackets(OutError))
	{
		FAPI::Get<FNVENC>().nvEncUnmapInputResource(State->Encoder, InputMapResource.mappedResource);
		return false;
	}

	if (State->FreeOutputSlots.Num() == 0)
	{
		if (!DrainPackets(OutError, true))
		{
			FAPI::Get<FNVENC>().nvEncUnmapInputResource(State->Encoder, InputMapResource.mappedResource);
			return false;
		}
	}

	if (State->FreeOutputSlots.Num() == 0)
	{
		FAPI::Get<FNVENC>().nvEncUnmapInputResource(State->Encoder, InputMapResource.mappedResource);
		OutError = TEXT("GPU encoder output slot queue is full.");
		return false;
	}

	const int32 OutputSlotIndex = State->FreeOutputSlots.Pop(EAllowShrinking::No);
	FState::FOutputSlot& OutputSlot = State->OutputSlots[OutputSlotIndex];
	if (!OutputSlot.RegisteredResource || !OutputSlot.BitstreamResource.IsValid())
	{
		FAPI::Get<FNVENC>().nvEncUnmapInputResource(State->Encoder, InputMapResource.mappedResource);
		State->FreeOutputSlots.Add(OutputSlotIndex);
		OutError = TEXT("GPU encoder output slot is not initialized.");
		return false;
	}

	NV_ENC_STRUCT(NV_ENC_MAP_INPUT_RESOURCE, OutputMapResource);
	OutputMapResource.registeredResource = OutputSlot.RegisteredResource;
	if (!CheckNvEnc(FAPI::Get<FNVENC>().nvEncMapInputResource(State->Encoder, &OutputMapResource), State->Encoder, TEXT("nvEncMapInputResource output"), OutError))
	{
		FAPI::Get<FNVENC>().nvEncUnmapInputResource(State->Encoder, InputMapResource.mappedResource);
		State->FreeOutputSlots.Add(OutputSlotIndex);
		return false;
	}

		NV_ENC_INPUT_RESOURCE_D3D12 InputResource = {};
		InputResource.version = NV_ENC_INPUT_RESOURCE_D3D12_VER;
		InputResource.inputFencePoint.version = NV_ENC_FENCE_POINT_D3D12_VER;
		InputResource.inputFencePoint.bWait = true;
		InputResource.inputFencePoint.pFence = State->InputFence.GetReference();
		InputResource.inputFencePoint.waitValue = InputFenceValue;
		InputResource.pInputBuffer = InputMapResource.mappedResource;

		++State->OutputFenceValue;
		NV_ENC_OUTPUT_RESOURCE_D3D12 OutputResource = {};
		OutputResource.version = NV_ENC_OUTPUT_RESOURCE_D3D12_VER;
		OutputResource.outputFencePoint.version = NV_ENC_FENCE_POINT_D3D12_VER;
		OutputResource.outputFencePoint.bSignal = true;
		OutputResource.outputFencePoint.pFence = State->OutputFence.GetReference();
		OutputResource.outputFencePoint.signalValue = State->OutputFenceValue;
		OutputResource.pOutputBuffer = OutputMapResource.mappedResource;

	NV_ENC_STRUCT(NV_ENC_PIC_PARAMS, Picture);
	Picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
	Picture.inputTimeStamp = static_cast<uint32>(FrameIndex);
	Picture.inputBuffer = &InputResource;
	Picture.bufferFmt = InputMapResource.mappedBufferFmt;
		Picture.inputWidth = static_cast<uint32>(Width);
		Picture.inputHeight = static_cast<uint32>(Height);
		Picture.outputBitstream = &OutputResource;
		if (FrameIndex == 0 || State->bForceNextFrameIdr)
		{
			Picture.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
			Picture.encodePicFlags |= NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
		}

		if (!CheckNvEnc(FAPI::Get<FNVENC>().nvEncEncodePicture(State->Encoder, &Picture), State->Encoder, TEXT("nvEncEncodePicture"), OutError))
		{
		FAPI::Get<FNVENC>().nvEncUnmapInputResource(State->Encoder, OutputMapResource.mappedResource);
		FAPI::Get<FNVENC>().nvEncUnmapInputResource(State->Encoder, InputMapResource.mappedResource);
		State->FreeOutputSlots.Add(OutputSlotIndex);
		return false;
	}

	if (!CheckNvEnc(FAPI::Get<FNVENC>().nvEncUnmapInputResource(State->Encoder, InputMapResource.mappedResource), State->Encoder, TEXT("nvEncUnmapInputResource input"), OutError))
	{
		FAPI::Get<FNVENC>().nvEncUnmapInputResource(State->Encoder, OutputMapResource.mappedResource);
		State->FreeOutputSlots.Add(OutputSlotIndex);
		return false;
	}

	FState::FPendingPacket PendingPacket;
	PendingPacket.OutputSlotIndex = OutputSlotIndex;
	PendingPacket.OutputResource = OutputResource;
		PendingPacket.OutputMapResource = OutputMapResource;
		PendingPacket.Timestamp = static_cast<uint64>(FrameIndex);
		PendingPacket.bIsKeyframe = (FrameIndex == 0 || State->bForceNextFrameIdr);
		PendingPacket.OutputFenceValue = State->OutputFenceValue;
		OutputMapResource.mappedResource = nullptr;
		State->PendingPackets.Add(MoveTemp(PendingPacket));
		State->bForceNextFrameIdr = false;

		return true;
	}

bool FRuntimeRecGpuVideoEncoder::Stop(FString& OutError)
{
	if (!bStarted)
	{
		return true;
	}

	bStopping = true;
	{
		FScopeLock Lock(&CriticalSection);
		UE_LOG(
			LogTemp,
			Display,
			TEXT("RuntimeRec GPU encoder stop begin [Output=%s] PendingPackets=%d FreeOutputSlots=%d OutputSlots=%d Encoder=%d SinkWriter=%d"),
			*OutputPath,
			State ? State->PendingPackets.Num() : -1,
			State ? State->FreeOutputSlots.Num() : -1,
			State ? State->OutputSlots.Num() : -1,
			State && State->Encoder ? 1 : 0,
			State && State->SinkWriter ? 1 : 0);
		if (!DrainPackets(OutError, true))
		{
			UE_LOG(LogTemp, Warning, TEXT("RuntimeRec GPU encoder stop drain failed [Output=%s]: %s"), *OutputPath, *OutError);
			DestroyActiveState(OutError);
			bStarted = false;
			if (bReservedGpuEncoderSlot)
			{
				ReleaseGpuEncoderSlot();
				bReservedGpuEncoderSlot = false;
			}
			return false;
		}

		if (!SendEndOfStream(OutError))
		{
			UE_LOG(LogTemp, Warning, TEXT("RuntimeRec GPU encoder stop EOS failed [Output=%s]: %s"), *OutputPath, *OutError);
			DestroyActiveState(OutError);
			bStarted = false;
			if (bReservedGpuEncoderSlot)
			{
				ReleaseGpuEncoderSlot();
				bReservedGpuEncoderSlot = false;
			}
			return false;
		}

		if (!DrainPackets(OutError, true))
		{
			UE_LOG(LogTemp, Warning, TEXT("RuntimeRec GPU encoder stop final drain failed [Output=%s]: %s"), *OutputPath, *OutError);
			DestroyActiveState(OutError);
			bStarted = false;
			if (bReservedGpuEncoderSlot)
			{
				ReleaseGpuEncoderSlot();
				bReservedGpuEncoderSlot = false;
			}
			return false;
		}

		UE_LOG(
			LogTemp,
			Display,
			TEXT("RuntimeRec GPU encoder stop drain complete [Output=%s] PendingPackets=%d"),
			*OutputPath,
			State ? State->PendingPackets.Num() : -1);
	}

	bStarted = false;
	if (RetireStateForReuse())
	{
		bReservedGpuEncoderSlot = false;
		UE_LOG(LogTemp, Display, TEXT("RuntimeRec GPU encoder retained idle NVENC session for reuse [Output=%s]."), *OutputPath);
	}
	else
	{
		DestroyActiveState(OutError);
		if (bReservedGpuEncoderSlot)
		{
			ReleaseGpuEncoderSlot();
			bReservedGpuEncoderSlot = false;
		}
	}
	UE_LOG(LogTemp, Display, TEXT("RuntimeRec GPU encoder stop end [Output=%s]."), *OutputPath);
	return true;
}

bool FRuntimeRecGpuVideoEncoder::InitializeWriter(FString& OutError)
{
#if PLATFORM_WINDOWS
	HRESULT Hr = MFStartup(MF_VERSION);
	if (!CheckHr(Hr, TEXT("MFStartup"), OutError))
	{
		return false;
	}
	State->bMfStarted = true;

	Hr = MFCreateSinkWriterFromURL(*OutputPath, nullptr, nullptr, &State->SinkWriter);
	if (!CheckHr(Hr, TEXT("MFCreateSinkWriterFromURL"), OutError))
	{
		return false;
	}

	const uint32 VideoBitrate = static_cast<uint32>(BitrateKbps * 1000);

	TRefCountPtr<IMFMediaType> MediaType;
	Hr = MFCreateMediaType(MediaType.GetInitReference());
	if (!CheckHr(Hr, TEXT("MFCreateMediaType video"), OutError))
	{
		return false;
	}

	MediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	MediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
	MediaType->SetUINT32(MF_MT_AVG_BITRATE, VideoBitrate);
	MediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	MFSetAttributeSize(MediaType, MF_MT_FRAME_SIZE, Width, Height);
	MFSetAttributeRatio(MediaType, MF_MT_FRAME_RATE, FPS, 1);
	MFSetAttributeRatio(MediaType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

	Hr = State->SinkWriter->AddStream(MediaType, &State->StreamIndex);
	if (!CheckHr(Hr, TEXT("IMFSinkWriter::AddStream"), OutError))
	{
		return false;
	}

	Hr = State->SinkWriter->SetInputMediaType(State->StreamIndex, MediaType, nullptr);
	if (!CheckHr(Hr, TEXT("IMFSinkWriter::SetInputMediaType"), OutError))
	{
		return false;
	}

	Hr = State->SinkWriter->BeginWriting();
	if (!CheckHr(Hr, TEXT("IMFSinkWriter::BeginWriting"), OutError))
	{
		return false;
	}

	return true;
#else
	OutError = TEXT("RuntimeRec GPU MP4 muxing is currently implemented for Windows only.");
	return false;
#endif
}

bool FRuntimeRecGpuVideoEncoder::WritePacket(const uint8* PacketData, uint64 PacketDataSize, uint64 Timestamp, bool bIsKeyframe, FString& OutError)
{
#if PLATFORM_WINDOWS
	if (!State || !State->SinkWriter)
	{
		OutError = TEXT("GPU encoder sink writer is not initialized.");
		return false;
	}

	if (!PacketData || PacketDataSize == 0 || PacketDataSize > MAX_uint32)
	{
		OutError = TEXT("GPU encoder produced an invalid packet.");
		return false;
	}

	TRefCountPtr<IMFMediaBuffer> Buffer;
	HRESULT Hr = MFCreateAlignedMemoryBuffer(static_cast<DWORD>(PacketDataSize), MF_1_BYTE_ALIGNMENT, Buffer.GetInitReference());
	if (!CheckHr(Hr, TEXT("MFCreateAlignedMemoryBuffer"), OutError))
	{
		return false;
	}

	BYTE* Data = nullptr;
	Hr = Buffer->Lock(&Data, nullptr, nullptr);
	if (!CheckHr(Hr, TEXT("IMFMediaBuffer::Lock"), OutError))
	{
		return false;
	}

	FMemory::Memcpy(Data, PacketData, static_cast<SIZE_T>(PacketDataSize));
	Buffer->Unlock();
	Buffer->SetCurrentLength(static_cast<DWORD>(PacketDataSize));

	TRefCountPtr<IMFSample> Sample;
	Hr = MFCreateSample(Sample.GetInitReference());
	if (!CheckHr(Hr, TEXT("MFCreateSample"), OutError))
	{
		return false;
	}

	Hr = Sample->AddBuffer(Buffer);
	if (!CheckHr(Hr, TEXT("IMFSample::AddBuffer"), OutError))
	{
		return false;
	}

	const LONGLONG FrameDuration = 10'000'000LL / FPS;
	Sample->SetSampleTime(static_cast<LONGLONG>(Timestamp) * FrameDuration);
	Sample->SetSampleDuration(FrameDuration);
	Sample->SetUINT32(MFSampleExtension_CleanPoint, bIsKeyframe ? 1u : 0u);

	Hr = State->SinkWriter->WriteSample(State->StreamIndex, Sample);
	return CheckHr(Hr, TEXT("IMFSinkWriter::WriteSample"), OutError);
#else
	OutError = TEXT("RuntimeRec GPU MP4 muxing is currently implemented for Windows only.");
	return false;
#endif
}

bool FRuntimeRecGpuVideoEncoder::DrainPackets(FString& OutError, bool bWaitForAll)
{
#if PLATFORM_WINDOWS
	if (!State || !State->Encoder || !State->SinkWriter)
	{
		return true;
	}

	while (State->PendingPackets.Num() > 0)
	{
		FState::FPendingPacket& PendingPacket = State->PendingPackets[0];
		if (!State->OutputFence.IsValid())
		{
			OutError = TEXT("GPU encoder output fence is not initialized.");
			return false;
		}

		const uint64 CompletedFenceValue = State->OutputFence->GetCompletedValue();
		if (!bWaitForAll && CompletedFenceValue < static_cast<uint64>(PendingPacket.OutputFenceValue))
		{
			break;
		}

		NV_ENC_STRUCT(NV_ENC_LOCK_BITSTREAM, BitstreamLock);
		BitstreamLock.outputBitstream = &PendingPacket.OutputResource;
		BitstreamLock.doNotWait = bWaitForAll ? 0 : 1;

		const NVENCSTATUS LockResult = FAPI::Get<FNVENC>().nvEncLockBitstream(State->Encoder, &BitstreamLock);
		if (LockResult == NV_ENC_ERR_ENCODER_BUSY)
		{
			if (!bWaitForAll)
			{
				break;
			}

			OutError = TEXT("GPU encoder bitstream lock stayed busy while waiting for completion.");
			return false;
		}

		if (!CheckNvEnc(LockResult, State->Encoder, TEXT("nvEncLockBitstream"), OutError))
		{
			return false;
		}

		const bool bIsKeyframe = (BitstreamLock.pictureType & NV_ENC_PIC_TYPE_IDR) != 0;
		if (!WritePacket(
			static_cast<const uint8*>(BitstreamLock.bitstreamBufferPtr),
			BitstreamLock.bitstreamSizeInBytes,
			BitstreamLock.outputTimeStamp,
			bIsKeyframe,
			OutError))
		{
			FAPI::Get<FNVENC>().nvEncUnlockBitstream(State->Encoder, &PendingPacket.OutputResource);
			if (PendingPacket.OutputMapResource.mappedResource)
			{
				FAPI::Get<FNVENC>().nvEncUnmapInputResource(State->Encoder, PendingPacket.OutputMapResource.mappedResource);
				PendingPacket.OutputMapResource.mappedResource = nullptr;
			}
			if (PendingPacket.OutputSlotIndex != INDEX_NONE)
			{
				State->FreeOutputSlots.Add(PendingPacket.OutputSlotIndex);
			}
			State->PendingPackets.RemoveAt(0);
			return false;
		}

		if (!CheckNvEnc(FAPI::Get<FNVENC>().nvEncUnlockBitstream(State->Encoder, &PendingPacket.OutputResource), State->Encoder, TEXT("nvEncUnlockBitstream"), OutError))
		{
			if (PendingPacket.OutputMapResource.mappedResource)
			{
				FAPI::Get<FNVENC>().nvEncUnmapInputResource(State->Encoder, PendingPacket.OutputMapResource.mappedResource);
				PendingPacket.OutputMapResource.mappedResource = nullptr;
			}
			if (PendingPacket.OutputSlotIndex != INDEX_NONE)
			{
				State->FreeOutputSlots.Add(PendingPacket.OutputSlotIndex);
			}
			State->PendingPackets.RemoveAt(0);
			return false;
		}

		if (PendingPacket.OutputMapResource.mappedResource)
		{
			FAPI::Get<FNVENC>().nvEncUnmapInputResource(State->Encoder, PendingPacket.OutputMapResource.mappedResource);
			PendingPacket.OutputMapResource.mappedResource = nullptr;
		}

		if (PendingPacket.OutputSlotIndex != INDEX_NONE)
		{
			State->FreeOutputSlots.Add(PendingPacket.OutputSlotIndex);
		}

		State->PendingPackets.RemoveAt(0);
	}

	return true;
#else
	OutError = TEXT("RuntimeRec GPU MP4 muxing is currently implemented for Windows only.");
	return false;
#endif
}

bool FRuntimeRecGpuVideoEncoder::RetireStateForReuse()
{
#if PLATFORM_WINDOWS
	if (!State || !State->Encoder || State->PendingPackets.Num() > 0)
	{
		return false;
	}

	const float ReusableIdleSeconds = CVarRuntimeRecGpuVideoEncoderReusableIdleSeconds.GetValueOnAnyThread();
	const int32 MaxReusableGpuEncoders = FMath::Max(0, CVarRuntimeRecMaxReusableGpuVideoEncoders.GetValueOnAnyThread());
	if (ReusableIdleSeconds <= 0.0f || MaxReusableGpuEncoders <= 0)
	{
		return false;
	}

	UE_LOG(
		LogTemp,
		Display,
		TEXT("RuntimeRec GPU encoder retire begin [Output=%s] Encoder=%p FreeOutputSlots=%d OutputSlots=%d SinkWriter=%d MfStarted=%d"),
		*OutputPath,
		State->Encoder,
		State->FreeOutputSlots.Num(),
		State->OutputSlots.Num(),
		State->SinkWriter ? 1 : 0,
		State->bMfStarted ? 1 : 0);

	State->FreeOutputSlots.Reset();
	for (int32 SlotIndex = 0; SlotIndex < State->OutputSlots.Num(); ++SlotIndex)
	{
		State->FreeOutputSlots.Add(SlotIndex);
	}

	{
		FString FinalizeError;
		if (!FinalizeWriter(FinalizeError))
		{
			UE_LOG(LogTemp, Warning, TEXT("RuntimeRec GPU encoder retire finalize failed [Output=%s]: %s"), *OutputPath, *FinalizeError);
			return false;
		}
	}

	State->IdleSinceSeconds = FPlatformTime::Seconds();

	{
		FScopeLock Lock(&GetReusableStateCriticalSection());
		GetReusableStates().Add(MoveTemp(State));
	}

	UE_LOG(LogTemp, Display, TEXT("RuntimeRec GPU encoder retire end [Output=%s]."), *OutputPath);
	return true;
#else
	return false;
#endif
}

void FRuntimeRecGpuVideoEncoder::PruneReusableStates()
{
#if PLATFORM_WINDOWS
	TArray<TUniquePtr<FState>> StatesToDestroy;
	TArray<FString> DropLogs;
	const float ReusableIdleSeconds = GetReusableIdleSecondsCVar();
	const int32 MaxReusableGpuEncoders = FMath::Max(0, CVarRuntimeRecMaxReusableGpuVideoEncoders.GetValueOnAnyThread());
	const bool bDisableReuseRetention = ReusableIdleSeconds <= 0.0f || MaxReusableGpuEncoders <= 0;
	const double NowSeconds = FPlatformTime::Seconds();
	auto CurrentHardwareDevice = FAVDevice::GetHardwareDevice();
	const auto CurrentVideoContext = CurrentHardwareDevice->GetContext<FVideoContextD3D12>();
	const void* CurrentHardwareDeviceIdentity = &CurrentHardwareDevice.Get();
	const void* CurrentVideoContextIdentity = CurrentVideoContext.Get();
	const void* CurrentD3D12DeviceIdentity = CurrentVideoContext.IsValid() && CurrentVideoContext->Device ? CurrentVideoContext->Device.GetReference() : nullptr;
	const int32 CurrentDeviceRemovedReason = static_cast<int32>(CurrentVideoContext.IsValid() && CurrentVideoContext->Device ? CurrentVideoContext->Device->GetDeviceRemovedReason() : S_OK);

	{
		FScopeLock Lock(&GetReusableStateCriticalSection());
		TArray<TUniquePtr<FState>>& ReusableStates = GetReusableStates();

		if (ReusableStates.IsEmpty())
		{
			return;
		}

		if (bDisableReuseRetention)
		{
			for (int32 StateIndex = ReusableStates.Num() - 1; StateIndex >= 0; --StateIndex)
			{
				TUniquePtr<FState>& Candidate = ReusableStates[StateIndex];
				if (!Candidate.IsValid())
				{
					void* EncoderPtr = static_cast<void*>(nullptr);
					void* DevicePtr = static_cast<void*>(nullptr);
					DropLogs.Add(FString::Printf(
						TEXT("RuntimeRec GPU encoder reusable pool prune state [Reason=Shutdown] IdleAgeSeconds=%.3f IdleTTLSeconds=%.3f Encoder=%p Device=%p RegisteredInputs=%d RegisteredOutputs=%d"),
						-1.0,
						static_cast<double>(ReusableIdleSeconds),
						EncoderPtr,
						DevicePtr,
						0,
						0));
				}
				else
				{
					int32 RegisteredInputCount = 0;
					for (const FState::FInputSlot& InputSlot : Candidate->InputSlots)
					{
						if (InputSlot.RegisteredResource)
						{
							++RegisteredInputCount;
						}
					}

					int32 RegisteredOutputCount = 0;
					for (const FState::FOutputSlot& OutputSlot : Candidate->OutputSlots)
					{
						if (OutputSlot.RegisteredResource)
						{
							++RegisteredOutputCount;
						}
					}

					DropLogs.Add(FString::Printf(
						TEXT("RuntimeRec GPU encoder reusable pool prune state [Reason=Shutdown] IdleAgeSeconds=%.3f IdleTTLSeconds=%.3f Encoder=%p Device=%p RegisteredInputs=%d RegisteredOutputs=%d"),
						NowSeconds - Candidate->IdleSinceSeconds,
						static_cast<double>(ReusableIdleSeconds),
						Candidate->Encoder,
						Candidate->D3D12Device.GetReference(),
						RegisteredInputCount,
						RegisteredOutputCount));
				}

				StatesToDestroy.Add(MoveTemp(ReusableStates[StateIndex]));
				ReusableStates.RemoveAtSwap(StateIndex, 1, EAllowShrinking::No);
			}
		}
		else
		{
			for (int32 StateIndex = ReusableStates.Num() - 1; StateIndex >= 0; --StateIndex)
			{
				const TUniquePtr<FState>& Candidate = ReusableStates[StateIndex];
				const bool bExpired =
#if WITH_EDITOR
					false;
#else
					!Candidate.IsValid() || (NowSeconds - Candidate->IdleSinceSeconds) >= static_cast<double>(ReusableIdleSeconds);
#endif
				const bool bDeviceMismatch =
					Candidate.IsValid() &&
					(Candidate->HardwareDeviceIdentity != CurrentHardwareDeviceIdentity ||
					Candidate->VideoContextIdentity != CurrentVideoContextIdentity ||
					Candidate->D3D12DeviceIdentity != CurrentD3D12DeviceIdentity ||
					Candidate->DeviceRemovedReason != CurrentDeviceRemovedReason);
				const bool bBroken =
					Candidate.IsValid() &&
					(!Candidate->Encoder || Candidate->SinkWriter || Candidate->bMfStarted || Candidate->PendingPackets.Num() > 0);

				FString Reason;
				if (!Candidate.IsValid() || bBroken)
				{
					Reason = TEXT("Broken");
				}
				else if (bDeviceMismatch)
				{
					Reason = TEXT("DeviceMismatch");
				}
				else if (bExpired)
				{
					Reason = TEXT("Expired");
				}

				if (!Reason.IsEmpty())
				{
					int32 RegisteredInputCount = 0;
					for (const FState::FInputSlot& InputSlot : Candidate->InputSlots)
					{
						if (InputSlot.RegisteredResource)
						{
							++RegisteredInputCount;
						}
					}

					int32 RegisteredOutputCount = 0;
					for (const FState::FOutputSlot& OutputSlot : Candidate->OutputSlots)
					{
						if (OutputSlot.RegisteredResource)
						{
							++RegisteredOutputCount;
						}
					}

					DropLogs.Add(FString::Printf(
						TEXT("RuntimeRec GPU encoder reusable pool prune state [Reason=%s] IdleAgeSeconds=%.3f IdleTTLSeconds=%.3f Encoder=%p Device=%p RegisteredInputs=%d RegisteredOutputs=%d"),
						*Reason,
						Candidate.IsValid() ? (NowSeconds - Candidate->IdleSinceSeconds) : -1.0,
						static_cast<double>(ReusableIdleSeconds),
						static_cast<void*>(Candidate.IsValid() ? Candidate->Encoder : nullptr),
						static_cast<void*>(Candidate.IsValid() ? Candidate->D3D12Device.GetReference() : nullptr),
						RegisteredInputCount,
						RegisteredOutputCount));
					StatesToDestroy.Add(MoveTemp(ReusableStates[StateIndex]));
					ReusableStates.RemoveAtSwap(StateIndex, 1, EAllowShrinking::No);
				}
			}

			while (ReusableStates.Num() > MaxReusableGpuEncoders)
			{
				int32 OldestIndex = INDEX_NONE;
				double OldestIdleSinceSeconds = MAX_dbl;
				for (int32 StateIndex = 0; StateIndex < ReusableStates.Num(); ++StateIndex)
				{
					const TUniquePtr<FState>& Candidate = ReusableStates[StateIndex];
					if (!Candidate.IsValid())
					{
						OldestIndex = StateIndex;
						break;
					}

					if (Candidate->IdleSinceSeconds < OldestIdleSinceSeconds)
					{
						OldestIdleSinceSeconds = Candidate->IdleSinceSeconds;
						OldestIndex = StateIndex;
					}
				}

				if (OldestIndex == INDEX_NONE)
				{
					break;
				}

				const TUniquePtr<FState>& Candidate = ReusableStates[OldestIndex];
				int32 RegisteredInputCount = 0;
				int32 RegisteredOutputCount = 0;
				if (Candidate.IsValid())
				{
					for (const FState::FInputSlot& InputSlot : Candidate->InputSlots)
					{
						if (InputSlot.RegisteredResource)
						{
							++RegisteredInputCount;
						}
					}

					for (const FState::FOutputSlot& OutputSlot : Candidate->OutputSlots)
					{
						if (OutputSlot.RegisteredResource)
						{
							++RegisteredOutputCount;
						}
					}
				}

				DropLogs.Add(FString::Printf(
					TEXT("RuntimeRec GPU encoder reusable pool prune state [Reason=OverLimit] IdleAgeSeconds=%.3f IdleTTLSeconds=%.3f Encoder=%p Device=%p RegisteredInputs=%d RegisteredOutputs=%d"),
					Candidate.IsValid() ? (NowSeconds - Candidate->IdleSinceSeconds) : -1.0,
					static_cast<double>(ReusableIdleSeconds),
					static_cast<void*>(Candidate.IsValid() ? Candidate->Encoder : nullptr),
					static_cast<void*>(Candidate.IsValid() ? Candidate->D3D12Device.GetReference() : nullptr),
					RegisteredInputCount,
					RegisteredOutputCount));

				StatesToDestroy.Add(MoveTemp(ReusableStates[OldestIndex]));
				ReusableStates.RemoveAtSwap(OldestIndex, 1, EAllowShrinking::No);
			}
		}
	}

	if (StatesToDestroy.Num() > 0)
	{
		const int32 PrunedCount = StatesToDestroy.Num();
		UE_LOG(LogTemp, Display, TEXT("RuntimeRec GPU encoder reusable pool prune begin [Count=%d]."), PrunedCount);
		for (const FString& DropLog : DropLogs)
		{
			UE_LOG(LogTemp, Display, TEXT("%s"), *DropLog);
		}
		for (TUniquePtr<FState>& ReusableState : StatesToDestroy)
		{
			if (ReusableState.IsValid())
			{
				DestroyStateResources(*ReusableState);
			}
			ReleaseGpuEncoderSlot();
		}
		RecordNvencPruneEvent(PrunedCount);
		RecordNvencDestroyEvent(PrunedCount);
		UE_LOG(LogTemp, Display, TEXT("RuntimeRec GPU encoder reusable pool prune end."));
	}
#endif
}

void FRuntimeRecGpuVideoEncoder::ShutdownReusableStates()
{
#if PLATFORM_WINDOWS
	TArray<TUniquePtr<FState>> StatesToDestroy;
	{
		FScopeLock Lock(&GetReusableStateCriticalSection());
		StatesToDestroy = MoveTemp(GetReusableStates());
	}

	if (StatesToDestroy.Num() > 0)
	{
		const int32 ShutdownCount = StatesToDestroy.Num();
		UE_LOG(LogTemp, Display, TEXT("RuntimeRec GPU encoder reusable pool shutdown begin [Count=%d]."), ShutdownCount);
		for (TUniquePtr<FState>& ReusableState : StatesToDestroy)
		{
			if (ReusableState.IsValid())
			{
				DestroyStateResources(*ReusableState);
			}
			ReleaseGpuEncoderSlot();
		}
		RecordNvencDestroyEvent(ShutdownCount);
		UE_LOG(LogTemp, Display, TEXT("RuntimeRec GPU encoder reusable pool shutdown end."));
	}
#endif
}

bool FRuntimeRecGpuVideoEncoder::FinalizeWriter(FString& OutError)
{
#if PLATFORM_WINDOWS
	if (!State)
	{
		return true;
	}

	bool bFinalizeSucceeded = true;
	if (State->SinkWriter)
	{
		const HRESULT FinalizeResult = State->SinkWriter->Finalize();
		UE_LOG(
			LogTemp,
			Display,
			TEXT("RuntimeRec GPU encoder sink writer finalize returned %s [Output=%s]."),
			*HResultToString(FinalizeResult),
			*OutputPath);

		if (!CheckHr(FinalizeResult, TEXT("IMFSinkWriter::Finalize"), OutError))
		{
			UE_LOG(LogTemp, Warning, TEXT("RuntimeRec GPU encoder sink writer finalize failed [Output=%s]: %s"), *OutputPath, *OutError);
			bFinalizeSucceeded = false;
		}
	}

	if (State->SinkWriter)
	{
		State->SinkWriter->Release();
		State->SinkWriter = nullptr;
	}

	if (State->bMfStarted)
	{
		MFShutdown();
		State->bMfStarted = false;
	}

	return bFinalizeSucceeded;
#else
	return true;
#endif
}

bool FRuntimeRecGpuVideoEncoder::SendEndOfStream(FString& OutError)
{
#if PLATFORM_WINDOWS
	if (!State || !State->Encoder)
	{
		return true;
	}

	NV_ENC_STRUCT(NV_ENC_PIC_PARAMS, EosPicture);
	EosPicture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
	EosPicture.encodePicFlags = NV_ENC_PIC_FLAG_EOS;

	const NVENCSTATUS Result = FAPI::Get<FNVENC>().nvEncEncodePicture(State->Encoder, &EosPicture);
	LogNvEncCallResult(TEXT("RuntimeRec GPU encoder EOS encode"), State->Encoder, Result);
	return CheckNvEnc(Result, State->Encoder, TEXT("nvEncEncodePicture EOS"), OutError);
#else
	return true;
#endif
}

void FRuntimeRecGpuVideoEncoder::DestroyActiveState(FString& OutError)
{
#if PLATFORM_WINDOWS
	(void)OutError;
	if (!State)
	{
		return;
	}

	if (State->SinkWriter || State->bMfStarted)
	{
		FString FinalizeError;
		FinalizeWriter(FinalizeError);
		if (!FinalizeError.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("RuntimeRec GPU encoder writer finalize during destroy reported [Output=%s]: %s"), *OutputPath, *FinalizeError);
		}
	}

	DestroyStateResources(*State);
	RecordNvencDestroyEvent(1);
#else
	(void)OutError;
#endif
}
