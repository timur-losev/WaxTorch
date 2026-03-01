#include "BlueprintGraphImportCommandlet.h"

#include "BlueprintGraphRAGService.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
static bool ParseBoolFlag(const FString& Params, const TCHAR* Key, const bool DefaultValue)
{
    FString ValueText;
    if (!FParse::Value(*Params, Key, ValueText))
    {
        return DefaultValue;
    }
    return ValueText.ToBool();
}
} // namespace

UBlueprintGraphImportCommandlet::UBlueprintGraphImportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;

    HelpDescription = TEXT("Imports Blueprint graph edits from JSON and rebuilds links using UEdGraph APIs.");
    HelpUsage = TEXT("UE4Editor-Cmd.exe <Project>.uproject -run=BlueprintGraphImport "
                     "-ImportDir=<Saved/BlueprintExports> -Compile=1 -Save=1");
}

int32 UBlueprintGraphImportCommandlet::Main(const FString& Params)
{
    FString ImportDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BlueprintExports"));
    FParse::Value(*Params, TEXT("ImportDir="), ImportDir);

    const bool bCompile = ParseBoolFlag(Params, TEXT("Compile="), true);
    const bool bSave = ParseBoolFlag(Params, TEXT("Save="), true);
    const bool bCreateMissingGraphs = ParseBoolFlag(Params, TEXT("CreateMissingGraphs="), true);
    const bool bClearGraph = ParseBoolFlag(Params, TEXT("ClearGraph="), false);

    TArray<FString> JsonFiles;
    IFileManager::Get().FindFilesRecursive(JsonFiles, *ImportDir, TEXT("*.bpl_json"), true, false);
    JsonFiles.Sort();

    UE_LOG(LogTemp, Display, TEXT("BlueprintGraphImport: discovered %d json files."), JsonFiles.Num());

    FBlueprintRAGImportOptions ImportOptions;
    ImportOptions.bCompileAfterImport = bCompile;
    ImportOptions.bCreateMissingGraphs = bCreateMissingGraphs;
    ImportOptions.bClearGraphBeforeImport = bClearGraph;
    ImportOptions.bMarkDirty = true;

    int32 ImportedCount = 0;
    int32 FailedCount = 0;

    for (const FString& FilePath : JsonFiles)
    {
        FString BlueprintPath;
        if (!FBlueprintGraphRAGService::LoadBlueprintPathFromJsonFile(FilePath, BlueprintPath))
        {
            ++FailedCount;
            UE_LOG(LogTemp, Warning, TEXT("BlueprintGraphImport: failed to read blueprint path from %s"), *FilePath);
            continue;
        }

        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
        if (!Blueprint)
        {
            ++FailedCount;
            UE_LOG(LogTemp, Warning, TEXT("BlueprintGraphImport: failed to load blueprint %s"), *BlueprintPath);
            continue;
        }

        FString Error;
        if (!FBlueprintGraphRAGService::ImportBlueprintFromFile(Blueprint, FilePath, ImportOptions, Error))
        {
            ++FailedCount;
            UE_LOG(LogTemp, Warning, TEXT("BlueprintGraphImport: %s"), *Error);
            continue;
        }

        if (bSave)
        {
            FString SaveError;
            if (!FBlueprintGraphRAGService::SaveBlueprintPackage(Blueprint, SaveError))
            {
                ++FailedCount;
                UE_LOG(LogTemp, Warning, TEXT("BlueprintGraphImport: %s"), *SaveError);
                continue;
            }
        }

        ++ImportedCount;
        UE_LOG(LogTemp, Display, TEXT("BlueprintGraphImport: imported %s"), *BlueprintPath);
    }

    UE_LOG(LogTemp, Display, TEXT("BlueprintGraphImport: imported=%d failed=%d"), ImportedCount, FailedCount);
    return FailedCount > 0 ? 2 : 0;
}
