#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "JevDoorGameplayFixture.generated.h"

/** Source-only automation parent: actual overlap geometry, no door logic in C++. */
UCLASS(Blueprintable, NotPlaceable)
class AJevDoorGameplayFixture : public AActor
{
    GENERATED_BODY()
public:
    AJevDoorGameplayFixture();
};
