#pragma once

#include "CoreMinimal.h"
#include "FunctionalTest.h"
#include "JevFunctionalTestFixture.generated.h"

/** Unsaved source fixture used only by Jev native automation. */
UCLASS(NotBlueprintable, NotPlaceable)
class AJevFunctionalTestFixture : public AFunctionalTest
{
    GENERATED_BODY()
public:
    AJevFunctionalTestFixture();
    UPROPERTY() int32 FixtureMode = 0;
    UPROPERTY() TObjectPtr<AActor> Probe;
    static int32 CleanupCount;
    static int32 SuccessfulMovementCount;
    void ConfigureMode(int32 Mode);
    virtual void Tick(float DeltaSeconds) override;
    virtual void CleanUp() override;
protected:
    virtual void StartTest() override;
private:
    float ProbeTime = 0;
    UFUNCTION() void HandleFixtureFinished();
};
