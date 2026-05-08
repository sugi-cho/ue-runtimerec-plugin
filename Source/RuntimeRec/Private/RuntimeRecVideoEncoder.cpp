#include "RuntimeRecVideoEncoder.h"

#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
THIRD_PARTY_INCLUDES_END
#include "Windows/HideWindowsPlatformTypes.h"

class FRuntimeRecVideoEncoder::FWindowsMediaFoundationState
{
public:
	IMFSinkWriter* SinkWriter = nullptr;
	DWORD StreamIndex = 0;
};

namespace RuntimeRecVideoEncoderWindows
{
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
}
#endif

FRuntimeRecVideoEncoder::FRuntimeRecVideoEncoder()
{
	QueueEvent = FPlatformProcess::GetSynchEventFromPool(false);
}

FRuntimeRecVideoEncoder::~FRuntimeRecVideoEncoder()
{
	FString IgnoredError;
	Stop(IgnoredError);

	if (QueueEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(QueueEvent);
		QueueEvent = nullptr;
	}
}

bool FRuntimeRecVideoEncoder::Start(
	const FString& InOutputPath,
	int32 InWidth,
	int32 InHeight,
	int32 InFPS,
	int32 InBitrateKbps,
	bool bInPreferHardwareEncoder,
	bool bInAllowFrameDrop,
	FString& OutError)
{
	if (bStarted)
	{
		OutError = TEXT("Encoder is already started.");
		return false;
	}

	if (InWidth <= 0 || InHeight <= 0 || InFPS <= 0 || InBitrateKbps <= 0)
	{
		OutError = TEXT("Invalid encoder settings.");
		return false;
	}

	OutputPath = InOutputPath;
	Width = InWidth;
	Height = InHeight;
	FPS = InFPS;
	BitrateKbps = InBitrateKbps;
	bPreferHardwareEncoder = bInPreferHardwareEncoder;
	bAllowFrameDrop = bInAllowFrameDrop;
	bStopping = false;
	NextFrameIndex = 0;

	if (!InitializeWriter(OutError))
	{
		ShutdownWriter();
		return false;
	}

	bStarted = true;
	WorkerFuture = Async(EAsyncExecution::Thread, [this]()
	{
		ThreadMain();
	});

	return true;
}

bool FRuntimeRecVideoEncoder::EnqueueFrame(TArray<FColor>&& FramePixels, FString& OutError)
{
	if (!bStarted || bStopping)
	{
		OutError = TEXT("Encoder is not accepting frames.");
		return false;
	}

	const int32 ExpectedPixelCount = Width * Height;
	if (FramePixels.Num() != ExpectedPixelCount)
	{
		OutError = FString::Printf(TEXT("Frame size mismatch. Expected %d pixels but got %d."), ExpectedPixelCount, FramePixels.Num());
		return false;
	}

	{
		FScopeLock Lock(&QueueCriticalSection);

		// Keep latency bounded. Runtime recording should drop stale frames rather than stall gameplay.
		constexpr int32 MaxQueuedFrames = 8;
		if (PendingFrames.Num() >= MaxQueuedFrames)
		{
			if (!bAllowFrameDrop)
			{
				OutError = TEXT("Encoder frame queue is full.");
				return false;
			}

			PendingFrames.RemoveAt(0);
		}

		FFrame Frame;
		Frame.Pixels = MoveTemp(FramePixels);
		Frame.Index = NextFrameIndex++;
		PendingFrames.Add(MoveTemp(Frame));
	}

	QueueEvent->Trigger();
	return true;
}

bool FRuntimeRecVideoEncoder::Stop(FString& OutError)
{
	if (!bStarted)
	{
		return true;
	}

	bStopping = true;
	if (QueueEvent)
	{
		QueueEvent->Trigger();
	}

	if (WorkerFuture.IsValid())
	{
		WorkerFuture.Wait();
	}

	bStarted = false;
	ShutdownWriter();
	return true;
}

void FRuntimeRecVideoEncoder::ThreadMain()
{
	while (true)
	{
		FFrame Frame;
		bool bHasFrame = false;

		{
			FScopeLock Lock(&QueueCriticalSection);
			if (PendingFrames.Num() > 0)
			{
				Frame = MoveTemp(PendingFrames[0]);
				PendingFrames.RemoveAt(0);
				bHasFrame = true;
			}
		}

		if (bHasFrame)
		{
			FString IgnoredError;
			WriteFrame(Frame, IgnoredError);
			continue;
		}

		if (bStopping)
		{
			break;
		}

		QueueEvent->Wait(10);
	}
}

bool FRuntimeRecVideoEncoder::InitializeWriter(FString& OutError)
{
#if PLATFORM_WINDOWS
	State = MakeUnique<FWindowsMediaFoundationState>();

	HRESULT Hr = MFStartup(MF_VERSION);
	if (!RuntimeRecVideoEncoderWindows::CheckHr(Hr, TEXT("MFStartup"), OutError))
	{
		return false;
	}

	TRefCountPtr<IMFAttributes> Attributes;
	Hr = MFCreateAttributes(Attributes.GetInitReference(), 2);
	if (!RuntimeRecVideoEncoderWindows::CheckHr(Hr, TEXT("MFCreateAttributes"), OutError))
	{
		return false;
	}

	Attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, bPreferHardwareEncoder ? 1u : 0u);
	Attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, 1u);

	Hr = MFCreateSinkWriterFromURL(*OutputPath, nullptr, Attributes, &State->SinkWriter);
	if (!RuntimeRecVideoEncoderWindows::CheckHr(Hr, TEXT("MFCreateSinkWriterFromURL"), OutError))
	{
		return false;
	}

	const uint32 VideoBitrate = static_cast<uint32>(BitrateKbps * 1000);

	TRefCountPtr<IMFMediaType> OutputType;
	Hr = MFCreateMediaType(OutputType.GetInitReference());
	if (!RuntimeRecVideoEncoderWindows::CheckHr(Hr, TEXT("MFCreateMediaType output"), OutError))
	{
		return false;
	}

	OutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	OutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
	OutputType->SetUINT32(MF_MT_AVG_BITRATE, VideoBitrate);
	OutputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	MFSetAttributeSize(OutputType, MF_MT_FRAME_SIZE, Width, Height);
	MFSetAttributeRatio(OutputType, MF_MT_FRAME_RATE, FPS, 1);
	MFSetAttributeRatio(OutputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

	Hr = State->SinkWriter->AddStream(OutputType, &State->StreamIndex);
	if (!RuntimeRecVideoEncoderWindows::CheckHr(Hr, TEXT("IMFSinkWriter::AddStream"), OutError))
	{
		return false;
	}

	TRefCountPtr<IMFMediaType> InputType;
	Hr = MFCreateMediaType(InputType.GetInitReference());
	if (!RuntimeRecVideoEncoderWindows::CheckHr(Hr, TEXT("MFCreateMediaType input"), OutError))
	{
		return false;
	}

	InputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	InputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
	InputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	MFSetAttributeSize(InputType, MF_MT_FRAME_SIZE, Width, Height);
	MFSetAttributeRatio(InputType, MF_MT_FRAME_RATE, FPS, 1);
	MFSetAttributeRatio(InputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

	Hr = State->SinkWriter->SetInputMediaType(State->StreamIndex, InputType, nullptr);
	if (!RuntimeRecVideoEncoderWindows::CheckHr(Hr, TEXT("IMFSinkWriter::SetInputMediaType"), OutError))
	{
		return false;
	}

	Hr = State->SinkWriter->BeginWriting();
	if (!RuntimeRecVideoEncoderWindows::CheckHr(Hr, TEXT("IMFSinkWriter::BeginWriting"), OutError))
	{
		return false;
	}

	return true;
#else
	OutError = TEXT("RuntimeRec MP4 encoding is currently implemented for Windows only.");
	return false;
#endif
}

bool FRuntimeRecVideoEncoder::WriteFrame(const FFrame& Frame, FString& OutError)
{
#if PLATFORM_WINDOWS
	if (!State || !State->SinkWriter)
	{
		OutError = TEXT("Media Foundation sink writer is not initialized.");
		return false;
	}

	const DWORD BufferSize = static_cast<DWORD>(Width * Height * sizeof(FColor));
	TRefCountPtr<IMFMediaBuffer> Buffer;
	HRESULT Hr = MFCreateMemoryBuffer(BufferSize, Buffer.GetInitReference());
	if (!RuntimeRecVideoEncoderWindows::CheckHr(Hr, TEXT("MFCreateMemoryBuffer"), OutError))
	{
		return false;
	}

	BYTE* Data = nullptr;
	DWORD MaxLength = 0;
	DWORD CurrentLength = 0;
	Hr = Buffer->Lock(&Data, &MaxLength, &CurrentLength);
	if (!RuntimeRecVideoEncoderWindows::CheckHr(Hr, TEXT("IMFMediaBuffer::Lock"), OutError))
	{
		return false;
	}

	FMemory::Memcpy(Data, Frame.Pixels.GetData(), BufferSize);
	for (int32 PixelIndex = 0; PixelIndex < Width * Height; ++PixelIndex)
	{
		Data[PixelIndex * 4 + 3] = 0xFF;
	}
	Buffer->Unlock();
	Buffer->SetCurrentLength(BufferSize);

	TRefCountPtr<IMFSample> Sample;
	Hr = MFCreateSample(Sample.GetInitReference());
	if (!RuntimeRecVideoEncoderWindows::CheckHr(Hr, TEXT("MFCreateSample"), OutError))
	{
		return false;
	}

	Hr = Sample->AddBuffer(Buffer);
	if (!RuntimeRecVideoEncoderWindows::CheckHr(Hr, TEXT("IMFSample::AddBuffer"), OutError))
	{
		return false;
	}

	const LONGLONG FrameDuration = 10'000'000LL / FPS;
	Sample->SetSampleTime(Frame.Index * FrameDuration);
	Sample->SetSampleDuration(FrameDuration);

	Hr = State->SinkWriter->WriteSample(State->StreamIndex, Sample);
	return RuntimeRecVideoEncoderWindows::CheckHr(Hr, TEXT("IMFSinkWriter::WriteSample"), OutError);
#else
	OutError = TEXT("RuntimeRec MP4 encoding is currently implemented for Windows only.");
	return false;
#endif
}

void FRuntimeRecVideoEncoder::ShutdownWriter()
{
#if PLATFORM_WINDOWS
	if (State && State->SinkWriter)
	{
		State->SinkWriter->Finalize();
		State->SinkWriter->Release();
		State->SinkWriter = nullptr;
	}

	State.Reset();
	MFShutdown();
#endif
}
