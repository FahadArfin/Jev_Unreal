using UnrealBuildTool;

public class JevEditor : ModuleRules
{
    public JevEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "UnrealEd", "Json", "HTTPServer",
            "AssetRegistry", "Sockets", "PhysicsCore", "RenderCore", "RHI",
            "Slate", "SlateCore", "InputCore", "ToolMenus", "DataValidation", "BlueprintGraph", "KismetCompiler", "FunctionalTesting"
        });
    }
}
