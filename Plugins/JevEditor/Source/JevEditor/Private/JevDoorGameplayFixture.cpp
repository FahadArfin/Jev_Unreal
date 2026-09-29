#include "JevDoorGameplayFixture.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"

AJevDoorGameplayFixture::AJevDoorGameplayFixture()
{
    auto* Trigger = CreateDefaultSubobject<UBoxComponent>(TEXT("DoorTrigger"));
    SetRootComponent(Trigger); Trigger->SetBoxExtent(FVector(80, 80, 80));
    Trigger->SetMobility(EComponentMobility::Movable);
    Trigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    Trigger->SetCollisionObjectType(ECC_WorldDynamic);
    Trigger->SetCollisionResponseToAllChannels(ECR_Overlap);
    Trigger->SetGenerateOverlapEvents(true);
    auto* Visual = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DoorVisual"));
    Visual->SetupAttachment(Trigger); Visual->SetMobility(EComponentMobility::Movable);
    Visual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Visual->SetRelativeScale3D(FVector(1.5, .3, 2.5));
}
