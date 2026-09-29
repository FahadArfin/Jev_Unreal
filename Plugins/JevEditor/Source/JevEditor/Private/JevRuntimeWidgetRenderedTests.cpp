#if WITH_DEV_AUTOMATION_TESTS

#include "JevEditorWorkflowTools.h"
#include "JevEditorBridge.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"

namespace JevRuntimeWidgetRendered
{
struct FState
{
    FAutomationTestBase* Test; FJevEditorBridge Bridge; FJevWorkflowTools Workflows; UPackage* Package; UWidgetBlueprint* BP = nullptr; TWeakObjectPtr<UUserWidget> Instance;
    EPlayNetMode OldMode = PIE_Standalone; int32 OldClients = 1; bool OldProcess = true, OldServer = false;
    double Began = FPlatformTime::Seconds(), LastChange = 0; int32 Phase = 0;
    explicit FState(FAutomationTestBase* InTest) : Test(InTest)
    {
        auto* Settings = GetMutableDefault<ULevelEditorPlaySettings>(); Settings->GetPlayNetMode(OldMode); Settings->GetPlayNumberOfClients(OldClients); Settings->GetRunUnderOneProcess(OldProcess); OldServer = Settings->bLaunchSeparateServer;
        Settings->SetPlayNetMode(PIE_Standalone); Settings->SetPlayNumberOfClients(1); Settings->SetRunUnderOneProcess(true); Settings->bLaunchSeparateServer = false;
        FAutomationEditorCommonUtils::CreateNewMap(); Package = CreatePackage(*(TEXT("/Game/JevRuntimeUIFixture_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))); Package->AddToRoot();
        BP = CastChecked<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), Package, TEXT("ReviewWidget"), BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass())); FAssetRegistryModule::AssetCreated(BP);
        auto* Root = BP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root")); BP->WidgetTree->RootWidget = Root;
        auto* Text = BP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("OverflowLabel")); Text->SetText(FText::FromString(TEXT("A long label intentionally exceeds the small allocation"))); auto* TextSlot = Root->AddChildToCanvas(Text); TextSlot->SetPosition(FVector2D(20, 20)); TextSlot->SetSize(FVector2D(40, 10));
        auto* Button = BP->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("ContinueButton")); auto* ButtonSlot = Root->AddChildToCanvas(Button); ButtonSlot->SetPosition(FVector2D(20, 100)); ButtonSlot->SetSize(FVector2D(160, 50));
        auto* Label = BP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ButtonLabel")); Label->SetText(FText::FromString(TEXT("Continue"))); Button->AddChild(Label); FKismetEditorUtilities::CompileBlueprint(BP);
    }
    ~FState()
    {
        if (Instance.IsValid()) Instance->RemoveFromParent();
        FAssetRegistryModule::AssetDeleted(BP); BP->ClearFlags(RF_Public | RF_Standalone); Package->SetDirtyFlag(false); Package->RemoveFromRoot();
        auto* Settings = GetMutableDefault<ULevelEditorPlaySettings>(); Settings->SetPlayNetMode(OldMode); Settings->SetPlayNumberOfClients(OldClients); Settings->SetRunUnderOneProcess(OldProcess); Settings->bLaunchSeparateServer = OldServer;
    }
};
class FInspect : public IAutomationLatentCommand
{
public:
    explicit FInspect(TSharedRef<FState> InState) : State(InState) {}
    virtual bool Update() override
    {
        auto& S = *State; const double Now = FPlatformTime::Seconds();
        if (Now - S.Began > 45) { S.Test->AddError(TEXT("Runtime widget fixture exceeded 45 seconds.")); return true; }
        auto* World = GEditor->PlayWorld.Get(); if (!World || !World->HasBegunPlay()) return false;
        if (!S.Instance.IsValid())
        {
            S.Instance = CreateWidget<UUserWidget>(World, S.BP->GeneratedClass.Get());
            if (!S.Test->TestTrue(TEXT("owned Widget Blueprint actually instantiates in PIE"), S.Instance.IsValid())) return true;
            S.Instance->AddToViewport(); S.Instance->SetDesiredSizeInViewport(FVector2D(640, 360)); S.LastChange = Now; return false;
        }
        if (Now - S.LastChange < 1) return false;
        auto* Button = Cast<UButton>(S.Instance->WidgetTree->FindWidget(TEXT("ContinueButton")));
        if (!Button || !Button->GetCachedWidget().IsValid()) return false;
        if (S.Phase == 0) { FSlateApplication::Get().SetKeyboardFocus(Button->GetCachedWidget(), EFocusCause::SetDirectly); S.Phase = 1; S.LastChange = Now; return false; }
        auto Query = MakeShared<FJsonObject>(); Query->SetStringField(TEXT("kind"), TEXT("widgets")); Query->SetStringField(TEXT("target_path"), S.BP->GetPathName()); Query->SetStringField(TEXT("runtime_instance_path"), S.Instance->GetPathName());
        auto StatusRequest = MakeShared<FJsonObject>(); StatusRequest->SetStringField(TEXT("action"), TEXT("status")); StatusRequest->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
        const auto Identity = S.Bridge.Execute(StatusRequest)->GetObjectField(TEXT("result"));
        // The module routes workflows before Bridge.Execute's scene-only PIE guard.
        // Exercise its real dispatcher with real bridge identity, not the leaf helper.
        S.Test->TestTrue(TEXT("module dispatch recognizes workflow inspection"), FJevWorkflowTools::HandlesAction(TEXT("workflow_inspect")));
        const auto Response = S.Workflows.Execute(TEXT("workflow_inspect"), Query, Identity.ToSharedRef());
        if (!S.Test->TestTrue(TEXT("existing PIE widget can be inspected through native workflow dispatch"), Response->GetBoolField(TEXT("ok"))))
        {
            const TSharedPtr<FJsonObject>* Error = nullptr;
            if (Response->TryGetObjectField(TEXT("error"), Error)) S.Test->AddError((*Error)->GetStringField(TEXT("code")) + TEXT(": ") + (*Error)->GetStringField(TEXT("message")));
            return true;
        }
        const auto R = Response->GetObjectField(TEXT("result")); S.Test->TestTrue(TEXT("runtime scope is explicit"), R->GetBoolField(TEXT("runtime_instantiated"))); S.Test->TestTrue(TEXT("actual game viewport has positive width"), R->GetArrayField(TEXT("viewport_size_pixels"))[0]->AsNumber() > 0); S.Test->TestTrue(TEXT("cached layout scale is available"), R->GetNumberField(TEXT("root_accumulated_layout_scale")) > 0);
        for (const TCHAR* Key : {TEXT("project_file"), TEXT("session_id"), TEXT("world_path"), TEXT("revision")}) S.Test->TestEqual(TEXT("runtime result preserves connected project and scene identity"), R->GetStringField(Key), Identity->GetStringField(Key));
        bool Overflow = false, Focus = false;
        for (const auto& Value : R->GetArrayField(TEXT("widgets")))
        {
            const auto Row = Value->AsObject(); if (Row->GetStringField(TEXT("name")) == TEXT("OverflowLabel")) Overflow = Row->GetBoolField(TEXT("desired_size_exceeds_allocation")); if (Row->GetStringField(TEXT("name")) == TEXT("ContinueButton")) Focus = Row->GetBoolField(TEXT("keyboard_focus"));
        }
        S.Test->TestTrue(TEXT("deliberately undersized live text produces overflow hint"), Overflow); S.Test->TestTrue(TEXT("actual native keyboard focus is observed"), Focus);
        TArray<FColor> Pixels; FIntVector Size = FIntVector::ZeroValue; const auto Content = S.Instance->GetCachedWidget();
        if (S.Test->TestTrue(TEXT("native Slate captures owned runtime content"), Content.IsValid() && FSlateApplication::Get().TakeScreenshot(Content.ToSharedRef(), Pixels, Size)))
        {
            if (!S.Test->TestTrue(TEXT("runtime capture dimensions match pixel data"), Size.X > 0 && Size.Y > 0 && static_cast<int64>(Size.X) * Size.Y == Pixels.Num())) return true;
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png); const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/Jev")); IFileManager::Get().MakeDirectory(*Dir, true);
            S.Test->TestTrue(TEXT("runtime allocation evidence PNG saved"), !Png.IsEmpty() && FFileHelper::SaveArrayToFile(Png, *FPaths::Combine(Dir, S.Phase == 1 ? TEXT("RuntimeWidgetWide.png") : TEXT("RuntimeWidgetCompact.png"))));
        }
        if (S.Phase == 1) { S.Instance->SetDesiredSizeInViewport(FVector2D(320, 240)); S.Phase = 2; S.LastChange = Now; return false; }
        S.Instance->RemoveFromParent(); S.Test->TestFalse(TEXT("removed instance no longer accepted as on-screen"), S.Workflows.Execute(TEXT("workflow_inspect"), Query, Identity.ToSharedRef())->GetBoolField(TEXT("ok"))); return true;
    }
private:
    TSharedRef<FState> State;
};
class FAfter : public IAutomationLatentCommand
{
public:
    explicit FAfter(TSharedRef<FState> InState) : State(InState) {}
    virtual bool Update() override
    {
        if (GEditor->PlayWorld && FPlatformTime::Seconds() - State->Began < 60) return false;
        State->Test->TestNull(TEXT("owned runtime UI PIE session ended"), GEditor->PlayWorld.Get()); return true;
    }
private:
    TSharedRef<FState> State;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJevRuntimeWidgetRendered, "Jev.Rendered.RuntimeWidgetInspection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FJevRuntimeWidgetRendered::RunTest(const FString&)
{
    if (!FApp::CanEverRender() || GEditor->PlayWorld) { AddError(TEXT("Runtime UI fixture requires a rendered editor outside PIE.")); return false; }
    auto State = MakeShared<JevRuntimeWidgetRendered::FState>(this); ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false)); FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<JevRuntimeWidgetRendered::FInspect>(State)); ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand()); FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<JevRuntimeWidgetRendered::FAfter>(State)); return true;
}

#endif
