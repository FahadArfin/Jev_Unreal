using UnrealBuildTool;

public class JevSandbox : ModuleRules
{
    public JevSandbox(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "FunctionalTesting", "NavigationSystem" });
    }
}
