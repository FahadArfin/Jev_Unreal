#include "JevBlueprintGraphEdits.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
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
UK2Node_CallFunction* Node(UEdGraph* G, const FString& Id)
{
    for (UEdGraphNode* N : G->Nodes)
    {
        if (!N || N->NodeGuid.ToString() != Id || N->GetClass() != UK2Node_CallFunction::StaticClass()) continue;
        auto* Call = CastChecked<UK2Node_CallFunction>(N);
        const UFunction* F = Call->GetTargetFunction();
        return F && Function(F->GetName()) == F && N->Pins.Num() <= 16 ? Call : nullptr;
    }
    return nullptr;
}
UEdGraphPin* Pin(UEdGraph* G, const FString& NodeId, const FString& PinId, EEdGraphPinDirection Direction)
{
    auto* N = Node(G, NodeId);
    if (!N) return nullptr;
    for (UEdGraphPin* P : N->Pins)
        if (P && P->PinId.ToString() == PinId && P->Direction == Direction && !P->bOrphanedPin && !P->ParentPin && P->SubPins.IsEmpty() && !P->PinType.IsContainer() && !P->PinType.bIsReference &&
            (P->PinType.PinCategory == UEdGraphSchema_K2::PC_Int || P->PinType.PinCategory == UEdGraphSchema_K2::PC_Real || P->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)) return P;
    return nullptr;
}
bool Endpoints(UEdGraph* G, const TSharedPtr<FJsonObject>& E, UEdGraphPin*& Output, UEdGraphPin*& Input)
{
    FString OutputNodeId, OutputPinId, InputNodeId, InputPinId;
    if (!Text(E, TEXT("output_node_id"), OutputNodeId) || !Text(E, TEXT("output_pin_id"), OutputPinId) || !Text(E, TEXT("input_node_id"), InputNodeId) || !Text(E, TEXT("input_pin_id"), InputPinId)) return false;
    Output = Pin(G, OutputNodeId, OutputPinId, EGPD_Output); Input = Pin(G, InputNodeId, InputPinId, EGPD_Input);
    return Output && Input && Output->GetOwningNode() != Input->GetOwningNode() && Output->PinType == Input->PinType;
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
    R->SetArrayField(TEXT("nodes"), Nodes); return FJsonSerializer::Serialize(R, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));
}

bool Validate(UBlueprint* BP, const TSharedPtr<FJsonObject>& E, FString& Reason)
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
    if (Op != TEXT("connect") && Op != TEXT("disconnect")) return false;
    if (!Only(E, {TEXT("operation"), TEXT("graph_id"), TEXT("output_node_id"), TEXT("output_pin_id"), TEXT("input_node_id"), TEXT("input_pin_id")})) return false;
    UEdGraphPin* Output = nullptr; UEdGraphPin* Input = nullptr; if (!Endpoints(G, E, Output, Input)) return false;
    if (Op == TEXT("disconnect")) return Output->LinkedTo.Contains(Input) && Input->LinkedTo.Contains(Output);
    if (!Input->LinkedTo.IsEmpty() || Output->LinkedTo.Contains(Input) || WouldCycle(Output, Input)) { Reason = TEXT("Connections cannot replace existing inputs or introduce graph cycles."); return false; }
    const auto Response = G->GetSchema()->CanCreateConnection(Output, Input);
    Reason = TEXT("Only exact-type schema connections without conversion nodes or implicit link replacement are allowed.");
    return Response.Response == CONNECT_RESPONSE_MAKE;
}

bool Apply(UBlueprint* BP, const TSharedPtr<FJsonObject>& E, const FGuid& AddedNodeId)
{
    FString Reason; if (!Validate(BP, E, Reason)) return false;
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
    UEdGraphPin* Output = nullptr; UEdGraphPin* Input = nullptr; if (!Endpoints(G, E, Output, Input)) return false;
    Output->GetOwningNode()->Modify(); Input->GetOwningNode()->Modify();
    if (Op == TEXT("connect")) { if (!G->GetSchema()->TryCreateConnection(Output, Input)) return false; }
    else G->GetSchema()->BreakSinglePinLink(Output, Input);
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
    return Output->LinkedTo.Contains(Input) == (Op == TEXT("connect")) && Input->LinkedTo.Contains(Output) == (Op == TEXT("connect"));
}
}
