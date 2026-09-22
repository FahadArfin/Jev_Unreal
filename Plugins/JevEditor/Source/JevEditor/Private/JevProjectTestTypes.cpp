#include "JevProjectTestTypes.h"

#include "HAL/PlatformProcess.h"
#include "Misc/DataValidation.h"
#include "UObject/AssetRegistryTagsContext.h"

bool UJevProjectFixtureValidator::bAutomationEnabled = false;
int32 UJevProjectFixtureValidator::Executions = 0;
TFunction<void()> UJevProjectFixtureValidator::OnValidate;
int32 UJevProjectFixtureExtension::GraphCallbacks = 0;

bool UJevProjectFixtureValidator::CanValidateAsset_Implementation(const FAssetData& AssetData, UObject* Asset, FDataValidationContext& Context) const
{
    return bAutomationEnabled && Asset && Asset->IsA<UJevProjectFixtureAsset>() && !Asset->GetName().Contains(TEXT("Skip"));
}

EDataValidationResult UJevProjectFixtureValidator::ValidateLoadedAsset_Implementation(const FAssetData& AssetData, UObject* Asset, FDataValidationContext& Context)
{
#if WITH_DEV_AUTOMATION_TESTS
    if (bAutomationEnabled)
    {
        ++Executions;
        if (OnValidate) OnValidate();
        if (Asset->GetName().Contains(TEXT("Slow"))) FPlatformProcess::SleepNoStats(1.05f);
        if (Asset->GetName().Contains(TEXT("Invalid")))
        {
            Context.AddError(FText::FromString(TEXT("Jev source fixture intentionally fails its project rule.")));
            return EDataValidationResult::Invalid;
        }
        if (Asset->GetName().Contains(TEXT("Verbose")))
            for (int32 I = 0; I < 20; ++I) Context.AddWarning(FText::FromString(FString::ChrN(700, TEXT('w'))));
        return EDataValidationResult::Valid;
    }
#endif
    return EDataValidationResult::NotValidated;
}

void UJevProjectFixtureAsset::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
    Super::GetAssetRegistryTags(Context);
    if (!ImportMetadata.IsEmpty()) Context.AddTag(FAssetRegistryTag(TEXT("SourceFile"), ImportMetadata, FAssetRegistryTag::TT_Hidden));
}
