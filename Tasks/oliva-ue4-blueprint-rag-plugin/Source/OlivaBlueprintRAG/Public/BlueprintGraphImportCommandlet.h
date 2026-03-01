#pragma once

#include "Commandlets/Commandlet.h"
#include "BlueprintGraphImportCommandlet.generated.h"

UCLASS()
class OLIVABLUEPRINTRAG_API UBlueprintGraphImportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UBlueprintGraphImportCommandlet();

    virtual int32 Main(const FString& Params) override;
};

