#pragma once

#include "CoreMinimal.h"
#include "Engine/Blueprint.h"
#include "K2Node.h"
#include "JevBlueprintTestTypes.generated.h"

/** An unsupported custom asset class proves exact-class matching stays closed. */
UCLASS()
class UJevBlueprintSubclassFixture : public UBlueprint
{
    GENERATED_BODY()
};

/** Source-only deterministic compiler failure for native automation. */
UCLASS()
class UJevBlueprintErrorFixtureNode : public UK2Node
{
    GENERATED_BODY()
public:
    virtual void EarlyValidation(FCompilerResultsLog& MessageLog) const override;
};
