#pragma once

#include "Commandlets/Commandlet.h"
#include "BlueprintGraphExportCommandlet.generated.h"

UCLASS()
class OLIVABLUEPRINTRAG_API UBlueprintGraphExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UBlueprintGraphExportCommandlet();

    virtual int32 Main(const FString& Params) override;
};

