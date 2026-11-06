using System.IO;
using UnrealBuildTool;

public class WindsOfTeinora : ModuleRules
{
	public WindsOfTeinora(ReadOnlyTargetRules Target) : base(Target)
	{
		// Unreal's shared headers still forward-declare PhysX types the old way, which
		// breaks the real ones in SharedPhysics.cpp, and that file sets macros aside too.
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;
		CppStandard = CppStandardVersion.Cpp20;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "ProceduralMeshComponent", "RHI", "Slate", "SlateCore" });

		// The world and its physics are the standalone game's own code, compiled here as it is.
		string Root = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", ".."));
		PrivateIncludePaths.Add(Path.Combine(Root, "Engine"));
		PrivateIncludePaths.Add(Path.Combine(Root, "Game"));

		string PhysX = Path.Combine(Root, ".local", "physx-sdk", "dynamic");
		PublicSystemIncludePaths.Add(Path.Combine(PhysX, "include"));
		PrivateDefinitions.Add("PX_PHYSX_STATIC_LIB");
		PrivateDefinitions.Add("DISABLE_CUDA_PHYSX");
		foreach (string Library in new string[] { "PhysX", "PhysXCommon", "PhysXFoundation", "PhysXExtensions", "PhysXCooking", "PhysXPvdSDK" })
		{
			PublicAdditionalLibraries.Add(Path.Combine(PhysX, "lib", Library + "_static_64.lib"));
		}
	}
}
