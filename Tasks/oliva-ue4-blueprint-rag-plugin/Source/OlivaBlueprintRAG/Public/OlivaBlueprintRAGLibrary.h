#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "OlivaBlueprintRAGLibrary.generated.h"

UCLASS()
class OLIVABLUEPRINTRAG_API UOlivaBlueprintRAGLibrary final : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Oliva|BlueprintRAG")
    static bool ExportAllBlueprintsToJson(
        const FString& RootPackagePath,
        const FString& NamePrefix,
        const FString& OutputDirectory,
        bool bRecursivePaths,
        int32& OutTotal,
        int32& OutExported,
        TArray<FString>& OutErrors);

    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Oliva|BlueprintRAG")
    static bool ImportBlueprintsFromJsonDirectory(
        const FString& JsonDirectory,
        bool bCompileAfterImport,
        bool bSavePackages,
        bool bClearGraphBeforeImport,
        int32& OutTotal,
        int32& OutImported,
        TArray<FString>& OutErrors);
};

