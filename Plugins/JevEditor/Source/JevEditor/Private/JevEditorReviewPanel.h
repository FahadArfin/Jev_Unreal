#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FJevEditorBridge;
class SDockTab;
class FSpawnTabArgs;
struct FJevReviewAccess;

/** Local human controls over the same bounded preview/apply path as MCP. */
class FJevEditorReviewPanel
{
public:
    FJevEditorReviewPanel();
    ~FJevEditorReviewPanel();
    void Register();
    void SetBridge(FJevEditorBridge* Bridge, const FString& DisabledReason = FString());
    void Unregister();

    // Selection and preview preparation are also testable without constructing Slate.
    static TSharedRef<FJsonObject> InspectSelection(FJevEditorBridge& Bridge);
    static TSharedRef<FJsonObject> PreviewSelection(FJevEditorBridge& Bridge, const FVector* Translation,
        const FString* Label, const FString* Folder);
    static FString DescribeReview(const TSharedPtr<FJsonObject>& Record);

private:
    void RegisterMenus();
    TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args);
    TSharedPtr<FJevReviewAccess> Access;
    TWeakPtr<SDockTab> Tab;
    bool bRegistered = false;
};
