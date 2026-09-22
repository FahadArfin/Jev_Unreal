#if WITH_DEV_AUTOMATION_TESTS

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "Layout/Children.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Widgets/Docking/SDockTab.h"

namespace JevReviewRendered
{
TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& Widget, FName Tag)
{
    if (Widget->GetTag() == Tag) return Widget;
    FChildren* Children = Widget->GetChildren();
    for (int32 Index = 0; Index < Children->Num(); ++Index)
        if (auto Found = Find(Children->GetChildAt(Index), Tag)) return Found;
    return nullptr;
}

class FCaptureReview : public IAutomationLatentCommand
{
public:
    FCaptureReview(FAutomationTestBase* InTest, TSharedPtr<SDockTab> InTab, bool bInAlreadyOpen)
        : Test(InTest), Tab(MoveTemp(InTab)), bAlreadyOpen(bInAlreadyOpen), Started(FPlatformTime::Seconds()) {}

    virtual bool Update() override
    {
        // Wait for normal Slate layout/paint; do not manually tick or block the editor.
        if (FPlatformTime::Seconds() - Started < 1.0) return false;
        if (!Tab.IsValid()) { Test->AddError(TEXT("Jev Review tab became unavailable.")); return true; }
        // SDockTab's own widget tree is its draggable header. The panel is hosted
        // separately by the docking stack and must be traversed/captured as content.
        const TSharedRef<SWidget> Content = Tab->GetContent();
        const FVector2D ContentSize = Content->GetCachedGeometry().GetLocalSize();
        Test->TestTrue(TEXT("Panel content has a meaningful arranged area (at least 240 by 200)"), ContentSize.X >= 240 && ContentSize.Y >= 200);
        for (const TCHAR* Name : {TEXT("Jev.Review.Root"), TEXT("Jev.Review.Status"), TEXT("Jev.Review.Inspect"),
            TEXT("Jev.Review.Translate.X"), TEXT("Jev.Review.Translate.Y"), TEXT("Jev.Review.Translate.Z"),
            TEXT("Jev.Review.Label"), TEXT("Jev.Review.Folder"), TEXT("Jev.Review.Plan"), TEXT("Jev.Review.Apply")})
        {
            const auto Widget = Find(Content, FName(Name));
            Test->TestTrue(FString::Printf(TEXT("Rendered control exists: %s"), Name), Widget.IsValid());
            if (!Widget) continue;
            const FVector2D Size = Widget->GetCachedGeometry().GetLocalSize();
            Test->TestTrue(FString::Printf(TEXT("Control has a nonzero arranged size: %s"), Name), Size.X > 0 && Size.Y > 0);
            if (!bAlreadyOpen && FString(Name) == TEXT("Jev.Review.Apply"))
                Test->TestFalse(TEXT("Opening a fresh tab never enables Apply without review"), Widget->IsEnabled());
#if WITH_ACCESSIBILITY
            if (FString(Name).Contains(TEXT("Translate.")) || FString(Name) == TEXT("Jev.Review.Label") || FString(Name) == TEXT("Jev.Review.Folder"))
                Test->TestFalse(FString::Printf(TEXT("Input has an accessible name: %s"), Name), Widget->GetAccessibleText().IsEmpty());
#endif
        }
        TArray<FColor> Pixels;
        FIntVector Size;
        if (Test->TestTrue(TEXT("Native Slate panel-content screenshot captured"), FSlateApplication::Get().TakeScreenshot(Content, Pixels, Size)))
        {
            if (!Test->TestTrue(TEXT("Screenshot contains panel content rather than just a tab header"), Size.X >= 240 && Size.Y >= 200 && static_cast<int64>(Size.X) * Size.Y == Pixels.Num()))
            {
                if (!bAlreadyOpen) Tab->RequestCloseTab();
                return true;
            }
            TArray64<uint8> Png;
            FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
            const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/Jev"));
            const FString Destination = FPaths::Combine(Directory, TEXT("ReviewPanel.png"));
            IFileManager::Get().MakeDirectory(*Directory, true);
            Test->TestTrue(TEXT("Review-panel PNG saved for human visual acceptance"), !Png.IsEmpty() && FFileHelper::SaveArrayToFile(Png, *Destination));
            Test->AddInfo(TEXT("Captured Saved/Automation/Jev/ReviewPanel.png. Construction, geometry and input names were checked; keyboard, screen-reader and artist acceptance remain manual."));
        }
        // Preserve a panel that was already open before this explicitly selected test.
        if (!bAlreadyOpen) Tab->RequestCloseTab();
        return true;
    }

private:
    FAutomationTestBase* Test;
    TSharedPtr<SDockTab> Tab;
    bool bAlreadyOpen;
    double Started;
};
}

// Deliberately outside Jev.Editor: a NullRHI suite must never claim visual acceptance.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevReviewPanelRenderedTest, "Jev.Rendered.ReviewPanel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevReviewPanelRenderedTest::RunTest(const FString& Parameters)
{
    if (!FApp::CanEverRender() || !FSlateApplication::IsInitialized())
    {
        AddError(TEXT("Jev.Rendered.ReviewPanel requires a rendered editor. Run it separately from NullRHI automation."));
        return false;
    }
    const FTabId Id(FName(TEXT("JevReview")));
    const bool bAlreadyOpen = FGlobalTabmanager::Get()->FindExistingLiveTab(Id).IsValid();
    const auto Tab = FGlobalTabmanager::Get()->TryInvokeTab(Id);
    if (!TestTrue(TEXT("Window-menu review tab opens"), Tab.IsValid())) return false;
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<JevReviewRendered::FCaptureReview>(this, Tab, bAlreadyOpen));
    return true;
}

#endif
