#include "JevBlueprintGraphEdits.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Serialization/JsonSerializer.h"

namespace JevBlueprintGraph
{
namespace
{
bool Only(const TSharedPtr<FJsonObject>& P, const TArray<FString>& Keys)
{
    if (!P) return false;
    for (const auto& Entry : P->Values) if (!Keys.Contains(FString(*Entry.Key))) return false;
    return true;
}
bool Text(const TSharedPtr<FJsonObject>& P, const TCHAR* Key, FString& Out)
{
    return P && P->HasTypedField<EJson::String>(Key) && P->TryGetStringField(Key, Out) && !Out.IsEmpty() && Out.Len() <= 64;
}
UFunction* Function(const FString& Name)
{
    const TSet<FString> Names = {TEXT("Add_IntInt"), TEXT("Multiply_IntInt"), TEXT("Add_DoubleDouble"), TEXT("Multiply_DoubleDouble"), TEXT("Not_PreBool")};
    return Names.Contains(Name) ? UKismetMathLibrary::StaticClass()->FindFunctionByName(FName(*Name)) : nullptr;
}
UFunction* ActorFunction(const FString& Name)
{
    const TSet<FString> Names = {TEXT("K2_SetActorRelativeLocation"), TEXT("SetActorEnableCollision"), TEXT("SetActorHiddenInGame")};
    return Names.Contains(Name) ? AActor::StaticClass()->FindFunctionByName(FName(*Name)) : nullptr;
}
bool EventName(const FString& Name) { return Name == TEXT("ReceiveBeginPlay") || Name == TEXT("ReceiveActorBeginOverlap"); }
bool VariableName(const FString& Name)
{
    if (Name.IsEmpty() || Name.Len() > 64 || !(FChar::IsAlpha(Name[0]) || Name[0] == TEXT('_'))) return false;
    for (TCHAR C : Name) if (!(C < 128 && (FChar::IsAlnum(C) || C == TEXT('_')))) return false;
    return true;
}
bool Primitive(const FEdGraphPinType& Type)
{
    return !Type.IsContainer() && !Type.bIsReference && (Type.PinCategory == UEdGraphSchema_K2::PC_Int || Type.PinCategory == UEdGraphSchema_K2::PC_Boolean || (Type.PinCategory == UEdGraphSchema_K2::PC_Real && Type.PinSubCategory == UEdGraphSchema_K2::PC_Double));
}
bool LocalVariable(UBlueprint* BP, FName Name)
{
    return BP && BP->NewVariables.ContainsByPredicate([&](const FBPVariableDescription& V) { return V.VarName == Name && Primitive(V.VarType); });
}
bool Supported(UEdGraphNode* N, bool Gameplay)
{
    if (!N || N->Pins.Num() > 32) return false;
    if (N->GetClass() == UK2Node_CallFunction::StaticClass())
    {
        UFunction* F = CastChecked<UK2Node_CallFunction>(N)->GetTargetFunction();
        if (!F) return false;
        if (Function(F->GetName()) == F) return true;
        if (!Gameplay || ActorFunction(F->GetName()) != F) return false;
        const UEdGraphPin* Self = N->FindPin(UEdGraphSchema_K2::PN_Self);
        return Self && Self->LinkedTo.IsEmpty() && Self->DefaultObject == nullptr && Self->DefaultValue.IsEmpty();
    }
    if (!Gameplay) return false;
    if (N->GetClass() == UK2Node_IfThenElse::StaticClass()) return true;
    if (N->GetClass() == UK2Node_Event::StaticClass())
    {
        auto* E = CastChecked<UK2Node_Event>(N); return E->bOverrideFunction && EventName(E->EventReference.GetMemberName().ToString()) && E->EventReference.GetMemberParentClass() == AActor::StaticClass();
    }
    if (N->GetClass() == UK2Node_VariableGet::StaticClass() || N->GetClass() == UK2Node_VariableSet::StaticClass())
    {
        auto* V = CastChecked<UK2Node_Variable>(N); return V->VariableReference.IsSelfContext() && LocalVariable(FBlueprintEditorUtils::FindBlueprintForGraph(N->GetGraph()), V->VariableReference.GetMemberName());
    }
    return false;
}
UEdGraphNode* Node(UEdGraph* G, const FString& Id, bool Gameplay = false)
{
    for (UEdGraphNode* N : G->Nodes) if (N && N->NodeGuid.ToString() == Id && Supported(N, Gameplay)) return N;
    return nullptr;
}
UEdGraphPin* Pin(UEdGraph* G, const FString& NodeId, const FString& PinId, EEdGraphPinDirection Direction, bool Gameplay)
{
    auto* N = Node(G, NodeId, Gameplay); if (!N) return nullptr;
    for (UEdGraphPin* P : N->Pins)
        if (P && P->PinId.ToString() == PinId && P->Direction == Direction && !P->bOrphanedPin && !P->ParentPin && P->SubPins.IsEmpty() && !P->PinType.IsContainer() && !P->PinType.bIsReference &&
            (P->PinType.PinCategory == UEdGraphSchema_K2::PC_Int || P->PinType.PinCategory == UEdGraphSchema_K2::PC_Real || P->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean || (Gameplay && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec))) return P;
    return nullptr;
}
bool Endpoints(UEdGraph* G, const TSharedPtr<FJsonObject>& E, UEdGraphPin*& Output, UEdGraphPin*& Input, bool Gameplay)
{
    FString OutputNodeId, OutputPinId, InputNodeId, InputPinId;
    if (!Text(E, TEXT("output_node_id"), OutputNodeId) || !Text(E, TEXT("output_pin_id"), OutputPinId) || !Text(E, TEXT("input_node_id"), InputNodeId) || !Text(E, TEXT("input_pin_id"), InputPinId)) return false;
    Output = Pin(G, OutputNodeId, OutputPinId, EGPD_Output, Gameplay); Input = Pin(G, InputNodeId, InputPinId, EGPD_Input, Gameplay);
    return Output && Input && Output->GetOwningNode() != Input->GetOwningNode() && Output->PinType == Input->PinType;
}
bool Position(const TSharedPtr<FJsonObject>& E)
{
    double X, Y; return E->HasTypedField<EJson::Number>(TEXT("x")) && E->HasTypedField<EJson::Number>(TEXT("y")) && E->TryGetNumberField(TEXT("x"), X) && E->TryGetNumberField(TEXT("y"), Y) && FMath::IsFinite(X) && FMath::IsFinite(Y) && FMath::Abs(X) <= 100000 && FMath::Abs(Y) <= 100000 && X == FMath::TruncToDouble(X) && Y == FMath::TruncToDouble(Y);
}
bool WouldCycle(UEdGraphPin* Output, UEdGraphPin* Input)
{
    TArray<UEdGraphNode*> Pending = {Input->GetOwningNode()}; TSet<UEdGraphNode*> Seen;
    int32 Edges = 0;
    while (!Pending.IsEmpty())
    {
        UEdGraphNode* N = Pending.Pop(EAllowShrinking::No);
        if (N == Output->GetOwningNode()) return true;
        if (Seen.Contains(N)) continue;
        if (Seen.Num() >= 2048 || !N || N->GetGraph() != Output->GetOwningNode()->GetGraph()) return true;
        Seen.Add(N);
        for (UEdGraphPin* P : N->Pins)
            if (P && P->Direction == EGPD_Output) for (UEdGraphPin* Link : P->LinkedTo)
            {
                if (++Edges > 8192 || !Link) return true;
                Pending.Add(Link->GetOwningNode());
            }
    }
    return false;
}
}

UEdGraph* Graph(UBlueprint* BP, const FString& Id)
{
    if (!BP || BP->GetClass() != UBlueprint::StaticClass()) return nullptr;
    TArray<UEdGraph*> Graphs; BP->GetAllGraphs(Graphs);
    if (Graphs.Num() > 128) return nullptr;
    UEdGraph* Found = nullptr;
    for (UEdGraph* G : Graphs)
        if (G && G->GraphGuid.ToString() == Id)
        {
            if (Found || !G->GraphGuid.IsValid() || !G->GetSchema() || G->GetSchema()->GetClass() != UEdGraphSchema_K2::StaticClass() || G->Nodes.Num() > 2048 || !G->bEditable) return nullptr;
            Found = G;
        }
    return Found;
}

bool Snapshot(UEdGraph* G, FString& Out)
{
    if (!G || G->Nodes.Num() > 2048) return false;
    auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("graph"), G->GetPathName()); R->SetStringField(TEXT("schema"), G->GetSchema()->GetClass()->GetPathName());
    TArray<TSharedPtr<FJsonValue>> Nodes; int32 PinCount = 0, LinkCount = 0; TSet<FGuid> NodeIds;
    for (UEdGraphNode* N : G->Nodes)
    {
        if (!N || !N->NodeGuid.IsValid() || NodeIds.Contains(N->NodeGuid) || N->Pins.Num() > 256) return false;
        NodeIds.Add(N->NodeGuid);
        auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("id"), N->NodeGuid.ToString()); Row->SetStringField(TEXT("class"), N->GetClass()->GetPathName()); Row->SetNumberField(TEXT("instance"), N->GetUniqueID());
        Row->SetNumberField(TEXT("x"), N->NodePosX); Row->SetNumberField(TEXT("y"), N->NodePosY);
        if (N->GetClass() == UK2Node_CallFunction::StaticClass()) Row->SetStringField(TEXT("function"), GetPathNameSafe(CastChecked<UK2Node_CallFunction>(N)->GetTargetFunction()));
        TArray<TSharedPtr<FJsonValue>> Pins; TSet<FGuid> PinIds;
        for (UEdGraphPin* P : N->Pins)
        {
            if (!P || !P->PinId.IsValid() || PinIds.Contains(P->PinId) || ++PinCount > 8192 || P->DefaultValue.Len() > 4096 || P->DefaultTextValue.ToString().Len() > 4096) return false;
            PinIds.Add(P->PinId);
            auto PR = MakeShared<FJsonObject>(); PR->SetStringField(TEXT("id"), P->PinId.ToString()); PR->SetStringField(TEXT("name"), P->PinName.ToString()); PR->SetStringField(TEXT("category"), P->PinType.PinCategory.ToString()); PR->SetStringField(TEXT("subcategory"), P->PinType.PinSubCategory.ToString());
            PR->SetStringField(TEXT("object_type"), GetPathNameSafe(P->PinType.PinSubCategoryObject.Get())); PR->SetNumberField(TEXT("container"), static_cast<int32>(P->PinType.ContainerType)); PR->SetBoolField(TEXT("reference"), P->PinType.bIsReference); PR->SetBoolField(TEXT("const"), P->PinType.bIsConst);
            PR->SetNumberField(TEXT("direction"), P->Direction); PR->SetStringField(TEXT("default"), P->DefaultValue); PR->SetStringField(TEXT("default_object"), GetPathNameSafe(P->DefaultObject)); PR->SetStringField(TEXT("default_text"), P->DefaultTextValue.ToString());
            TArray<TSharedPtr<FJsonValue>> Links;
            for (UEdGraphPin* L : P->LinkedTo) { if (!L || ++LinkCount > 16384) return false; Links.Add(MakeShared<FJsonValueString>(L->GetOwningNode()->NodeGuid.ToString() + TEXT(":") + L->PinId.ToString())); }
            PR->SetArrayField(TEXT("links"), Links); Pins.Add(MakeShared<FJsonValueObject>(PR));
        }
        Row->SetArrayField(TEXT("pins"), Pins); Nodes.Add(MakeShared<FJsonValueObject>(Row));
    }
    TArray<TSharedPtr<FJsonValue>> Variables;
    if (UBlueprint* BP = FBlueprintEditorUtils::FindBlueprintForGraph(G))
    {
        if (BP->NewVariables.Num() > 128) return false;
        for (const FBPVariableDescription& V : BP->NewVariables) { if (V.DefaultValue.Len() > 4096) return false; auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("name"), V.VarName.ToString()); Row->SetStringField(TEXT("type"), V.VarType.PinCategory.ToString()); Row->SetStringField(TEXT("subtype"), V.VarType.PinSubCategory.ToString()); Row->SetStringField(TEXT("default"), V.DefaultValue); Variables.Add(MakeShared<FJsonValueObject>(Row)); }
    }
    R->SetArrayField(TEXT("variables"), Variables);
    R->SetArrayField(TEXT("nodes"), Nodes); return FJsonSerializer::Serialize(R, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));
}

UEdGraphPin* GameplayLiteralPin(UBlueprint* BP, const FString& NodeId, const FString& PinId)
{
    if (!BP || BP->GetClass() != UBlueprint::StaticClass()) return nullptr;
    for (UEdGraph* G : BP->UbergraphPages)
    {
        if (!G || G->Nodes.Num() > 2048) continue;
        auto* P = Pin(G, NodeId, PinId, EGPD_Input, true);
        if (P && P->LinkedTo.IsEmpty() && !P->bDefaultValueIsIgnored && !P->bDefaultValueIsReadOnly && Primitive(P->PinType)) return P;
    }
    return nullptr;
}

bool Validate(UBlueprint* BP, const TSharedPtr<FJsonObject>& E, FString& Reason, bool bGameplay)
{
    Reason = TEXT("Use one documented graph edit on exact native math nodes in a bounded editable K2 graph.");
    FString Id, Op; if (!Text(E, TEXT("graph_id"), Id) || !Text(E, TEXT("operation"), Op)) return false;
    UEdGraph* G = Graph(BP, Id); if (!G) return false;
    FString Baseline; if (!Snapshot(G, Baseline)) return false;
    if (Op == TEXT("add_math_node"))
    {
        FString Name; double X = 0, Y = 0;
        return Only(E, {TEXT("operation"), TEXT("graph_id"), TEXT("function"), TEXT("x"), TEXT("y")}) && Text(E, TEXT("function"), Name) && Function(Name) && G->Nodes.Num() < 2048 && E->HasTypedField<EJson::Number>(TEXT("x")) && E->HasTypedField<EJson::Number>(TEXT("y")) && E->TryGetNumberField(TEXT("x"), X) && E->TryGetNumberField(TEXT("y"), Y) && FMath::IsFinite(X) && FMath::IsFinite(Y) && FMath::Abs(X) <= 100000 && FMath::Abs(Y) <= 100000 && X == FMath::TruncToDouble(X) && Y == FMath::TruncToDouble(Y);
    }
    if (Op == TEXT("remove_math_node"))
    {
        FString NodeId; if (!Only(E, {TEXT("operation"), TEXT("graph_id"), TEXT("node_id")}) || !Text(E, TEXT("node_id"), NodeId)) return false;
        auto* N = Node(G, NodeId); if (!N) return false;
        for (UEdGraphPin* P : N->Pins) if (!P || !P->LinkedTo.IsEmpty()) { Reason = TEXT("Disconnect the reviewed links before removing a math node."); return false; }
        return true;
    }
    if (bGameplay && BP->ParentClass && BP->ParentClass->IsChildOf(AActor::StaticClass()) && BP->UbergraphPages.Contains(G))
    {
        if (Op == TEXT("add_branch")) return Only(E, {TEXT("operation"), TEXT("graph_id"), TEXT("x"), TEXT("y")}) && Position(E) && G->Nodes.Num() < 2048;
        if (Op == TEXT("add_event"))
        {
            FString Name; if (!Only(E, {TEXT("operation"), TEXT("graph_id"), TEXT("event"), TEXT("x"), TEXT("y")}) || !Position(E) || !Text(E, TEXT("event"), Name) || !EventName(Name) || G->Nodes.Num() >= 2048) return false;
            TArray<UEdGraph*> Graphs; BP->GetAllGraphs(Graphs);
            for (UEdGraph* Page : Graphs) for (UEdGraphNode* N : Page->Nodes) if (auto* Existing = Cast<UK2Node_Event>(N)) if (Existing->EventReference.GetMemberName() == FName(*Name)) { Reason = TEXT("This event already exists; inspect and use its current pins."); return false; }
            return true;
        }
        if (Op == TEXT("add_actor_call"))
        {
            FString Name; if (!Only(E, {TEXT("operation"), TEXT("graph_id"), TEXT("function"), TEXT("x"), TEXT("y"), TEXT("location")}) || !Position(E) || !Text(E, TEXT("function"), Name) || !ActorFunction(Name) || G->Nodes.Num() >= 2048) return false;
            if (Name != TEXT("K2_SetActorRelativeLocation")) return !E->HasField(TEXT("location"));
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr; if (!E->TryGetArrayField(TEXT("location"), Values) || Values->Num() != 3) return false;
            for (const auto& V : *Values) { double Number; if (!V || V->Type != EJson::Number || !V->TryGetNumber(Number) || !FMath::IsFinite(Number) || FMath::Abs(Number) > 1000000) return false; }
            return true;
        }
        if (Op == TEXT("add_variable"))
        {
            FString Name, Type; if (!Only(E, {TEXT("operation"), TEXT("graph_id"), TEXT("name"), TEXT("type")}) || !Text(E, TEXT("name"), Name) || !VariableName(Name) || !Text(E, TEXT("type"), Type) || (Type != TEXT("bool") && Type != TEXT("int") && Type != TEXT("double")) || BP->NewVariables.Num() >= 128) return false;
            const FName Key(*Name); return !BP->NewVariables.ContainsByPredicate([&](const FBPVariableDescription& V) { return V.VarName == Key; }) && !(BP->SkeletonGeneratedClass && FindFProperty<FProperty>(BP->SkeletonGeneratedClass, Key)) && !(BP->ParentClass && BP->ParentClass->FindFunctionByName(Key));
        }
        if (Op == TEXT("add_variable_get") || Op == TEXT("add_variable_set"))
        {
            FString Name; return Only(E, {TEXT("operation"), TEXT("graph_id"), TEXT("name"), TEXT("x"), TEXT("y")}) && Text(E, TEXT("name"), Name) && LocalVariable(BP, FName(*Name)) && Position(E) && G->Nodes.Num() < 2048;
        }
    }
    if (Op != TEXT("connect") && Op != TEXT("disconnect")) return false;
    if (!Only(E, {TEXT("operation"), TEXT("graph_id"), TEXT("output_node_id"), TEXT("output_pin_id"), TEXT("input_node_id"), TEXT("input_pin_id")})) return false;
    UEdGraphPin* Output = nullptr; UEdGraphPin* Input = nullptr; if (!Endpoints(G, E, Output, Input, bGameplay)) return false;
    if (Op == TEXT("disconnect")) return Output->LinkedTo.Contains(Input) && Input->LinkedTo.Contains(Output);
    if (!Input->LinkedTo.IsEmpty() || Output->LinkedTo.Contains(Input) || WouldCycle(Output, Input)) { Reason = TEXT("Connections cannot replace existing inputs or introduce graph cycles."); return false; }
    const auto Response = G->GetSchema()->CanCreateConnection(Output, Input);
    Reason = TEXT("Only exact-type schema connections without conversion nodes or implicit link replacement are allowed.");
    return Response.Response == CONNECT_RESPONSE_MAKE;
}

bool Apply(UBlueprint* BP, const TSharedPtr<FJsonObject>& E, const FGuid& AddedNodeId, bool bGameplay)
{
    FString Reason; if (!Validate(BP, E, Reason, bGameplay)) return false;
    UEdGraph* G = Graph(BP, E->GetStringField(TEXT("graph_id"))); const FString Op = E->GetStringField(TEXT("operation")); BP->Modify(); G->Modify();
    if (Op == TEXT("add_math_node"))
    {
        auto* N = NewObject<UK2Node_CallFunction>(G, NAME_None, RF_Transactional); N->NodeGuid = AddedNodeId; N->SetFromFunction(Function(E->GetStringField(TEXT("function")))); N->NodePosX = E->GetIntegerField(TEXT("x")); N->NodePosY = E->GetIntegerField(TEXT("y")); N->AllocateDefaultPins(); G->AddNode(N, false, false);
        FBlueprintEditorUtils::MarkBlueprintAsModified(BP); return G->Nodes.Contains(N);
    }
    if (Op == TEXT("remove_math_node"))
    {
        auto* N = Node(G, E->GetStringField(TEXT("node_id"))); N->Modify(); FBlueprintEditorUtils::RemoveNode(BP, N, true); FBlueprintEditorUtils::MarkBlueprintAsModified(BP); return !G->Nodes.Contains(N);
    }
    if (Op == TEXT("add_variable"))
    {
        FEdGraphPinType Type; const FString Name = E->GetStringField(TEXT("type")); Type.PinCategory = Name == TEXT("bool") ? UEdGraphSchema_K2::PC_Boolean : Name == TEXT("int") ? UEdGraphSchema_K2::PC_Int : UEdGraphSchema_K2::PC_Real;
        if (Name == TEXT("double")) Type.PinSubCategory = UEdGraphSchema_K2::PC_Double;
        return FBlueprintEditorUtils::AddMemberVariable(BP, FName(*E->GetStringField(TEXT("name"))), Type);
    }
    if (Op == TEXT("add_event") || Op == TEXT("add_branch") || Op == TEXT("add_actor_call") || Op == TEXT("add_variable_get") || Op == TEXT("add_variable_set"))
    {
        UEdGraphNode* N = nullptr;
        if (Op == TEXT("add_event")) { auto* Event = NewObject<UK2Node_Event>(G, NAME_None, RF_Transactional); Event->EventReference.SetExternalMember(FName(*E->GetStringField(TEXT("event"))), AActor::StaticClass()); Event->bOverrideFunction = true; N = Event; }
        else if (Op == TEXT("add_branch")) N = NewObject<UK2Node_IfThenElse>(G, NAME_None, RF_Transactional);
        else if (Op == TEXT("add_actor_call")) { auto* Call = NewObject<UK2Node_CallFunction>(G, NAME_None, RF_Transactional); Call->SetFromFunction(ActorFunction(E->GetStringField(TEXT("function")))); N = Call; }
        else { UK2Node_Variable* V = Op == TEXT("add_variable_get") ? static_cast<UK2Node_Variable*>(NewObject<UK2Node_VariableGet>(G, NAME_None, RF_Transactional)) : static_cast<UK2Node_Variable*>(NewObject<UK2Node_VariableSet>(G, NAME_None, RF_Transactional)); V->VariableReference.SetSelfMember(FName(*E->GetStringField(TEXT("name")))); N = V; }
        N->NodeGuid = AddedNodeId; N->NodePosX = E->GetIntegerField(TEXT("x")); N->NodePosY = E->GetIntegerField(TEXT("y")); N->AllocateDefaultPins(); G->AddNode(N, false, false);
        if (Op == TEXT("add_actor_call") && E->HasField(TEXT("location")))
        {
            const auto& V = E->GetArrayField(TEXT("location")); const FVector Location(V[0]->AsNumber(), V[1]->AsNumber(), V[2]->AsNumber()); UEdGraphPin* P = N->FindPin(TEXT("NewRelativeLocation"));
            if (!P) return false; G->GetSchema()->TrySetDefaultValue(*P, FString::Printf(TEXT("%f,%f,%f"), Location.X, Location.Y, Location.Z));
        }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP); return G->Nodes.Contains(N);
    }
    UEdGraphPin* Output = nullptr; UEdGraphPin* Input = nullptr; if (!Endpoints(G, E, Output, Input, bGameplay)) return false;
    Output->GetOwningNode()->Modify(); Input->GetOwningNode()->Modify();
    if (Op == TEXT("connect")) { if (!G->GetSchema()->TryCreateConnection(Output, Input)) return false; }
    else G->GetSchema()->BreakSinglePinLink(Output, Input);
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
    return Output->LinkedTo.Contains(Input) == (Op == TEXT("connect")) && Input->LinkedTo.Contains(Output) == (Op == TEXT("connect"));
}
}
