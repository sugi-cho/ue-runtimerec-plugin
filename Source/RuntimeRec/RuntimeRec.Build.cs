using UnrealBuildTool;

public class RuntimeRec : ModuleRules
{
	public RuntimeRec(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"ApplicationCore",
				"AVCodecsCore",
				"AVCodecsCoreRHI",
				"D3D12RHI",
				"NVCodecs",
				"NVCodecsRHI",
				"NVENC",
				"Projects",
				"RenderCore",
				"RHI"
			}
		);

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			AddEngineThirdPartyPrivateStaticDependencies(Target, "DX12");

			PublicSystemLibraries.AddRange(
				new string[]
				{
					"mf.lib",
					"mfplat.lib",
					"mfreadwrite.lib",
					"mfuuid.lib",
					"propsys.lib",
					"shlwapi.lib",
					"strmiids.lib",
					"wmcodecdspuuid.lib"
				}
			);
		}
	}
}
