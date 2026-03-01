using UnrealBuildTool;

public class OlivaBlueprintRAG : ModuleRules
{
    public OlivaBlueprintRAG(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "AssetRegistry",
            "BlueprintGraph",
            "Kismet",
            "KismetCompiler",
            "UnrealEd",
            "Json",
            "JsonUtilities"
        });
    }
}
