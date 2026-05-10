#include "RuntimeRecGpuVideoEncoder.h"

#include "Containers/Ticker.h"
#include "Modules/ModuleManager.h"

class FRuntimeRecModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FRuntimeRecModule::Tick),
			1.0f);
	}

	virtual void ShutdownModule() override
	{
		if (TickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
			TickerHandle.Reset();
		}

		FRuntimeRecGpuVideoEncoder::ShutdownReusableStates();
	}

private:
	bool Tick(float DeltaTime)
	{
		FRuntimeRecGpuVideoEncoder::PruneReusableStates();
		return true;
	}

private:
	FTSTicker::FDelegateHandle TickerHandle;
};

IMPLEMENT_MODULE(FRuntimeRecModule, RuntimeRec)
