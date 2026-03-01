#include "OlivaBlueprintRAGLibrary.h"

#include "AssetRegistry/AssetData.h"
#include "BlueprintGraphRAGService.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

bool UOlivaBlueprintRAGLibrary::ExportAllBlueprintsToJson(
    const FString& RootPackagePath,
    const FString& NamePrefix,
    const FString& OutputDirectory,
    const bool bRecursivePaths,
    int32& OutTotal,
    int32& OutExported,
    TArray<FString>& OutErrors)
{
    OutTotal = 0;
    OutExported = 0;
    OutErrors.Reset();

    FString EffectiveOutputDir = OutputDirectory;
    if (EffectiveOutputDir.IsEmpty())
    {
        EffectiveOutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BlueprintExports"));
    }
    IFileManager::Get().MakeDirectory(*EffectiveOutputDir, true);

    TArray<FAssetData> Assets;
    if (!FBlueprintGraphRAGService::FindBlueprintAssets(RootPackagePath, NamePrefix, bRecursivePaths, Assets))
    {
        OutErrors.Add(TEXT("FindBlueprintAssets failed."));
        return false;
    }

    OutTotal = Assets.Num();

    for (const FAssetData& AssetData : Assets)
    {
        UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
        if (!Blueprint)
        {
            OutErrors.Add(FString::Printf(TEXT("Failed to load asset: %s"), *AssetData.GetObjectPathString()));
            continue;
        }

        FString FileName = Blueprint->GetPathName();
        FileName.ReplaceInline(TEXT("/"), TEXT("_"));
        FileName.ReplaceInline(TEXT("."), TEXT("_"));
        FileName += TEXT(".json");

        const FString FilePath = FPaths::Combine(EffectiveOutputDir, FileName);

        FString Error;
        if (!FBlueprintGraphRAGService::ExportBlueprintToFile(Blueprint, FilePath, Error))
        {
            OutErrors.Add(Error);
            continue;
        }

        ++OutExported;
    }

    return OutErrors.Num() == 0;
}

bool UOlivaBlueprintRAGLibrary::ImportBlueprintsFromJsonDirectory(
    const FString& JsonDirectory,
    const bool bCompileAfterImport,
    const bool bSavePackages,
    const bool bClearGraphBeforeImport,
    int32& OutTotal,
    int32& OutImported,
    TArray<FString>& OutErrors)
{
    OutTotal = 0;
    OutImported = 0;
    OutErrors.Reset();

    FString EffectiveDir = JsonDirectory;
    if (EffectiveDir.IsEmpty())
    {
        EffectiveDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BlueprintExports"));
    }

    TArray<FString> JsonFiles;
    IFileManager::Get().FindFilesRecursive(JsonFiles, *EffectiveDir, TEXT("*.json"), true, false);
    JsonFiles.Sort();

    OutTotal = JsonFiles.Num();

    FBlueprintRAGImportOptions Options;
    Options.bCompileAfterImport = bCompileAfterImport;
    Options.bCreateMissingGraphs = true;
    Options.bClearGraphBeforeImport = bClearGraphBeforeImport;
    Options.bMarkDirty = true;

    for (const FString& FilePath : JsonFiles)
    {
        FString BlueprintPath;
        if (!FBlueprintGraphRAGService::LoadBlueprintPathFromJsonFile(FilePath, BlueprintPath))
        {
            OutErrors.Add(FString::Printf(TEXT("No 'blueprint' field in %s"), *FilePath));
            continue;
        }

        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
        if (!Blueprint)
        {
            OutErrors.Add(FString::Printf(TEXT("Failed to load blueprint: %s"), *BlueprintPath));
            continue;
        }

        FString Error;
        if (!FBlueprintGraphRAGService::ImportBlueprintFromFile(Blueprint, FilePath, Options, Error))
        {
            OutErrors.Add(Error);
            continue;
        }

        if (bSavePackages)
        {
            FString SaveError;
            if (!FBlueprintGraphRAGService::SaveBlueprintPackage(Blueprint, SaveError))
            {
                OutErrors.Add(SaveError);
                continue;
            }
        }

        ++OutImported;
    }

    return OutErrors.Num() == 0;
}
