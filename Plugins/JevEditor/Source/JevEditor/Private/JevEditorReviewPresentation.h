#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Presentation only: this never resolves editor objects, authorizes, or executes a plan. */
struct FJevReviewPresentation
{
    bool bValid = false;
    FText Summary;
    FText Body;
    FText TechnicalDetails;
};

class FJevEditorReviewPresentation
{
public:
    static FJevReviewPresentation Build(const TSharedPtr<FJsonObject>& Record);
    static FText RecoveryMessage(const FString& Code, const FString& NativeMessage, bool bApplyAttempted);
};
