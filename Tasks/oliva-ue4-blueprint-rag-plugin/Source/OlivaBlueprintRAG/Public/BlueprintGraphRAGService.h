#pragma once

#include "CoreMinimal.h"

class UBlueprint;
struct FAssetData;

struct FBlueprintRAGImportOptions
{
    bool bCompileAfterImport = true;
    bool bMarkDirty = true;
    bool bCreateMissingGraphs = true;
    bool bClearGraphBeforeImport = false;
    bool bImportVariables = true;
};

class OLIVABLUEPRINTRAG_API FBlueprintGraphRAGService final
{
public:
    static bool FindBlueprintAssets(
        const FString& RootPackagePath,
        const FString& NamePrefix,
        bool bRecursivePaths,
        TArray<FAssetData>& OutAssets);

    static bool ExportBlueprintToJson(UBlueprint* Blueprint, FString& OutJson, FString& OutError);
    static bool ExportBlueprintToFile(UBlueprint* Blueprint, const FString& FilePath, FString& OutError);

    static bool ImportBlueprintFromJson(
        UBlueprint* Blueprint,
        const FString& JsonText,
        const FBlueprintRAGImportOptions& Options,
        FString& OutError);

    static bool ImportBlueprintFromFile(
        UBlueprint* Blueprint,
        const FString& FilePath,
        const FBlueprintRAGImportOptions& Options,
        FString& OutError);

    static bool SaveBlueprintPackage(UBlueprint* Blueprint, FString& OutError);

    static bool LoadBlueprintPathFromJsonFile(const FString& FilePath, FString& OutBlueprintPath);
};
