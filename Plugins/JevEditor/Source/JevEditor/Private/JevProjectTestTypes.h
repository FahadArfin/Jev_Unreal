#pragma once

#include "CoreMinimal.h"
#include "EditorValidatorBase.h"
#include "Engine/DataAsset.h"
#include "Blueprint/BlueprintExtension.h"
#include "JevProjectTestTypes.generated.h"

/** Source-only automation fixture; never enabled by normal editor validation. */
UCLASS()
class UJevProjectFixtureValidator : public UEditorValidatorBase
{
    GENERATED_BODY()
public:
    static bool bAutomationEnabled;
    static int32 Executions;
    static TFunction<void()> OnValidate;
    virtual bool IsEnabled() const override { return bAutomationEnabled; }
protected:
    virtual bool CanValidateAsset_Implementation(const FAssetData& AssetData, UObject* Asset, FDataValidationContext& Context) const override;
    virtual EDataValidationResult ValidateLoadedAsset_Implementation(const FAssetData& AssetData, UObject* Asset, FDataValidationContext& Context) override;
};

UCLASS()
class UJevProjectFixtureExtension : public UBlueprintExtension
{
    GENERATED_BODY()
public:
    static int32 GraphCallbacks;
    virtual void GetAllGraphs(TArray<UEdGraph*>& Graphs) const override { ++GraphCallbacks; }
};

/** Source-only import-provenance fixture, kept entirely in memory. */
UCLASS()
class UJevProjectFixtureAsset : public UDataAsset
{
    GENERATED_BODY()
public:
    FString ImportMetadata;
    virtual void GetAssetRegistryTags(FAssetRegistryTagsContext Context) const override;
};

/** Exercises real native validation conventions: legacy result helpers and dirty side effects. */
UCLASS()
class UJevCompatibilityFixtureValidator : public UEditorValidatorBase
{
    GENERATED_BODY()
public:
    static bool bAutomationEnabled;
    static int32 PostCalls;
    virtual bool IsEnabled() const override { return bAutomationEnabled; }
    virtual void PostAssetValidation(TArray<TSharedRef<FTokenizedMessage>>& OutMessages) override { ++PostCalls; }
protected:
    virtual bool CanValidateAsset_Implementation(const FAssetData& Data, UObject* Asset, FDataValidationContext& Context) const override;
    virtual EDataValidationResult ValidateLoadedAsset_Implementation(const FAssetData& Data, UObject* Asset, FDataValidationContext& Context) override;
private:
    int32 InstanceCalls = 0;
};
