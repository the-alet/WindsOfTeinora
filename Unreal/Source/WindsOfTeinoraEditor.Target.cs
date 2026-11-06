using UnrealBuildTool;

public class WindsOfTeinoraEditorTarget : TargetRules
{
	public WindsOfTeinoraEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("WindsOfTeinora");
	}
}
