#include "JevFunctionalTestFixture.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "FunctionalTestingModule.h"
#include "Misc/OutputDeviceRedirector.h"

int32 AJevFunctionalTestFixture::CleanupCount = 0;
int32 AJevFunctionalTestFixture::SuccessfulMovementCount = 0;

AJevFunctionalTestFixture::AJevFunctionalTestFixture()
{
    TestLabel = TEXT("Jev source movement fixture");
    TimeLimit = 0;
    LogErrorHandling = EFunctionalTestLogHandling::OutputIgnored;
    LogWarningHandling = EFunctionalTestLogHandling::OutputIgnored;
    OnTestFinished.AddDynamic(this, &AJevFunctionalTestFixture::HandleFixtureFinished);
}

void AJevFunctionalTestFixture::ConfigureMode(int32 Mode)
{
    FixtureMode = Mode;
    LogErrorHandling = Mode == 3 ? EFunctionalTestLogHandling::OutputIsError : EFunctionalTestLogHandling::OutputIgnored;
}

void AJevFunctionalTestFixture::StartTest()
{
    Super::StartTest();
#if WITH_DEV_AUTOMATION_TESTS
    ProbeTime = 0;
    Probe = GetWorld()->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator);
    if (auto* MeshActor = Cast<AStaticMeshActor>(Probe))
    {
        MeshActor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
        MeshActor->SetActorLocation(FVector(100, 200, 300));
    }
#endif
}

void AJevFunctionalTestFixture::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
#if WITH_DEV_AUTOMATION_TESTS
    if (IsRunning() && Probe && FixtureMode != 2 && FixtureMode != 5)
    {
        ProbeTime += DeltaSeconds;
        if (ProbeTime > 0.05f)
        {
            const bool bMoved = Probe->GetActorLocation().Equals(FVector(100, 200, 300), 0.01);
            if (bMoved) ++SuccessfulMovementCount;
            if (FixtureMode == 3)
            {
                AssertTrue(false, TEXT("Intentional assertion before Succeeded fixture."));
                // UE_LOG's expected-error hook downgrades expected messages before
                // dispatch; directly exercise the observer's real Error path.
                GLog->Serialize(TEXT("Intentional native functional log error fixture."), ELogVerbosity::Error, FName(TEXT("LogFunctionalTest")));
            }
            const bool bPass = bMoved && FixtureMode != 1;
            FinishTest(bPass ? EFunctionalTestResult::Succeeded : EFunctionalTestResult::Failed, bPass ? TEXT("Probe reached the expected location.") : TEXT("Intentional negative movement fixture."));
        }
    }
#endif
}

void AJevFunctionalTestFixture::CleanUp()
{
#if WITH_DEV_AUTOMATION_TESTS
    ++CleanupCount;
    if (IsValid(Probe)) Probe->Destroy();
    Probe = nullptr;
#endif
    Super::CleanUp();
    if (FixtureMode == 4) Destroy();
}

void AJevFunctionalTestFixture::HandleFixtureFinished()
{
#if WITH_DEV_AUTOMATION_TESTS
    if (FixtureMode == 5) { if (IsValid(Probe)) Probe->Destroy(); Destroy(); }
#endif
}
