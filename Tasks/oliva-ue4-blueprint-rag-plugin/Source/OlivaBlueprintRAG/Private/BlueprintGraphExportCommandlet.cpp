#include "BlueprintGraphExportCommandlet.h"

#include "AssetRegistry/AssetData.h"
#include "BlueprintGraphRAGService.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

UBlueprintGraphExportCommandlet::UBlueprintGraphExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;

    HelpDescription = TEXT("Exports Blueprint graph nodes/connections to JSON for RAG indexing.");
    HelpUsage = TEXT("UE4Editor-Cmd.exe <Project>.uproject -run=BlueprintGraphExport "
                     "-Root=/Game -Prefix=BP_ -ExportDir=<Saved/BlueprintExports>");
}

int32 UBlueprintGraphExportCommandlet::Main(const FString& Params)
{
    FString RootPath = TEXT("/Game");
    FString Prefix = TEXT("BP_");
    FString ExportDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BlueprintExports"));
    bool bRecursivePaths = true;

    FParse::Value(*Params, TEXT("Root="), RootPath);
    FParse::Value(*Params, TEXT("Prefix="), Prefix);
    FParse::Value(*Params, TEXT("ExportDir="), ExportDir);
    if (FParse::Param(*Params, TEXT("NoRecursive")))
    {
        bRecursivePaths = false;
    }

    IFileManager::Get().MakeDirectory(*ExportDir, true);

    TArray<FAssetData> BlueprintAssets;
    if (!FBlueprintGraphRAGService::FindBlueprintAssets(RootPath, Prefix, bRecursivePaths, BlueprintAssets))
    {
        UE_LOG(LogTemp, Error, TEXT("BlueprintGraphExport: failed to query blueprint assets."));
        return 1;
    }

    UE_LOG(LogTemp, Display, TEXT("BlueprintGraphExport: discovered %d assets."), BlueprintAssets.Num());

    int32 ExportedCount = 0;
    int32 FailedCount = 0;

    for (const FAssetData& AssetData : BlueprintAssets)
    {
        UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
        if (!Blueprint)
        {
            ++FailedCount;
            UE_LOG(LogTemp, Warning, TEXT("BlueprintGraphExport: failed to load %s"), *AssetData.GetObjectPathString());
            continue;
        }

        FString FileName = Blueprint->GetPathName();
        FileName.ReplaceInline(TEXT("/"), TEXT("_"));
        FileName.ReplaceInline(TEXT("."), TEXT("_"));
        FileName += TEXT(".bpl_json");

        const FString OutputPath = FPaths::Combine(ExportDir, FileName);
        FString Error;
        if (!FBlueprintGraphRAGService::ExportBlueprintToFile(Blueprint, OutputPath, Error))
        {
            ++FailedCount;
            UE_LOG(LogTemp, Warning, TEXT("BlueprintGraphExport: %s"), *Error);
            continue;
        }

        ++ExportedCount;
        UE_LOG(LogTemp, Display, TEXT("BlueprintGraphExport: %s"), *OutputPath);
    }

    UE_LOG(LogTemp, Display, TEXT("BlueprintGraphExport: exported=%d failed=%d total=%d"),
           ExportedCount, FailedCount, BlueprintAssets.Num());

    if (ExportedCount == 0 && BlueprintAssets.Num() > 0)
    {
        UE_LOG(LogTemp, Error, TEXT("BlueprintGraphExport: all assets failed to export."));
        return 1;
    }

    if (FailedCount > 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("BlueprintGraphExport: %d assets skipped due to errors (OK)."), FailedCount);
    }

    return 0;
}
