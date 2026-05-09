#include "RuntimeRecGpuVideoEncoder.h"

#include "AVDevice.h"
#include "DynamicRHI.h"
#include "HAL/IConsoleManager.h"
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

	FCriticalSection GpuEncoderSlotCriticalSection;
	int32 ActiveGpuEncoderSlots = 0;
	constexpr int32 RuntimeRecGpuEncoderInputBufferCount = 3;

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

	TRefCountPtr<ID3D12Device> D3D12Device;
	TRefCountPtr<ID3D12Fence> InputFence;
	TRefCountPtr<ID3D12Fence> OutputFence;
	TRefCountPtr<ID3D12Resource> OutputBitstreamResource;
	TStaticArray<FInputSlot, RuntimeRecGpuEncoderInputBufferCount> InputSlots;
	int64 InputFenceValue = 0;
	int64 OutputFenceValue = 0;

	void* Encoder = nullptr;
	NV_ENC_CONFIG EncodeConfig = {};
	NV_ENC_INITIALIZE_PARAMS InitializeParams = {};
	NV_ENC_REGISTERED_PTR RegisteredOutputResource = nullptr;

	IMFSinkWriter* SinkWriter = nullptr;
	DWORD StreamIndex = 0;
	bool bMfStarted = false;
#endif
};

FRuntimeRecGpuVideoEncoder::FRuntimeRecGpuVideoEncoder()
{
}

FRuntimeRecGpuVideoEncoder::~FRuntimeRecGpuVideoEncoder()
{
	FString IgnoredError;
	Stop(IgnoredError);
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
	if (bStarted)
	{
		OutError = TEXT("GPU encoder is already started.");
		return false;
	}

	if (InWidth <= 0 || InHeight <= 0 || InFPS <= 0 || InBitrateKbps <= 0)
	{
		OutError = TEXT("Invalid GPU encoder settings.");
		return false;
	}

	FString UnavailableReason;
	if (!IsAvailable(UnavailableReason))
	{
		OutError = UnavailableReason;
		return false;
	}

	if (!TryReserveGpuEncoderSlot(UnavailableReason))
	{
		OutError = UnavailableReason;
		return false;
	}
	bReservedGpuEncoderSlot = true;

	OutputPath = InOutputPath;
	Width = InWidth;
	Height = InHeight;
	FPS = InFPS;
	BitrateKbps = InBitrateKbps;
	bStopping = false;

	State = MakeUnique<FState>();

#if PLATFORM_WINDOWS
	State->D3D12Device = FAVDevice::GetHardwareDevice()->GetContext<FVideoContextD3D12>()->Device;
	if (!State->D3D12Device.IsValid())
	{
		OutError = TEXT("D3D12 device is not available for Direct NVENC.");
		ShutdownWriter();
		State.Reset();
		if (bReservedGpuEncoderSlot)
		{
			ReleaseGpuEncoderSlot();
			bReservedGpuEncoderSlot = false;
		}
		return false;
	}

	NV_ENC_STRUCT(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS, SessionParams);
	SessionParams.apiVersion = NVENCAPI_VERSION;
	SessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
	SessionParams.device = State->D3D12Device.GetReference();

	if (!CheckNvEnc(FAPI::Get<FNVENC>().nvEncOpenEncodeSessionEx(&SessionParams, &State->Encoder), State->Encoder, TEXT("nvEncOpenEncodeSessionEx"), OutError))
	{
		ShutdownWriter();
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
		ShutdownWriter();
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
		ShutdownWriter();
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
		ShutdownWriter();
		State.Reset();
		if (bReservedGpuEncoderSlot)
		{
			ReleaseGpuEncoderSlot();
			bReservedGpuEncoderSlot = false;
		}
		return false;
	}

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

	if (!CheckHr(State->D3D12Device->CreateCommittedResource(
		&HeapProps,
		D3D12_HEAP_FLAG_NONE,
		&ResourceDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(State->OutputBitstreamResource.GetInitReference())),
		TEXT("ID3D12Device::CreateCommittedResource output bitstream"),
		OutError))
	{
		ShutdownWriter();
		State.Reset();
		if (bReservedGpuEncoderSlot)
		{
			ReleaseGpuEncoderSlot();
			bReservedGpuEncoderSlot = false;
		}
		return false;
	}
#endif

	if (!InitializeWriter(OutError))
	{
		ShutdownWriter();
		State.Reset();
		if (bReservedGpuEncoderSlot)
		{
			ReleaseGpuEncoderSlot();
			bReservedGpuEncoderSlot = false;
		}
		return false;
	}

	bStarted = true;
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

	if (!State->RegisteredOutputResource)
	{
		NV_ENC_STRUCT(NV_ENC_REGISTER_RESOURCE, OutputRegisterResource);
		OutputRegisterResource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
		OutputRegisterResource.resourceToRegister = State->OutputBitstreamResource.GetReference();
		OutputRegisterResource.width = Align(static_cast<uint32>(Width * Height * 8), 4u);
		OutputRegisterResource.height = 1;
		OutputRegisterResource.pitch = 0;
		OutputRegisterResource.bufferFormat = NV_ENC_BUFFER_FORMAT_U8;
		OutputRegisterResource.bufferUsage = NV_ENC_OUTPUT_BITSTREAM;

		if (!CheckNvEnc(FAPI::Get<FNVENC>().nvEncRegisterResource(State->Encoder, &OutputRegisterResource), State->Encoder, TEXT("nvEncRegisterResource output"), OutError))
		{
			return false;
		}

		State->RegisteredOutputResource = OutputRegisterResource.registeredResource;
	}

	NV_ENC_STRUCT(NV_ENC_MAP_INPUT_RESOURCE, InputMapResource);
	InputMapResource.registeredResource = InputSlot.RegisteredResource;
	if (!CheckNvEnc(FAPI::Get<FNVENC>().nvEncMapInputResource(State->Encoder, &InputMapResource), State->Encoder, TEXT("nvEncMapInputResource input"), OutError))
	{
		return false;
	}

	NV_ENC_STRUCT(NV_ENC_MAP_INPUT_RESOURCE, OutputMapResource);
	OutputMapResource.registeredResource = State->RegisteredOutputResource;
	if (!CheckNvEnc(FAPI::Get<FNVENC>().nvEncMapInputResource(State->Encoder, &OutputMapResource), State->Encoder, TEXT("nvEncMapInputResource output"), OutError))
	{
		FAPI::Get<FNVENC>().nvEncUnmapInputResource(State->Encoder, InputMapResource.mappedResource);
		return false;
	}

	ON_SCOPE_EXIT
	{
		if (OutputMapResource.mappedResource)
		{
			FAPI::Get<FNVENC>().nvEncUnmapInputResource(State->Encoder, OutputMapResource.mappedResource);
		}
		if (InputMapResource.mappedResource)
		{
			FAPI::Get<FNVENC>().nvEncUnmapInputResource(State->Encoder, InputMapResource.mappedResource);
		}
	};

	NV_ENC_INPUT_RESOURCE_D3D12 InputResource = {};
	InputResource.inputFencePoint.bWait = true;
	InputResource.inputFencePoint.pFence = State->InputFence.GetReference();
	InputResource.inputFencePoint.waitValue = InputFenceValue;
	InputResource.pInputBuffer = InputMapResource.mappedResource;

	++State->OutputFenceValue;
	NV_ENC_OUTPUT_RESOURCE_D3D12 OutputResource = {};
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
	if (FrameIndex == 0)
	{
		Picture.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
	}

	if (!CheckNvEnc(FAPI::Get<FNVENC>().nvEncEncodePicture(State->Encoder, &Picture), State->Encoder, TEXT("nvEncEncodePicture"), OutError))
	{
		return false;
	}

	NV_ENC_STRUCT(NV_ENC_LOCK_BITSTREAM, BitstreamLock);
	BitstreamLock.outputBitstream = &OutputResource;
	if (!CheckNvEnc(FAPI::Get<FNVENC>().nvEncLockBitstream(State->Encoder, &BitstreamLock), State->Encoder, TEXT("nvEncLockBitstream"), OutError))
	{
		return false;
	}

	const bool bIsKeyframe = (BitstreamLock.pictureType & NV_ENC_PIC_TYPE_IDR) != 0;
	const bool bWrotePacket = WritePacket(
		static_cast<const uint8*>(BitstreamLock.bitstreamBufferPtr),
		BitstreamLock.bitstreamSizeInBytes,
		BitstreamLock.outputTimeStamp,
		bIsKeyframe,
		OutError);

	FString UnlockError;
	CheckNvEnc(FAPI::Get<FNVENC>().nvEncUnlockBitstream(State->Encoder, &OutputResource), State->Encoder, TEXT("nvEncUnlockBitstream"), UnlockError);
	if (!bWrotePacket)
	{
		return false;
	}

	if (!UnlockError.IsEmpty())
	{
		OutError = UnlockError;
		return false;
	}

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
		if (!DrainPackets(OutError))
		{
			ShutdownWriter();
			bStarted = false;
			if (bReservedGpuEncoderSlot)
			{
				ReleaseGpuEncoderSlot();
				bReservedGpuEncoderSlot = false;
			}
			return false;
		}
	}

	bStarted = false;
	ShutdownWriter();
	if (bReservedGpuEncoderSlot)
	{
		ReleaseGpuEncoderSlot();
		bReservedGpuEncoderSlot = false;
	}
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

bool FRuntimeRecGpuVideoEncoder::DrainPackets(FString& OutError)
{
	return true;
}

void FRuntimeRecGpuVideoEncoder::ShutdownWriter()
{
#if PLATFORM_WINDOWS
	if (State)
	{
		if (State->Encoder)
		{
			for (FState::FInputSlot& InputSlot : State->InputSlots)
			{
				if (InputSlot.RegisteredResource)
				{
					FAPI::Get<FNVENC>().nvEncUnregisterResource(State->Encoder, InputSlot.RegisteredResource);
					InputSlot.RegisteredResource = nullptr;
				}
			}

			if (State->RegisteredOutputResource)
			{
				FAPI::Get<FNVENC>().nvEncUnregisterResource(State->Encoder, State->RegisteredOutputResource);
				State->RegisteredOutputResource = nullptr;
			}

			FAPI::Get<FNVENC>().nvEncDestroyEncoder(State->Encoder);
			State->Encoder = nullptr;
		}
		State->StagingResource.Reset();
		for (FState::FInputSlot& InputSlot : State->InputSlots)
		{
			InputSlot.TextureRHI.SafeRelease();
			InputSlot.TextureResource.SafeRelease();
		}

		if (State->SinkWriter)
		{
			State->SinkWriter->Finalize();
			State->SinkWriter->Release();
			State->SinkWriter = nullptr;
		}
	}

	if (State && State->bMfStarted)
	{
		MFShutdown();
		State->bMfStarted = false;
	}
#else
#endif
}
