#include "JevEditorReviewPresentation.h"

#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "JevReviewPresentation"

namespace JevPresentation
{
using FObject = TSharedPtr<FJsonObject>;

FText Literal(const FString& Value)
{
    // Keep data visibly distinct from the surrounding review text. Escape line
    // breaks rather than allowing actor names to impersonate additional rows.
    FString Safe = Value.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\r"), TEXT("\\r")).Replace(TEXT("\n"), TEXT("\\n")).Replace(TEXT("\t"), TEXT("\\t"));
    return FText::FromString(Safe);
}

bool String(const FObject& Object, const TCHAR* Key, FString& Out, bool bEmpty = false)
{
    return Object && Object->HasTypedField<EJson::String>(Key) && Object->TryGetStringField(Key, Out) && Out.Len() <= 4096 && (bEmpty || !Out.IsEmpty());
}

FString String(const FObject& Object, const TCHAR* Key)
{
    FString Out;
    if (Object) Object->TryGetStringField(Key, Out);
    return Out;
}

bool Object(const FObject& Parent, const TCHAR* Key, FObject& Out)
{
    const FObject* Value = nullptr;
    if (!Parent || !Parent->TryGetObjectField(Key, Value) || !Value || !Value->IsValid()) return false;
    Out = *Value;
    return true;
}

bool Integer(const FObject& Object, const TCHAR* Key, int32& Out, int32 Max = 64)
{
    double Value = 0;
    if (!Object || !Object->HasTypedField<EJson::Number>(Key) || !Object->TryGetNumberField(Key, Value) ||
        !FMath::IsFinite(Value) || Value < 0 || Value > Max || FMath::FloorToDouble(Value) != Value) return false;
    Out = static_cast<int32>(Value);
    return true;
}

bool Vector(const FObject& Object, const TCHAR* Key)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Object || !Object->TryGetArrayField(Key, Values) || Values->Num() != 3) return false;
    for (const auto& Value : *Values)
    {
        double Number = 0;
        if (!Value || Value->Type != EJson::Number || !Value->TryGetNumber(Number) || !FMath::IsFinite(Number)) return false;
    }
    return true;
}

FString Number(double Value)
{
    // 17 significant digits preserve the actual double rather than rounding a
    // small proposed movement away. Integral coordinates retain familiar .00.
    FString Out = FString::Printf(TEXT("%.17g"), Value);
    if (!Out.Contains(TEXT(".")) && !Out.Contains(TEXT("e")) && !Out.Contains(TEXT("E"))) Out += TEXT(".00");
    return Out;
}

FText VectorText(const FObject& Object, const TCHAR* Key)
{
    const auto& Values = Object->GetArrayField(Key);
    return FText::Format(LOCTEXT("Vector", "({0}, {1}, {2})"), Literal(Number(Values[0]->AsNumber())), Literal(Number(Values[1]->AsNumber())), Literal(Number(Values[2]->AsNumber())));
}

bool Transforms(const FObject& Object)
{
    return Vector(Object, TEXT("location")) && Vector(Object, TEXT("rotation")) && Vector(Object, TEXT("scale"));
}

bool Same(const FObject& Before, const FObject& After, const TCHAR* Field)
{
    const auto Left = Before->TryGetField(Field), Right = After->TryGetField(Field);
    return Left && Right && FJsonValue::CompareEqual(*Left, *Right);
}

bool SameTransform(const FObject& Before, const FObject& After)
{
    return Same(Before, After, TEXT("location")) && Same(Before, After, TEXT("rotation")) && Same(Before, After, TEXT("scale"));
}

bool KnownOperationFields(const FObject& Operation, const FString& Op)
{
    TSet<FString> Fields = {TEXT("op"), TEXT("location"), TEXT("rotation"), TEXT("scale")};
    if (Op.StartsWith(TEXT("spawn_")))
    {
        Fields.Add(TEXT("label"));
        Fields.Add(Op == TEXT("spawn_primitive") ? TEXT("shape") : TEXT("asset_path"));
    }
    else
    {
        Fields.Add(TEXT("actor_path"));
        if (Op == TEXT("set_material")) { Fields.Add(TEXT("slot")); Fields.Add(TEXT("material_path")); }
        if (Op == TEXT("set_metadata")) { Fields.Add(TEXT("label")); Fields.Add(TEXT("folder")); }
        if (Op == TEXT("replace_mesh") || Op == TEXT("duplicate_mesh"))
        {
            for (const TCHAR* Field : {TEXT("source_instance_id"), TEXT("asset_path"), TEXT("label"), TEXT("folder"), TEXT("mesh_settings"), TEXT("material_slot_count"), TEXT("material_override_count"), TEXT("materials"), TEXT("mesh_review")}) Fields.Add(Field);
            Fields.Add(Op == TEXT("replace_mesh") ? TEXT("material_policy") : TEXT("source_actor_path"));
        }
    }
    for (const auto& Pair : Operation->Values) if (!Fields.Contains(FString(*Pair.Key))) return false;
    return true;
}

bool Baseline(const FObject& Before, const FString& Path)
{
    FString Value;
    return String(Before, TEXT("path"), Value) && Value == Path && String(Before, TEXT("instance_id"), Value) &&
        String(Before, TEXT("label"), Value, true) && String(Before, TEXT("folder"), Value, true) && Transforms(Before);
}

// A defensive bound before traversing or serializing retained JSON, including
// depth/cycle protection. Unknown fields never become executable instructions.
bool Bounded(const TSharedPtr<FJsonValue>& Value, int32 Depth, int32& Remaining)
{
    if (!Value || Depth > 9 || --Remaining < 0) return false;
    switch (Value->Type)
    {
    case EJson::Null: case EJson::Boolean: return true;
    case EJson::Number: return FMath::IsFinite(Value->AsNumber());
    case EJson::String:
        Remaining -= Value->AsString().Len();
        return Value->AsString().Len() <= 4096 && Remaining >= 0;
    case EJson::Array:
        if (Value->AsArray().Num() > 64) return false;
        for (const auto& Child : Value->AsArray()) if (!Bounded(Child, Depth + 1, Remaining)) return false;
        return true;
    case EJson::Object:
        if (!Value->AsObject() || Value->AsObject()->Values.Num() > 64) return false;
        for (const auto& Pair : Value->AsObject()->Values)
        {
            Remaining -= Pair.Key.Len();
            if (Pair.Key.Len() > 128 || Remaining < 0 || !Bounded(Pair.Value, Depth + 1, Remaining)) return false;
        }
        return true;
    default: return false;
    }
}

TSharedPtr<FJsonValue> Sorted(const TSharedPtr<FJsonValue>& Value)
{
    if (Value->Type == EJson::Object)
    {
        auto Out = MakeShared<FJsonObject>();
        TArray<FString> Keys;
        for (const auto& Pair : Value->AsObject()->Values) Keys.Add(FString(*Pair.Key));
        Keys.Sort();
        for (const auto& Key : Keys) Out->SetField(Key, Sorted(Value->AsObject()->TryGetField(Key)));
        return MakeShared<FJsonValueObject>(Out);
    }
    if (Value->Type == EJson::Array)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const auto& Child : Value->AsArray()) Out.Add(Sorted(Child));
        return MakeShared<FJsonValueArray>(Out);
    }
    return Value;
}

void Line(FString& Into, const FText& Value) { Into += Value.ToString() + TEXT("\n"); }

void Row(FString& Into, const FText& Field, const FText& Before, const FText& After)
{
    Line(Into, FText::Format(LOCTEXT("ReviewRow", "{0}\n  Before: {1}\n  After: {2}"), Field, Before, After));
}

FText Folder(const FObject& Object)
{
    const auto Value = String(Object, TEXT("folder"));
    return Value.IsEmpty() ? LOCTEXT("RootFolder", "(root folder)") : Literal(Value);
}

FText NullablePath(const FObject& Object, const TCHAR* Field, bool bOverride = false)
{
    const auto Value = Object->TryGetField(Field);
    return Value->Type == EJson::Null ? (bOverride ? LOCTEXT("DefaultMaterial", "(no explicit override; mesh default)") : LOCTEXT("NoMaterial", "(no material)")) : Literal(Value->AsString());
}

bool NullablePathValid(const FObject& Object, const TCHAR* Field)
{
    const auto Value = Object ? Object->TryGetField(Field) : nullptr;
    return Value && (Value->Type == EJson::Null || (Value->Type == EJson::String && !Value->AsString().IsEmpty()));
}

bool MeshState(const FObject& State)
{
    int32 Slots = 0, Overrides = 0;
    FObject Settings;
    const TArray<TSharedPtr<FJsonValue>>* Materials = nullptr;
    if (!Integer(State, TEXT("material_slot_count"), Slots) || !Integer(State, TEXT("material_override_count"), Overrides) || Overrides > Slots ||
        !State->TryGetArrayField(TEXT("materials"), Materials) || Materials->Num() != Slots || !Object(State, TEXT("mesh_settings"), Settings)) return false;
    for (int32 Index = 0; Index < Slots; ++Index)
    {
        if (!(*Materials)[Index] || (*Materials)[Index]->Type != EJson::Object) return false;
        const FObject Material = (*Materials)[Index]->AsObject();
        int32 Slot = 0;
        if (!Integer(Material, TEXT("slot"), Slot, 63) || Slot != Index || !NullablePathValid(Material, TEXT("path")) || !NullablePathValid(Material, TEXT("override_path")) ||
            (Index >= Overrides && Material->TryGetField(TEXT("override_path"))->Type != EJson::Null)) return false;
    }
    FString Mobility, Profile;
    int32 Mode = 0, ObjectType = 0;
    if (!String(Settings, TEXT("mobility"), Mobility) || (Mobility != TEXT("Static") && Mobility != TEXT("Stationary") && Mobility != TEXT("Movable")) ||
        !String(Settings, TEXT("collision_profile"), Profile) || !Integer(Settings, TEXT("collision_mode"), Mode, 5) || !Integer(Settings, TEXT("collision_object_type"), ObjectType, 31)) return false;
    for (const TCHAR* Field : {TEXT("use_mesh_default_collision"), TEXT("actor_collision_enabled"), TEXT("cast_shadow"), TEXT("visible"), TEXT("hidden_in_game"), TEXT("actor_hidden_in_game"), TEXT("actor_hidden_in_editor"), TEXT("tags_truncated")})
        if (!Settings->HasTypedField<EJson::Boolean>(Field)) return false;
    if (Settings->GetBoolField(TEXT("tags_truncated"))) return false;
    const TArray<TSharedPtr<FJsonValue>>* Responses = nullptr;
    if (!Settings->TryGetArrayField(TEXT("collision_responses"), Responses) || Responses->Num() != 32) return false;
    for (const auto& Value : *Responses)
        if (!Value || Value->Type != EJson::Number || Value->AsNumber() < 0 || Value->AsNumber() > 2 || FMath::FloorToDouble(Value->AsNumber()) != Value->AsNumber()) return false;
    for (const TCHAR* Field : {TEXT("actor_tags"), TEXT("component_tags")})
    {
        const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr;
        if (!Settings->TryGetArrayField(Field, Tags) || Tags->Num() > 32) return false;
        for (const auto& Tag : *Tags) if (!Tag || Tag->Type != EJson::String || Tag->AsString().Len() > 128) return false;
    }
    return true;
}

bool MeshAsset(const FObject& Asset)
{
    FString Value;
    int32 Count = 0;
    const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
    if (!String(Asset, TEXT("path"), Value) || !String(Asset, TEXT("instance_id"), Value) || !String(Asset, TEXT("lighting_guid"), Value) ||
        !Vector(Asset, TEXT("local_bounds_center_cm")) || !Vector(Asset, TEXT("local_bounds_extent_cm")) ||
        !Integer(Asset, TEXT("material_slot_count"), Count) || !Asset->TryGetArrayField(TEXT("slots"), Slots) || Slots->Num() != Count ||
        !String(Asset, TEXT("body_instance_id"), Value) || !String(Asset, TEXT("pivot_semantics"), Value)) return false;
    for (const auto& Slot : *Slots)
    {
        if (!Slot || Slot->Type != EJson::Object || !String(Slot->AsObject(), TEXT("slot_name"), Value, true) ||
            !String(Slot->AsObject(), TEXT("imported_slot_name"), Value, true) || !String(Slot->AsObject(), TEXT("material_fingerprint"), Value)) return false;
    }
    // Meshes without a body setup legitimately omit these four fields. Partial
    // body information, however, cannot be presented as a complete review.
    const bool bAnyBody = Asset->HasField(TEXT("body_guid")) || Asset->HasField(TEXT("collision_trace_mode")) || Asset->HasField(TEXT("simple_collision_shapes")) || Asset->HasField(TEXT("default_collision"));
    if (bAnyBody)
    {
        FObject Defaults;
        int32 Ignored = 0;
        const TArray<TSharedPtr<FJsonValue>>* Responses = nullptr;
        if (!String(Asset, TEXT("body_guid"), Value) || !Integer(Asset, TEXT("collision_trace_mode"), Ignored, 3) || !Integer(Asset, TEXT("simple_collision_shapes"), Ignored, MAX_int32) ||
            !Object(Asset, TEXT("default_collision"), Defaults) || !String(Defaults, TEXT("profile"), Value) || !Integer(Defaults, TEXT("mode"), Ignored, 5) ||
            !Integer(Defaults, TEXT("object_type"), Ignored, 31) || !Defaults->TryGetArrayField(TEXT("responses"), Responses) || Responses->Num() != 32) return false;
        for (const auto& Response : *Responses) if (!Response || Response->Type != EJson::Number || Response->AsNumber() < 0 || Response->AsNumber() > 2 || FMath::FloorToDouble(Response->AsNumber()) != Response->AsNumber()) return false;
    }
    return true;
}

FText CollisionMode(int32 Mode)
{
    switch (Mode)
    {
    case 0: return LOCTEXT("CollisionNone", "No collision");
    case 1: return LOCTEXT("CollisionQuery", "Query only");
    case 2: return LOCTEXT("CollisionPhysics", "Physics only");
    case 3: return LOCTEXT("CollisionBoth", "Query and physics");
    case 4: return LOCTEXT("CollisionProbe", "Probe only");
    case 5: return LOCTEXT("CollisionQueryProbe", "Query and probe");
    default: return LOCTEXT("CollisionUnknown", "Unknown");
    }
}

FText Bool(const FObject& Object, const TCHAR* Field)
{
    return Object->GetBoolField(Field) ? LOCTEXT("Enabled", "Enabled") : LOCTEXT("Disabled", "Disabled");
}

void TransformRows(FString& Text, const FObject& Before, const FObject& After)
{
    const FText Absent = LOCTEXT("ActorAbsent", "(actor does not exist yet)");
    Row(Text, LOCTEXT("Location", "Location (X, Y, Z), cm"), Before ? VectorText(Before, TEXT("location")) : Absent, VectorText(After, TEXT("location")));
    Row(Text, LOCTEXT("Rotation", "Rotation (pitch, yaw, roll), degrees"), Before ? VectorText(Before, TEXT("rotation")) : Absent, VectorText(After, TEXT("rotation")));
    Row(Text, LOCTEXT("Scale", "Scale (X, Y, Z), multiplier"), Before ? VectorText(Before, TEXT("scale")) : Absent, VectorText(After, TEXT("scale")));
}

FJevReviewPresentation Invalid()
{
    FJevReviewPresentation Out;
    Out.Summary = LOCTEXT("InvalidSummary", "Plan review unavailable");
    Out.Body = LOCTEXT("InvalidBody", "This record is incomplete, unsupported or exceeds the review limits. Apply is unavailable. Refresh the plan or create a smaller preview; do not infer missing changes.");
    return Out;
}
}

FJevReviewPresentation FJevEditorReviewPresentation::Build(const TSharedPtr<FJsonObject>& Record)
{
    using namespace JevPresentation;
    if (!Record)
    {
        FJevReviewPresentation Empty;
        Empty.Summary = LOCTEXT("NoReviewSummary", "No plan selected");
        Empty.Body = LOCTEXT("NoReviewBody", "Choose a pending plan, or preview an edit to the current selection. Creating or opening a preview does not apply changes.");
        Empty.TechnicalDetails = LOCTEXT("NoReviewDetails", "Select a plan to inspect its complete reviewed details.");
        return Empty;
    }
    FObject Review;
    FString PlanId, Project, World, Revision, Status;
    double Expiry = 0;
    if (!String(Record, TEXT("plan_id"), PlanId) || !String(Record, TEXT("status"), Status) || !Record->HasTypedField<EJson::Number>(TEXT("expires_in_seconds")) ||
        !Record->TryGetNumberField(TEXT("expires_in_seconds"), Expiry) || !FMath::IsFinite(Expiry) || Expiry < 0 || !Object(Record, TEXT("review"), Review)) return Invalid();
    if (Status != TEXT("pending") && Status != TEXT("applying") && Status != TEXT("applied") && Status != TEXT("rejected") && Status != TEXT("expired") && Status != TEXT("rolled_back") && Status != TEXT("unknown")) return Invalid();
    int32 Budget = 131072;
    if (!Bounded(MakeShared<FJsonValueObject>(Review), 0, Budget) || !String(Review, TEXT("project_file"), Project) ||
        !String(Review, TEXT("world_path"), World) || !String(Review, TEXT("revision"), Revision)) return Invalid();
    const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Baselines = nullptr;
    if (!Review->TryGetArrayField(TEXT("operations"), Operations) || Operations->IsEmpty() || Operations->Num() > 20 ||
        !Review->TryGetArrayField(TEXT("before"), Baselines) || Baselines->Num() != Operations->Num()) return Invalid();

    FString Body;
    TArray<TSharedPtr<FJsonValue>> TechnicalOperations;
    for (int32 Index = 0; Index < Operations->Num(); ++Index)
    {
        const auto Value = (*Operations)[Index];
        const auto BaselineValue = (*Baselines)[Index];
        if (!Value || Value->Type != EJson::Object || !BaselineValue) return Invalid();
        const FObject Operation = Value->AsObject();
        FObject Before = BaselineValue->Type == EJson::Object ? BaselineValue->AsObject() : nullptr;
        FString Op, ActorPath, Text;
        if (!String(Operation, TEXT("op"), Op) || !Transforms(Operation)) return Invalid();
        const bool bSpawn = Op == TEXT("spawn_primitive") || Op == TEXT("spawn_static_mesh");
        const bool bMesh = Op == TEXT("replace_mesh") || Op == TEXT("duplicate_mesh");
        if ((!bSpawn && !bMesh && Op != TEXT("set_transform") && Op != TEXT("set_metadata") && Op != TEXT("set_material")) || !KnownOperationFields(Operation, Op)) return Invalid();
        if (bSpawn ? BaselineValue->Type != EJson::Null : (!String(Operation, TEXT("actor_path"), ActorPath) || !Baseline(Before, ActorPath))) return Invalid();
        const FString HeadingLabel = bSpawn ? String(Operation, TEXT("label")) : String(Before, TEXT("label"));
        Line(Body, FText::Format(LOCTEXT("OperationHeading", "{0}. {1}"), FText::AsNumber(Index + 1), Literal(HeadingLabel)));
        if (!bSpawn)
        {
            Line(Body, FText::Format(LOCTEXT("ActorPath", "Actor: {0}"), Literal(ActorPath)));
            Line(Body, FText::Format(LOCTEXT("ActorInstance", "Actor instance: {0}"), Literal(String(Before, TEXT("instance_id")))));
        }
        if (bSpawn)
        {
            if (!String(Operation, TEXT("label"), Text)) return Invalid();
            const FText Absent = LOCTEXT("ActorAbsent", "(actor does not exist yet)");
            Row(Body, LOCTEXT("Label", "Label"), Absent, Literal(Text));
            if (Op == TEXT("spawn_primitive"))
            {
                if (!String(Operation, TEXT("shape"), Text) || (Text != TEXT("Cube") && Text != TEXT("Sphere") && Text != TEXT("Cylinder") && Text != TEXT("Plane"))) return Invalid();
                Row(Body, LOCTEXT("Primitive", "Create primitive"), Absent, Literal(Text));
            }
            else
            {
                if (!String(Operation, TEXT("asset_path"), Text)) return Invalid();
                Row(Body, LOCTEXT("StaticMesh", "Static mesh"), Absent, Literal(Text));
            }
            Line(Body, LOCTEXT("SpawnIdentity", "Creates a new actor. Its actor path and instance identity are assigned during apply."));
            TransformRows(Body, nullptr, Operation);
        }
        else if (Op == TEXT("set_transform"))
        {
            Line(Body, LOCTEXT("TransformAction", "Change actor transform"));
            TransformRows(Body, Before, Operation);
        }
        else if (Op == TEXT("set_metadata"))
        {
            if ((!Operation->HasField(TEXT("label")) && !Operation->HasField(TEXT("folder"))) || !SameTransform(Before, Operation)) return Invalid();
            if (Operation->HasField(TEXT("label")))
            {
                if (!String(Operation, TEXT("label"), Text)) return Invalid();
                Row(Body, LOCTEXT("Label", "Label"), Literal(String(Before, TEXT("label"))), Literal(Text));
            }
            if (Operation->HasField(TEXT("folder")))
            {
                if (!String(Operation, TEXT("folder"), Text, true)) return Invalid();
                Row(Body, LOCTEXT("Folder", "Folder"), Folder(Before), Folder(Operation));
            }
            Line(Body, LOCTEXT("TransformUnchanged", "Transform unchanged."));
        }
        else if (Op == TEXT("set_material"))
        {
            int32 Slot = 0, BeforeSlot = 0;
            if (!Integer(Operation, TEXT("slot"), Slot, 63) || !Integer(Before, TEXT("slot"), BeforeSlot, 63) || Slot != BeforeSlot ||
                !String(Operation, TEXT("material_path"), Text) || !NullablePathValid(Before, TEXT("material_path")) || !SameTransform(Before, Operation)) return Invalid();
            Row(Body, FText::Format(LOCTEXT("MaterialSlot", "Material slot {0}"), FText::AsNumber(Slot)), NullablePath(Before, TEXT("material_path")), Literal(Text));
            Line(Body, LOCTEXT("MaterialAssignment", "After apply, this slot uses an explicit component material override. Other slots and the actor transform remain unchanged."));
        }
        else if (bMesh)
        {
            FObject MeshReview, SourceMesh, ResultMesh;
            FString SourceInstance, OldMesh, AssetPath;
            if (!String(Operation, TEXT("source_instance_id"), SourceInstance) || SourceInstance != String(Before, TEXT("instance_id")) ||
                !String(Before, TEXT("static_mesh_path"), OldMesh) || !String(Operation, TEXT("asset_path"), AssetPath) ||
                !String(Operation, TEXT("label"), Text) || !String(Operation, TEXT("folder"), Text, true) || !MeshState(Before) || !MeshState(Operation) ||
                !Object(Operation, TEXT("mesh_review"), MeshReview) || !Object(MeshReview, TEXT("source_mesh"), SourceMesh) || !Object(MeshReview, TEXT("result_mesh"), ResultMesh) ||
                !MeshAsset(SourceMesh) || !MeshAsset(ResultMesh) || String(SourceMesh, TEXT("path")) != OldMesh || String(ResultMesh, TEXT("path")) != AssetPath || !Vector(MeshReview, TEXT("actor_pivot_offset_cm")) ||
                !Same(Before, Operation, TEXT("folder")) || !Same(Before, Operation, TEXT("mesh_settings")) ||
                !Same(Before, SourceMesh, TEXT("material_slot_count")) || !Same(Operation, ResultMesh, TEXT("material_slot_count"))) return Invalid();
            for (const TCHAR* Field : {TEXT("material_assignment"), TEXT("collision_semantics"), TEXT("copy_scope")}) if (!String(MeshReview, Field, Text)) return Invalid();
            if (Op == TEXT("replace_mesh"))
            {
                FString Policy;
                if (!String(Operation, TEXT("material_policy"), Policy) || (Policy != TEXT("preserve_slots") && Policy != TEXT("mesh_defaults")) ||
                    !SameTransform(Before, Operation) || !Same(Before, Operation, TEXT("label")) ||
                    (Policy == TEXT("mesh_defaults") && Operation->GetIntegerField(TEXT("material_override_count")) != 0) ||
                    (Policy == TEXT("preserve_slots") && (!Same(Before, Operation, TEXT("material_slot_count")) || Operation->GetIntegerField(TEXT("material_override_count")) != Operation->GetIntegerField(TEXT("material_slot_count"))))) return Invalid();
                if (Policy == TEXT("preserve_slots"))
                {
                    const auto& OldMaterials = Before->GetArrayField(TEXT("materials"));
                    const auto& NewMaterials = Operation->GetArrayField(TEXT("materials"));
                    for (int32 Slot = 0; Slot < OldMaterials.Num(); ++Slot)
                    {
                        const FObject Old = OldMaterials[Slot]->AsObject(), New = NewMaterials[Slot]->AsObject();
                        if (!Same(Old, New, TEXT("path")) || !FJsonValue::CompareEqual(*Old->TryGetField(TEXT("path")), *New->TryGetField(TEXT("override_path")))) return Invalid();
                    }
                }
                Line(Body, LOCTEXT("ReplaceMesh", "Replace mesh:"));
                Row(Body, LOCTEXT("StaticMesh", "Static mesh"), Literal(OldMesh), Literal(AssetPath));
                Line(Body, FText::Format(LOCTEXT("MaterialPolicy", "Material policy: {0}"), Literal(Policy)));
                Line(Body, Policy == TEXT("preserve_slots") ? LOCTEXT("PreserveSlots", "Keep effective source materials as explicit overrides by equal slot index; slot names do not remap assignments.") : LOCTEXT("MeshDefaults", "Clear every component material override and use the replacement mesh's default materials."));
                Line(Body, LOCTEXT("ReplacementIdentity", "The actor identity, transform, label and folder remain. Geometry, local bounds and collision shapes follow the replacement mesh."));
            }
            else
            {
                if (!String(Operation, TEXT("source_actor_path"), Text) || Text != ActorPath || OldMesh != AssetPath || !Same(Before, Operation, TEXT("materials")) ||
                    !Same(Before, Operation, TEXT("material_override_count")) || !Same(SourceMesh, ResultMesh, TEXT("instance_id"))) return Invalid();
                Line(Body, FText::Format(LOCTEXT("DuplicateMesh", "Create controlled mesh copy: {0}"), Literal(String(Operation, TEXT("label")))));
                Line(Body, LOCTEXT("DuplicateIdentity", "Before values below describe the source. After values describe a new actor; the source stays unchanged. The new actor path and instance identity are assigned during apply."));
                Row(Body, LOCTEXT("StaticMesh", "Static mesh"), Literal(OldMesh), Literal(AssetPath));
                Row(Body, LOCTEXT("Label", "Label"), Literal(String(Before, TEXT("label"))), Literal(String(Operation, TEXT("label"))));
                TransformRows(Body, Before, Operation);
                Line(Body, LOCTEXT("DuplicateScope", "Only the reviewed native mesh settings are copied. Script state, attachments, extra components, physics simulation, per-instance paint and baked lighting are not cloned."));
            }
            Row(Body, LOCTEXT("Folder", "Folder"), Folder(Before), Folder(Operation));
            Row(Body, LOCTEXT("MeshBounds", "Mesh local bounds extent (X, Y, Z), cm"), VectorText(SourceMesh, TEXT("local_bounds_extent_cm")), VectorText(ResultMesh, TEXT("local_bounds_extent_cm")));
            Row(Body, LOCTEXT("MeshBoundsCenter", "Mesh local bounds center (X, Y, Z), cm"), VectorText(SourceMesh, TEXT("local_bounds_center_cm")), VectorText(ResultMesh, TEXT("local_bounds_center_cm")));
            Row(Body, LOCTEXT("MaterialCount", "Material slot count"), FText::AsNumber(Before->GetIntegerField(TEXT("material_slot_count"))), FText::AsNumber(Operation->GetIntegerField(TEXT("material_slot_count"))));
            Row(Body, LOCTEXT("OverrideCount", "Material override array length"), FText::AsNumber(Before->GetIntegerField(TEXT("material_override_count"))), FText::AsNumber(Operation->GetIntegerField(TEXT("material_override_count"))));
            const auto& BeforeMaterials = Before->GetArrayField(TEXT("materials"));
            const auto& AfterMaterials = Operation->GetArrayField(TEXT("materials"));
            for (int32 Slot = 0; Slot < FMath::Max(BeforeMaterials.Num(), AfterMaterials.Num()); ++Slot)
            {
                const FText Missing = LOCTEXT("MissingSlot", "(slot does not exist)");
                const FObject Old = BeforeMaterials.IsValidIndex(Slot) ? BeforeMaterials[Slot]->AsObject() : nullptr;
                const FObject New = AfterMaterials.IsValidIndex(Slot) ? AfterMaterials[Slot]->AsObject() : nullptr;
                Row(Body, FText::Format(LOCTEXT("EffectiveSlot", "Slot {0}: effective material"), FText::AsNumber(Slot)), Old ? NullablePath(Old, TEXT("path")) : Missing, New ? NullablePath(New, TEXT("path")) : Missing);
                Row(Body, FText::Format(LOCTEXT("OverrideSlot", "Slot {0}: explicit override"), FText::AsNumber(Slot)), Old ? NullablePath(Old, TEXT("override_path"), true) : Missing, New ? NullablePath(New, TEXT("override_path"), true) : Missing);
            }
            const FObject OldSettings = Before->GetObjectField(TEXT("mesh_settings")), NewSettings = Operation->GetObjectField(TEXT("mesh_settings"));
            Row(Body, LOCTEXT("Mobility", "Mobility"), Literal(String(OldSettings, TEXT("mobility"))), Literal(String(NewSettings, TEXT("mobility"))));
            Row(Body, LOCTEXT("CollisionProfile", "Collision profile"), Literal(String(OldSettings, TEXT("collision_profile"))), Literal(String(NewSettings, TEXT("collision_profile"))));
            Row(Body, LOCTEXT("CollisionMode", "Component collision mode"), CollisionMode(OldSettings->GetIntegerField(TEXT("collision_mode"))), CollisionMode(NewSettings->GetIntegerField(TEXT("collision_mode"))));
            Row(Body, LOCTEXT("CollisionInheritance", "Use mesh default collision"), Bool(OldSettings, TEXT("use_mesh_default_collision")), Bool(NewSettings, TEXT("use_mesh_default_collision")));
            Row(Body, LOCTEXT("ActorCollision", "Actor collision"), Bool(OldSettings, TEXT("actor_collision_enabled")), Bool(NewSettings, TEXT("actor_collision_enabled")));
            Row(Body, LOCTEXT("Visible", "Component visibility"), Bool(OldSettings, TEXT("visible")), Bool(NewSettings, TEXT("visible")));
            Row(Body, LOCTEXT("Shadow", "Cast shadow"), Bool(OldSettings, TEXT("cast_shadow")), Bool(NewSettings, TEXT("cast_shadow")));
            Line(Body, LOCTEXT("MeshLimitations", "No automatic pivot compensation. Collision overlap and visual fit require fresh inspection. Technical details retain all material slots, collision responses, tags, hidden flags and mesh metadata."));
            auto Detail = MakeShared<FJsonObject>();
            Detail->SetNumberField(TEXT("operation_number"), Index + 1);
            Detail->SetObjectField(TEXT("before"), Before);
            Detail->SetObjectField(TEXT("after"), Operation);
            TechnicalOperations.Add(MakeShared<FJsonValueObject>(Detail));
        }
        Body += TEXT("\n");
    }
    Line(Body, LOCTEXT("ExecutionBoundary", "Apply consumes this exact plan once. Scene changes or expiry reject it. Changes participate in Unreal Undo; this panel never saves the level. A receipt describes a past outcome, not current scene state. Fresh verification is required after applying."));
    if (Body.Len() > 262144) return Invalid();
    FJevReviewPresentation Out;
    Out.Summary = FText::Format(LOCTEXT("Summary", "{0} changes\nPlan: {1}\nProject: {2}\nWorld: {3}"), FText::AsNumber(Operations->Num()), Literal(PlanId), Literal(Project), Literal(World));
    Out.Body = FText::FromString(Body);
    Out.TechnicalDetails = LOCTEXT("NoMeshDetails", "This plan has no additional mesh details. All reviewed changes appear in the before/after review.");
    if (!TechnicalOperations.IsEmpty())
    {
        auto Details = MakeShared<FJsonObject>();
        Details->SetArrayField(TEXT("mesh_operations"), TechnicalOperations);
        FString Encoded;
        if (!FJsonSerializer::Serialize(Sorted(MakeShared<FJsonValueObject>(Details))->AsObject().ToSharedRef(), TJsonWriterFactory<>::Create(&Encoded)) || Encoded.Len() > 262144) return Invalid();
        Out.TechnicalDetails = FText::FromString(Encoded);
    }
    Out.bValid = true;
    return Out;
}

FText FJevEditorReviewPresentation::RecoveryMessage(const FString& Code, const FString& NativeMessage, bool bApplyAttempted)
{
    using namespace JevPresentation;
    const FText Details = FText::Format(LOCTEXT("NativeFailure", "{0}: {1}"), Literal(Code.Left(128)), Literal(NativeMessage.Left(4096)));
    FText Recovery;
    if (Code == TEXT("rollback_failed") || Code == TEXT("outcome_unknown") || Code == TEXT("unknown_outcome"))
        Recovery = LOCTEXT("UnknownRecovery", "The apply outcome is unknown. Inspect the scene and the retained plan receipt before making another edit. Do not assume that changes were undone.");
    else if (Code == TEXT("stale_plan") || Code == TEXT("expired_plan") || Code == TEXT("plan_expired"))
        Recovery = LOCTEXT("StaleRecovery", "Inspect the current actors and create a new preview. The previous review cannot authorize the new scene state.");
    else if (Code == TEXT("unknown_plan"))
        Recovery = LOCTEXT("MissingPlanRecovery", "Refresh the retained plan receipt and inspect the scene. The plan may have expired, been consumed or left editor memory; its absence alone does not prove whether an edit occurred.");
    else if (Code == TEXT("selection_empty"))
        Recovery = LOCTEXT("SelectionRecovery", "Select one to 20 actors in the World Outliner or viewport, then inspect the selection.");
    else if (Code == TEXT("actor_unsupported") || Code == TEXT("actor_locked") || Code == TEXT("selection_too_large"))
        Recovery = LOCTEXT("UnsupportedRecovery", "Inspect the selection and its edit blockers. Use at most 20 supported, unlocked native static mesh actors, then create a fresh preview.");
    else if (Code == TEXT("bad_request"))
        Recovery = LOCTEXT("InputRecovery", "Correct the indicated input, then create and review a new preview.");
    else
        Recovery = LOCTEXT("GeneralRecovery", "Read the diagnostic and inspect the current scene before creating a new preview.");
    return bApplyAttempted
        ? FText::Format(LOCTEXT("ApplyRecovery", "{0}\n{1}\nNo automatic retry was made."), Details, Recovery)
        : FText::Format(LOCTEXT("ReadRecovery", "{0}\n{1}"), Details, Recovery);
}

#undef LOCTEXT_NAMESPACE
