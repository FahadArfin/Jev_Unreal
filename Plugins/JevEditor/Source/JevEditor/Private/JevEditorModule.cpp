#include "JevEditorBridge.h"
#include "JevEditorFunctionalTools.h"
#include "JevEditorProjectTools.h"
#include "JevEditorReviewPanel.h"

#include "Containers/Ticker.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "IPAddress.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

DEFINE_LOG_CATEGORY_STATIC(LogJevEditor, Log, All);

namespace
{
constexpr int32 MaximumBodyBytes = 65536;

const TArray<FString>* Header(const FHttpServerRequest& Request, const FString& Name)
{
    for (const auto& Pair : Request.Headers)
        if (Pair.Key.Equals(Name, ESearchCase::IgnoreCase)) return &Pair.Value;
    return nullptr;
}

bool EqualSecret(const FString& Actual, const FString& Expected)
{
    // Do not short-circuit on the contents of the secret.
    uint32 Difference = Actual.Len() ^ Expected.Len();
    for (int32 I = 0; I < Expected.Len(); ++I)
        Difference |= static_cast<uint32>((I < Actual.Len() ? Actual[I] : 0) ^ Expected[I]);
    return Difference == 0;
}

void Reply(const FHttpResultCallback& Complete, const TSharedRef<FJsonObject>& Object, int32 Status = 200)
{
    const FString Body = FJevEditorBridge::BoundedResponseBody(Object);
    auto Response = FHttpServerResponse::Create(Body, TEXT("application/json; charset=utf-8"));
    Response->Code = static_cast<EHttpServerResponseCodes>(Status);
    Response->Headers.Add(TEXT("Cache-Control"), {TEXT("no-store")});
    Response->Headers.Add(TEXT("X-Content-Type-Options"), {TEXT("nosniff")});
    Complete(MoveTemp(Response));
}
}

class FJevEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        if (IsRunningCommandlet()) return;
        ReviewPanel = MakeUnique<FJevEditorReviewPanel>();
        ReviewPanel->Register();
        ReviewPanel->SetBridge(nullptr, TEXT("Bridge is disabled. Configure the token and restart this editor."));
        const FString PortText = FPlatformMisc::GetEnvironmentVariable(TEXT("JEV_BRIDGE_PORT"));
        if (!PortText.IsEmpty())
        {
            bool bValidPort = PortText.Len() <= 5;
            for (TCHAR C : PortText) bValidPort &= C >= '0' && C <= '9';
            const int32 ParsedPort = bValidPort ? FCString::Atoi(*PortText) : 0;
            if (ParsedPort < 1024 || ParsedPort > 65535)
            {
                ReviewPanel->SetBridge(nullptr, TEXT("JEV_BRIDGE_PORT must be an integer from 1024 to 65535."));
                UE_LOG(LogJevEditor, Error, TEXT("Bridge disabled: invalid JEV_BRIDGE_PORT."));
                return;
            }
            BridgePort = static_cast<uint32>(ParsedPort);
        }
        Token = FPlatformMisc::GetEnvironmentVariable(TEXT("JEV_BRIDGE_TOKEN"));
        bool bValidToken = Token.Len() >= 32 && Token.Len() <= 256;
        for (TCHAR Character : Token) bValidToken &= Character >= 33 && Character <= 126;
        if (!bValidToken)
        {
            UE_LOG(LogJevEditor, Display, TEXT("Bridge disabled: set JEV_BRIDGE_TOKEN to 32-256 printable non-space ASCII characters before launching the editor."));
            Token.Empty();
            return;
        }

        // Fail closed if anyone already owns this port. Reusing a router could attach our
        // authenticated API to a wildcard listener created by another editor plugin.
        ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        if (!Sockets) return;
        {
            auto Probe = Sockets->CreateUniqueSocket(NAME_Stream, TEXT("JevLoopbackPortProbe"));
            const auto Address = Sockets->CreateInternetAddr();
            bool bValidIp = false;
            Address->SetIp(TEXT("127.0.0.1"), bValidIp);
            Address->SetPort(BridgePort);
            if (!Probe || !bValidIp || !Probe->Bind(*Address))
            {
                ReviewPanel->SetBridge(nullptr, TEXT("The configured loopback port is occupied or unavailable. Select a unique JEV_BRIDGE_PORT and restart."));
                UE_LOG(LogJevEditor, Error, TEXT("Bridge disabled: loopback port %u is already occupied or unavailable."), BridgePort);
                return;
            }
        }

        // HTTPServer reads per-port ListenerOverrides when it binds. Prepend our explicit
        // override and invalidate its cache; do not change any other listener's address.
        // This is an in-memory engine config override and is never flushed to disk.
        TArray<FString> Overrides;
        GConfig->GetArray(TEXT("HTTPServer.Listeners"), TEXT("ListenerOverrides"), Overrides, GEngineIni);
        Overrides.Insert(FString::Printf(TEXT("(Port=%u,BindAddress=127.0.0.1,ReuseAddressAndPort=false,MaxConnectionsAcceptPerFrame=4,ConnectionsBacklogSize=16)"), BridgePort), 0);
        GConfig->SetArray(TEXT("HTTPServer.Listeners"), TEXT("ListenerOverrides"), Overrides, GEngineIni);
        TSet<FString> Sections = { TEXT("HTTPServer.Listeners") };
        FCoreDelegates::TSOnConfigSectionsChanged().Broadcast(GEngineIni, Sections);

        FHttpServerModule& Server = FHttpServerModule::Get();
        Server.StartAllListeners();
        Router = Server.GetHttpRouter(BridgePort, true);
        if (!Router)
        {
            UE_LOG(LogJevEditor, Error, TEXT("Bridge disabled: could not create the loopback listener."));
            return;
        }
        Bridge = MakeUnique<FJevEditorBridge>();
        Route = Router->BindRoute(FHttpPath(TEXT("/jev/v1/call")), EHttpServerRequestVerbs::VERB_POST,
            FHttpRequestHandler::CreateRaw(this, &FJevEditorModule::HandleRequest));
        if (!Route)
        {
            Bridge.Reset();
            UE_LOG(LogJevEditor, Error, TEXT("Bridge disabled: its route is already occupied."));
            return;
        }
        ProjectTools = MakeUnique<FJevProjectTools>();
        FunctionalTools = MakeUnique<FJevFunctionalTools>();
        ReviewPanel->SetBridge(Bridge.Get());
        TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FJevEditorModule::Tick));
        UE_LOG(LogJevEditor, Display, TEXT("Jev editor bridge listening at http://127.0.0.1:%u/jev/v1/call (authentication required)."), BridgePort);
    }

    virtual void ShutdownModule() override
    {
        if (TickHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        if (ProjectTools) ProjectTools->Shutdown();
        ProjectTools.Reset();
        if (FunctionalTools) FunctionalTools->Shutdown();
        FunctionalTools.Reset();
        if (ReviewPanel) ReviewPanel->Unregister();
        ReviewPanel.Reset();
        if (Router && Route) Router->UnbindRoute(Route);
        Route.Reset();
        Router.Reset();
        Bridge.Reset();
        Token.Empty();
        // HTTPServer is shared. Never call StopAllListeners from this plugin.
    }

private:
    TSharedPtr<FJsonObject> Identity()
    {
        if (!Bridge) return nullptr;
        auto Request = MakeShared<FJsonObject>();
        Request->SetStringField(TEXT("action"), TEXT("status"));
        Request->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
        const auto Response = Bridge->Execute(Request);
        bool bOk = false;
        const TSharedPtr<FJsonObject>* Result = nullptr;
        return Response->TryGetBoolField(TEXT("ok"), bOk) && bOk && Response->TryGetObjectField(TEXT("result"), Result) ? *Result : nullptr;
    }

    bool Tick(float)
    {
        if ((!ProjectTools || !ProjectTools->HasActiveJob()) && (!FunctionalTools || !FunctionalTools->HasActiveJob())) return true;
        const auto Current = Identity();
        if (!Current)
        {
            if (ProjectTools) ProjectTools->Shutdown();
            if (FunctionalTools) FunctionalTools->Shutdown();
            return true;
        }
        if (ProjectTools && Current) ProjectTools->Tick(Current.ToSharedRef());
        if (FunctionalTools && Current) FunctionalTools->Tick(Current.ToSharedRef());
        return true;
    }

    bool HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& Complete)
    {
        if (!IsInGameThread() || !Bridge)
        {
            Reply(Complete, FJevEditorBridge::Error(TEXT("editor_unavailable"), TEXT("Editor bridge is unavailable.")), 503);
            return true;
        }
        if (!Request.PeerAddress || Request.PeerAddress->ToString(false) != TEXT("127.0.0.1") || Header(Request, TEXT("Origin")))
        {
            Reply(Complete, FJevEditorBridge::Error(TEXT("forbidden"), TEXT("Only local non-browser clients are accepted.")), 403);
            return true;
        }
        const auto* Host = Header(Request, TEXT("Host"));
        if (!Host || Host->Num() != 1 || (*Host)[0] != FString::Printf(TEXT("127.0.0.1:%u"), BridgePort))
        {
            Reply(Complete, FJevEditorBridge::Error(TEXT("forbidden"), TEXT("The Host header must identify the loopback bridge.")), 403);
            return true;
        }
        const auto* Authorization = Header(Request, TEXT("Authorization"));
        if (!Authorization || Authorization->Num() != 1 || !EqualSecret((*Authorization)[0], TEXT("Bearer ") + Token))
        {
            Reply(Complete, FJevEditorBridge::Error(TEXT("unauthorized"), TEXT("A valid bridge bearer token is required.")), 401);
            return true;
        }
        const auto* ContentType = Header(Request, TEXT("Content-Type"));
        if (!ContentType || ContentType->Num() != 1 || !(*ContentType)[0].StartsWith(TEXT("application/json"), ESearchCase::IgnoreCase) || !Request.QueryParams.IsEmpty())
        {
            Reply(Complete, FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Send application/json without URL query parameters.")), 400);
            return true;
        }
        if (Request.Body.Num() == 0 || Request.Body.Num() > MaximumBodyBytes)
        {
            Reply(Complete, FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Request body must contain 1 to 65536 bytes.")), 413);
            return true;
        }
        const double Now = FPlatformTime::Seconds();
        if (Now - RateWindow >= 1.0) { RateWindow = Now; RequestsInWindow = 0; }
        if (++RequestsInWindow > 30)
        {
            Reply(Complete, FJevEditorBridge::Error(TEXT("rate_limited"), TEXT("At most 30 authenticated requests per second are accepted.")), 429);
            return true;
        }
        const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
        FString Body(Converted.Length(), Converted.Get());
        TSharedPtr<FJsonObject> Object;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Object) || !Object)
        {
            Reply(Complete, FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Body must be a JSON object.")), 400);
            return true;
        }
        FString Action;
        if (Object->TryGetStringField(TEXT("action"), Action) &&
            ((ProjectTools && FJevProjectTools::HandlesAction(Action)) || (FunctionalTools && FJevFunctionalTools::HandlesAction(Action))))
        {
            const TSharedPtr<FJsonObject>* Params = nullptr;
            bool bValid = Object->TryGetObjectField(TEXT("params"), Params);
            for (const auto& Field : Object->Values) bValid &= Field.Key == TEXT("action") || Field.Key == TEXT("params");
            const auto Current = Identity();
            if (!bValid) Reply(Complete, FJevEditorBridge::Error(TEXT("bad_request"), TEXT("Use action and an object params only.")));
            else if (!Current) Reply(Complete, FJevEditorBridge::Error(TEXT("editor_unavailable"), TEXT("Editor identity is unavailable.")));
            else if ((Action == TEXT("validation_start") && FunctionalTools->HasActiveJob()) ||
                (Action == TEXT("functional_start") && ProjectTools->HasActiveJob()))
                Reply(Complete, FJevEditorBridge::Error(TEXT("job_busy"), TEXT("Another project job is active.")));
            else if (FJevFunctionalTools::HandlesAction(Action)) Reply(Complete, FunctionalTools->Execute(Action, *Params, Current.ToSharedRef()));
            else Reply(Complete, ProjectTools->Execute(Action, *Params, Current.ToSharedRef()));
        }
        else Reply(Complete, Bridge->Execute(Object));
        return true;
    }

    FString Token;
    uint32 BridgePort = 9845;
    TUniquePtr<FJevEditorBridge> Bridge;
    TUniquePtr<FJevProjectTools> ProjectTools;
    TUniquePtr<FJevFunctionalTools> FunctionalTools;
    TUniquePtr<FJevEditorReviewPanel> ReviewPanel;
    FTSTicker::FDelegateHandle TickHandle;
    TSharedPtr<IHttpRouter> Router;
    FHttpRouteHandle Route;
    double RateWindow = 0;
    int32 RequestsInWindow = 0;
};

IMPLEMENT_MODULE(FJevEditorModule, JevEditor)
