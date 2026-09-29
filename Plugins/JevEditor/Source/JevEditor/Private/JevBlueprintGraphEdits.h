#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UEdGraph;

/** Closed graph vocabulary. All work is performed on the editor game thread. */
namespace JevBlueprintGraph
{
UEdGraph* Graph(UBlueprint* Blueprint, const FString& Id);
bool Snapshot(UEdGraph* Graph, FString& Out);
bool Validate(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Edit, FString& Reason);
bool Apply(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Edit, const FGuid& AddedNodeId);
}
