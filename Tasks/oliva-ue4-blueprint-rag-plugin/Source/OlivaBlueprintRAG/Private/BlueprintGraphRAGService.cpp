#include "BlueprintGraphRAGService.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Json.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Modules/ModuleManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SavePackage.h"

namespace
{
static const TCHAR* KeyBlueprint = TEXT("blueprint");
static const TCHAR* KeyGraphs = TEXT("graphs");
static const TCHAR* KeyGraphName = TEXT("name");
static const TCHAR* KeyGraphGuid = TEXT("graph_guid");
static const TCHAR* KeyGraphSchema = TEXT("schema_class");
static const TCHAR* KeyNodes = TEXT("nodes");
static const TCHAR* KeyLinks = TEXT("links");

static const TCHAR* KeyNodeGuid = TEXT("node_guid");
static const TCHAR* KeyNodeClass = TEXT("class_path");
static const TCHAR* KeyNodeTitle = TEXT("title");
static const TCHAR* KeyNodePosX = TEXT("pos_x");
static const TCHAR* KeyNodePosY = TEXT("pos_y");
static const TCHAR* KeyNodeComment = TEXT("comment");
static const TCHAR* KeyNodeFunction = TEXT("function");

static const TCHAR* KeyFunctionName = TEXT("name");
static const TCHAR* KeyFunctionOwner = TEXT("owner_class_path");

static const TCHAR* KeyPins = TEXT("pins");
static const TCHAR* KeyPinId = TEXT("pin_id");
static const TCHAR* KeyPinName = TEXT("name");
static const TCHAR* KeyPinDirection = TEXT("direction");
static const TCHAR* KeyPinTypeCat = TEXT("type_cat");
static const TCHAR* KeyPinTypeSub = TEXT("type_sub");
static const TCHAR* KeyPinTypeObj = TEXT("type_obj");
static const TCHAR* KeyPinDefaultValue = TEXT("default_value");
static const TCHAR* KeyPinDefaultObject = TEXT("default_object");
static const TCHAR* KeyPinDefaultText = TEXT("default_text");

static const TCHAR* KeyLinkFromNodeGuid = TEXT("from_node_guid");
static const TCHAR* KeyLinkFromPinId = TEXT("from_pin_id");
static const TCHAR* KeyLinkFromPinName = TEXT("from_pin_name");
static const TCHAR* KeyLinkToNodeGuid = TEXT("to_node_guid");
static const TCHAR* KeyLinkToPinId = TEXT("to_pin_id");
static const TCHAR* KeyLinkToPinName = TEXT("to_pin_name");

// ── Variable reference ──────────────────────────────────────────────
static const TCHAR* KeyVariableRef    = TEXT("variable_ref");
static const TCHAR* KeyMemberName     = TEXT("member_name");
static const TCHAR* KeyMemberParent   = TEXT("member_parent");
static const TCHAR* KeyMemberGuid     = TEXT("member_guid");
static const TCHAR* KeySelfContext    = TEXT("self_context");

// ── Event ───────────────────────────────────────────────────────────
static const TCHAR* KeyEventRef           = TEXT("event_ref");
static const TCHAR* KeyCustomFunctionName = TEXT("custom_function_name");
static const TCHAR* KeyOverrideFunction   = TEXT("override_function");

// ── Custom event ────────────────────────────────────────────────────
static const TCHAR* KeyCustomEvent         = TEXT("custom_event");
static const TCHAR* KeyCustomEventFuncName = TEXT("function_name");
static const TCHAR* KeyCustomEventFlags    = TEXT("function_flags");

// ── Macro instance ──────────────────────────────────────────────────
static const TCHAR* KeyMacroRef       = TEXT("macro_ref");
static const TCHAR* KeyMacroBlueprint = TEXT("macro_blueprint");
static const TCHAR* KeyMacroGraphGuid = TEXT("macro_graph_guid");

// ── Dynamic cast ────────────────────────────────────────────────────
static const TCHAR* KeyCastTarget = TEXT("cast_target");

// ── Component bound event ───────────────────────────────────────────
static const TCHAR* KeyComponentEvent     = TEXT("component_event");
static const TCHAR* KeyDelegatePropName   = TEXT("delegate_property_name");
static const TCHAR* KeyDelegateOwnerClass = TEXT("delegate_owner_class");
static const TCHAR* KeyComponentPropName  = TEXT("component_property_name");

// ── Blueprint variables ─────────────────────────────────────────────
static const TCHAR* KeyVariables         = TEXT("variables");
static const TCHAR* KeyVarName           = TEXT("var_name");
static const TCHAR* KeyVarGuid           = TEXT("var_guid");
static const TCHAR* KeyVarType           = TEXT("var_type");
static const TCHAR* KeyVarTypeCat        = TEXT("category");
static const TCHAR* KeyVarTypeSub        = TEXT("sub_category");
static const TCHAR* KeyVarTypeObj        = TEXT("sub_category_object");
static const TCHAR* KeyVarFriendlyName   = TEXT("friendly_name");
static const TCHAR* KeyVarCategory       = TEXT("var_category");
static const TCHAR* KeyVarPropertyFlags  = TEXT("property_flags");
static const TCHAR* KeyVarRepNotifyFunc  = TEXT("rep_notify_func");
static const TCHAR* KeyVarRepCondition   = TEXT("replication_condition");
static const TCHAR* KeyVarDefaultValue   = TEXT("default_value");
static const TCHAR* KeyVarMetaData       = TEXT("meta_data");

// ── Extended pin type ───────────────────────────────────────────────
static const TCHAR* KeyPinIsArray      = TEXT("is_array");
static const TCHAR* KeyPinIsReference  = TEXT("is_reference");
static const TCHAR* KeyPinIsConst      = TEXT("is_const");
static const TCHAR* KeyPinContainerType = TEXT("container_type");

static FString PinDirectionToString(const EEdGraphPinDirection Direction)
{
    switch (Direction)
    {
    case EGPD_Input:
        return TEXT("in");
    case EGPD_Output:
        return TEXT("out");
    default:
        return TEXT("unknown");
    }
}

static EEdGraphPinDirection PinDirectionFromString(const FString& Direction)
{
    if (Direction.Equals(TEXT("out"), ESearchCase::IgnoreCase))
    {
        return EGPD_Output;
    }
    return EGPD_Input;
}

static bool ParseGuidField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, FGuid& OutGuid)
{
    FString GuidString;
    if (!Object.IsValid() || !Object->TryGetStringField(Key, GuidString))
    {
        OutGuid.Invalidate();
        return false;
    }
    return FGuid::Parse(GuidString, OutGuid);
}

static UEdGraph* FindGraphByGuidOrName(UBlueprint* Blueprint, const FGuid& GraphGuid, const FString& GraphName)
{
    TArray<UEdGraph*> AllGraphs;
    Blueprint->GetAllGraphs(AllGraphs);

    if (GraphGuid.IsValid())
    {
        for (UEdGraph* Graph : AllGraphs)
        {
            if (Graph && Graph->GraphGuid == GraphGuid)
            {
                return Graph;
            }
        }
    }

    for (UEdGraph* Graph : AllGraphs)
    {
        if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::CaseSensitive))
        {
            return Graph;
        }
    }
    return nullptr;
}

static UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FGuid& NodeGuid)
{
    if (!Graph || !NodeGuid.IsValid())
    {
        return nullptr;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node && Node->NodeGuid == NodeGuid)
        {
            return Node;
        }
    }
    return nullptr;
}

static UEdGraphPin* FindPinByIdOrName(
    UEdGraphNode* Node,
    const FGuid& PinGuid,
    const FString& PinName,
    const EEdGraphPinDirection Direction)
{
    if (!Node)
    {
        return nullptr;
    }

    if (PinGuid.IsValid())
    {
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->PinId == PinGuid)
            {
                return Pin;
            }
        }
    }

    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin)
        {
            continue;
        }
        if (Pin->Direction == Direction && Pin->PinName.ToString().Equals(PinName, ESearchCase::CaseSensitive))
        {
            return Pin;
        }
    }

    return nullptr;
}

static bool TryLoadClassByPath(const FString& ClassPath, UClass*& OutClass)
{
    OutClass = nullptr;
    if (ClassPath.IsEmpty())
    {
        return false;
    }

    OutClass = FindObject<UClass>(nullptr, *ClassPath);
    if (!OutClass)
    {
        OutClass = LoadObject<UClass>(nullptr, *ClassPath);
    }
    return OutClass != nullptr;
}

static void ApplyPinDefaults(UEdGraphPin* Pin, const TSharedPtr<FJsonObject>& PinObject)
{
    if (!Pin || !PinObject.IsValid())
    {
        return;
    }

    FString DefaultValue;
    if (PinObject->TryGetStringField(KeyPinDefaultValue, DefaultValue))
    {
        Pin->DefaultValue = DefaultValue;
    }

    FString DefaultText;
    if (PinObject->TryGetStringField(KeyPinDefaultText, DefaultText))
    {
        Pin->DefaultTextValue = FText::FromString(DefaultText);
    }

    FString DefaultObjectPath;
    if (PinObject->TryGetStringField(KeyPinDefaultObject, DefaultObjectPath) && !DefaultObjectPath.IsEmpty())
    {
        UObject* LoadedObject = LoadObject<UObject>(nullptr, *DefaultObjectPath);
        Pin->DefaultObject = LoadedObject;
    }
}

// ═══════════════════════════════════════════════════════════════════
// FMemberReference helpers (reusable for Variable, Event, CallFunction)
// ═══════════════════════════════════════════════════════════════════

static TSharedRef<FJsonObject> ExportMemberReference(
    const FMemberReference& Ref,
    const UK2Node* ContextNode)
{
    TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(KeyMemberName, Ref.GetMemberName().ToString());
    UClass* ParentClass = Ref.GetMemberParentClass(
        ContextNode ? ContextNode->GetBlueprintClassFromNode() : nullptr);
    Obj->SetStringField(KeyMemberParent,
        ParentClass ? ParentClass->GetPathName() : FString());
    Obj->SetStringField(KeyMemberGuid, Ref.GetMemberGuid().ToString());
    Obj->SetBoolField(KeySelfContext, Ref.IsSelfContext());
    return Obj;
}

static bool ImportMemberReference(
    const TSharedPtr<FJsonObject>& Obj,
    FMemberReference& OutRef)
{
    if (!Obj.IsValid()) return false;

    FString MemberName, ParentPath;
    bool bSelfContext = false;
    Obj->TryGetStringField(KeyMemberName, MemberName);
    Obj->TryGetStringField(KeyMemberParent, ParentPath);
    Obj->TryGetBoolField(KeySelfContext, bSelfContext);

    if (MemberName.IsEmpty()) return false;

    if (bSelfContext)
    {
        OutRef.SetSelfMember(*MemberName);
    }
    else
    {
        UClass* ParentClass = nullptr;
        if (TryLoadClassByPath(ParentPath, ParentClass))
        {
            OutRef.SetExternalMember(*MemberName, ParentClass);
        }
        else
        {
            return false;
        }
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════
// Extended pin type helpers
// ═══════════════════════════════════════════════════════════════════

static void ExportPinTypeExtended(
    const FEdGraphPinType& PinType,
    TSharedRef<FJsonObject>& PinObject)
{
    PinObject->SetBoolField(KeyPinIsArray, PinType.IsArray());
    PinObject->SetBoolField(KeyPinIsReference, PinType.bIsReference);
    PinObject->SetBoolField(KeyPinIsConst, PinType.bIsConst);
    PinObject->SetStringField(KeyPinContainerType,
        PinType.IsArray() ? TEXT("Array")
        : PinType.IsSet()  ? TEXT("Set")
        : PinType.IsMap()   ? TEXT("Map")
        : TEXT("None"));
}

static void ImportPinTypeFromJson(
    UEdGraphPin* Pin,
    const TSharedPtr<FJsonObject>& PinObject)
{
    if (!Pin || !PinObject.IsValid()) return;

    FString TypeCat, TypeSub, TypeObj;
    if (PinObject->TryGetStringField(KeyPinTypeCat, TypeCat))
        Pin->PinType.PinCategory = *TypeCat;
    if (PinObject->TryGetStringField(KeyPinTypeSub, TypeSub))
        Pin->PinType.PinSubCategory = *TypeSub;
    if (PinObject->TryGetStringField(KeyPinTypeObj, TypeObj) && !TypeObj.IsEmpty())
        Pin->PinType.PinSubCategoryObject = LoadObject<UObject>(nullptr, *TypeObj);

    bool bVal = false;
    if (PinObject->TryGetBoolField(KeyPinIsArray, bVal) && bVal)
        Pin->PinType.ContainerType = EPinContainerType::Array;
    if (PinObject->TryGetBoolField(KeyPinIsReference, bVal))
        Pin->PinType.bIsReference = bVal;
    if (PinObject->TryGetBoolField(KeyPinIsConst, bVal))
        Pin->PinType.bIsConst = bVal;

    FString ContainerStr;
    if (PinObject->TryGetStringField(KeyPinContainerType, ContainerStr))
    {
        if (ContainerStr == TEXT("Array"))
            Pin->PinType.ContainerType = EPinContainerType::Array;
        else if (ContainerStr == TEXT("Set"))
            Pin->PinType.ContainerType = EPinContainerType::Set;
        else if (ContainerStr == TEXT("Map"))
            Pin->PinType.ContainerType = EPinContainerType::Map;
    }
}

// ═══════════════════════════════════════════════════════════════════
// FEdGraphPinType serialization for variables
// ═══════════════════════════════════════════════════════════════════

static TSharedRef<FJsonObject> ExportEdGraphPinType(const FEdGraphPinType& PinType)
{
    TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(KeyVarTypeCat, PinType.PinCategory.ToString());
    Obj->SetStringField(KeyVarTypeSub, PinType.PinSubCategory.ToString());
    Obj->SetStringField(KeyVarTypeObj,
        PinType.PinSubCategoryObject.IsValid()
            ? PinType.PinSubCategoryObject->GetPathName() : FString());
    Obj->SetBoolField(KeyPinIsArray, PinType.IsArray());
    Obj->SetBoolField(KeyPinIsReference, PinType.bIsReference);
    Obj->SetBoolField(KeyPinIsConst, PinType.bIsConst);
    return Obj;
}

static void ImportEdGraphPinType(
    const TSharedPtr<FJsonObject>& Obj,
    FEdGraphPinType& OutType)
{
    if (!Obj.IsValid()) return;

    FString Cat, Sub, SubObj;
    Obj->TryGetStringField(KeyVarTypeCat, Cat);
    Obj->TryGetStringField(KeyVarTypeSub, Sub);
    Obj->TryGetStringField(KeyVarTypeObj, SubObj);

    OutType.PinCategory = *Cat;
    OutType.PinSubCategory = *Sub;
    if (!SubObj.IsEmpty())
        OutType.PinSubCategoryObject = LoadObject<UObject>(nullptr, *SubObj);

    bool bArr = false;
    if (Obj->TryGetBoolField(KeyPinIsArray, bArr) && bArr)
        OutType.ContainerType = EPinContainerType::Array;

    bool bRef = false;
    if (Obj->TryGetBoolField(KeyPinIsReference, bRef))
        OutType.bIsReference = bRef;

    bool bConst = false;
    if (Obj->TryGetBoolField(KeyPinIsConst, bConst))
        OutType.bIsConst = bConst;
}

// ═══════════════════════════════════════════════════════════════════
// Node export/import handler signatures and dispatch
// ═══════════════════════════════════════════════════════════════════

using FNodeExportHandler = void(*)(const UEdGraphNode*, TSharedRef<FJsonObject>&);
using FNodeImportHandler = bool(*)(UEdGraphNode*, const TSharedPtr<FJsonObject>&, UBlueprint*);

// ── Export handlers ─────────────────────────────────────────────────

static void ExportNode_CallFunction(const UEdGraphNode* Node, TSharedRef<FJsonObject>& NodeObject)
{
    const UK2Node_CallFunction* Typed = CastChecked<UK2Node_CallFunction>(Node);
    NodeObject->SetObjectField(KeyNodeFunction,
        ExportMemberReference(Typed->FunctionReference, Typed));
}

static void ExportNode_Variable(const UEdGraphNode* Node, TSharedRef<FJsonObject>& NodeObject)
{
    const UK2Node_Variable* Typed = CastChecked<UK2Node_Variable>(Node);
    NodeObject->SetObjectField(KeyVariableRef,
        ExportMemberReference(Typed->VariableReference, Typed));
}

static void ExportNode_Event(const UEdGraphNode* Node, TSharedRef<FJsonObject>& NodeObject)
{
    const UK2Node_Event* Typed = CastChecked<UK2Node_Event>(Node);
    NodeObject->SetObjectField(KeyEventRef,
        ExportMemberReference(Typed->EventReference, Typed));
    NodeObject->SetStringField(KeyCustomFunctionName, Typed->CustomFunctionName.ToString());
    NodeObject->SetBoolField(KeyOverrideFunction, Typed->bOverrideFunction != 0);
}

static void ExportNode_CustomEvent(const UEdGraphNode* Node, TSharedRef<FJsonObject>& NodeObject)
{
    const UK2Node_CustomEvent* Typed = CastChecked<UK2Node_CustomEvent>(Node);
    TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(KeyCustomEventFuncName, Typed->CustomFunctionName.ToString());
    Obj->SetNumberField(KeyCustomEventFlags, static_cast<double>(Typed->FunctionFlags));
    NodeObject->SetObjectField(KeyCustomEvent, Obj);
    // Also export event_ref from parent class
    ExportNode_Event(Node, NodeObject);
}

static void ExportNode_MacroInstance(const UEdGraphNode* Node, TSharedRef<FJsonObject>& NodeObject)
{
    const UK2Node_MacroInstance* Typed = CastChecked<UK2Node_MacroInstance>(Node);
    TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
    UEdGraph* MacroGraph = Typed->GetMacroGraph();
    UBlueprint* MacroBP = MacroGraph ? Cast<UBlueprint>(MacroGraph->GetOuter()) : nullptr;
    Obj->SetStringField(KeyMacroBlueprint,
        MacroBP ? MacroBP->GetPathName() : FString());
    Obj->SetStringField(KeyMacroGraphGuid,
        MacroGraph ? MacroGraph->GraphGuid.ToString() : FString());
    NodeObject->SetObjectField(KeyMacroRef, Obj);
}

static void ExportNode_DynamicCast(const UEdGraphNode* Node, TSharedRef<FJsonObject>& NodeObject)
{
    const UK2Node_DynamicCast* Typed = CastChecked<UK2Node_DynamicCast>(Node);
    NodeObject->SetStringField(KeyCastTarget,
        Typed->TargetType ? Typed->TargetType->GetPathName() : FString());
}

static void ExportNode_ComponentBoundEvent(const UEdGraphNode* Node, TSharedRef<FJsonObject>& NodeObject)
{
    const UK2Node_ComponentBoundEvent* Typed = CastChecked<UK2Node_ComponentBoundEvent>(Node);
    TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(KeyDelegatePropName, Typed->DelegatePropertyName.ToString());
    Obj->SetStringField(KeyDelegateOwnerClass,
        Typed->DelegateOwnerClass ? Typed->DelegateOwnerClass->GetPathName() : FString());
    Obj->SetStringField(KeyComponentPropName, Typed->ComponentPropertyName.ToString());
    NodeObject->SetObjectField(KeyComponentEvent, Obj);
    // Also export event_ref from parent class
    ExportNode_Event(Node, NodeObject);
}

// ── Import handlers ─────────────────────────────────────────────────

static bool ImportNode_CallFunction(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObject, UBlueprint* Blueprint)
{
    UK2Node_CallFunction* Typed = CastChecked<UK2Node_CallFunction>(Node);
    const TSharedPtr<FJsonObject>* FuncObj = nullptr;
    if (!NodeObject->TryGetObjectField(KeyNodeFunction, FuncObj) || !FuncObj)
        return true;

    if (ImportMemberReference(*FuncObj, Typed->FunctionReference))
    {
        Typed->ReconstructNode();
        return true;
    }
    return false;
}

static bool ImportNode_Variable(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObject, UBlueprint* Blueprint)
{
    UK2Node_Variable* Typed = CastChecked<UK2Node_Variable>(Node);
    const TSharedPtr<FJsonObject>* RefObj = nullptr;
    if (!NodeObject->TryGetObjectField(KeyVariableRef, RefObj) || !RefObj)
        return true;

    if (ImportMemberReference(*RefObj, Typed->VariableReference))
    {
        Typed->ReconstructNode();
        return true;
    }
    return false;
}

static bool ImportNode_Event(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObject, UBlueprint* Blueprint)
{
    UK2Node_Event* Typed = CastChecked<UK2Node_Event>(Node);

    const TSharedPtr<FJsonObject>* RefObj = nullptr;
    if (NodeObject->TryGetObjectField(KeyEventRef, RefObj) && RefObj)
    {
        ImportMemberReference(*RefObj, Typed->EventReference);
    }

    FString CustomName;
    if (NodeObject->TryGetStringField(KeyCustomFunctionName, CustomName))
    {
        Typed->CustomFunctionName = *CustomName;
    }

    bool bOverride = false;
    if (NodeObject->TryGetBoolField(KeyOverrideFunction, bOverride))
    {
        Typed->bOverrideFunction = bOverride;
    }

    Typed->ReconstructNode();
    return true;
}

static bool ImportNode_CustomEvent(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObject, UBlueprint* Blueprint)
{
    UK2Node_CustomEvent* Typed = CastChecked<UK2Node_CustomEvent>(Node);

    const TSharedPtr<FJsonObject>* Obj = nullptr;
    if (NodeObject->TryGetObjectField(KeyCustomEvent, Obj) && Obj && (*Obj).IsValid())
    {
        FString FuncName;
        (*Obj)->TryGetStringField(KeyCustomEventFuncName, FuncName);
        if (!FuncName.IsEmpty())
        {
            Typed->CustomFunctionName = *FuncName;
        }

        double Flags = 0;
        if ((*Obj)->TryGetNumberField(KeyCustomEventFlags, Flags))
        {
            Typed->FunctionFlags = static_cast<uint32>(Flags);
        }
    }

    // Also restore event_ref from parent handler
    ImportNode_Event(Node, NodeObject, Blueprint);
    return true;
}

static bool ImportNode_MacroInstance(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObject, UBlueprint* Blueprint)
{
    UK2Node_MacroInstance* Typed = CastChecked<UK2Node_MacroInstance>(Node);

    const TSharedPtr<FJsonObject>* Obj = nullptr;
    if (!NodeObject->TryGetObjectField(KeyMacroRef, Obj) || !Obj)
        return true;

    FString MacroBPPath, MacroGuidStr;
    (*Obj)->TryGetStringField(KeyMacroBlueprint, MacroBPPath);
    (*Obj)->TryGetStringField(KeyMacroGraphGuid, MacroGuidStr);

    if (MacroBPPath.IsEmpty()) return false;

    UBlueprint* MacroBP = LoadObject<UBlueprint>(nullptr, *MacroBPPath);
    if (!MacroBP) return false;

    FGuid MacroGuid;
    FGuid::Parse(MacroGuidStr, MacroGuid);

    UEdGraph* MacroGraph = nullptr;
    TArray<UEdGraph*> AllGraphs;
    MacroBP->GetAllGraphs(AllGraphs);
    for (UEdGraph* G : AllGraphs)
    {
        if (G && MacroGuid.IsValid() && G->GraphGuid == MacroGuid)
        {
            MacroGraph = G;
            break;
        }
    }

    if (MacroGraph)
    {
        Typed->SetMacroGraph(MacroGraph);
        Typed->ReconstructNode();
    }
    return MacroGraph != nullptr;
}

static bool ImportNode_DynamicCast(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObject, UBlueprint* Blueprint)
{
    UK2Node_DynamicCast* Typed = CastChecked<UK2Node_DynamicCast>(Node);

    FString TargetClassPath;
    if (!NodeObject->TryGetStringField(KeyCastTarget, TargetClassPath) || TargetClassPath.IsEmpty())
        return true;

    UClass* TargetClass = nullptr;
    if (!TryLoadClassByPath(TargetClassPath, TargetClass))
        return false;

    Typed->TargetType = TargetClass;
    Typed->ReconstructNode();
    return true;
}

static bool ImportNode_ComponentBoundEvent(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObject, UBlueprint* Blueprint)
{
    UK2Node_ComponentBoundEvent* Typed = CastChecked<UK2Node_ComponentBoundEvent>(Node);

    const TSharedPtr<FJsonObject>* Obj = nullptr;
    if (NodeObject->TryGetObjectField(KeyComponentEvent, Obj) && Obj && (*Obj).IsValid())
    {
        FString DelegateName, OwnerClassPath, CompPropName;
        (*Obj)->TryGetStringField(KeyDelegatePropName, DelegateName);
        (*Obj)->TryGetStringField(KeyDelegateOwnerClass, OwnerClassPath);
        (*Obj)->TryGetStringField(KeyComponentPropName, CompPropName);

        if (!DelegateName.IsEmpty())
            Typed->DelegatePropertyName = *DelegateName;
        if (!CompPropName.IsEmpty())
            Typed->ComponentPropertyName = *CompPropName;

        UClass* OwnerClass = nullptr;
        if (TryLoadClassByPath(OwnerClassPath, OwnerClass))
            Typed->DelegateOwnerClass = OwnerClass;
    }

    // Also restore event_ref from parent handler
    ImportNode_Event(Node, NodeObject, Blueprint);
    return true;
}

// ── Dispatch maps ───────────────────────────────────────────────────

static const TMap<UClass*, FNodeExportHandler>& GetExportHandlers()
{
    static TMap<UClass*, FNodeExportHandler> Handlers;
    if (Handlers.Num() == 0)
    {
        Handlers.Add(UK2Node_CallFunction::StaticClass(),        &ExportNode_CallFunction);
        Handlers.Add(UK2Node_VariableGet::StaticClass(),         &ExportNode_Variable);
        Handlers.Add(UK2Node_VariableSet::StaticClass(),         &ExportNode_Variable);
        Handlers.Add(UK2Node_CustomEvent::StaticClass(),         &ExportNode_CustomEvent);
        Handlers.Add(UK2Node_Event::StaticClass(),               &ExportNode_Event);
        Handlers.Add(UK2Node_MacroInstance::StaticClass(),       &ExportNode_MacroInstance);
        Handlers.Add(UK2Node_DynamicCast::StaticClass(),         &ExportNode_DynamicCast);
        Handlers.Add(UK2Node_ComponentBoundEvent::StaticClass(), &ExportNode_ComponentBoundEvent);
    }
    return Handlers;
}

static const TMap<UClass*, FNodeImportHandler>& GetImportHandlers()
{
    static TMap<UClass*, FNodeImportHandler> Handlers;
    if (Handlers.Num() == 0)
    {
        Handlers.Add(UK2Node_CallFunction::StaticClass(),        &ImportNode_CallFunction);
        Handlers.Add(UK2Node_VariableGet::StaticClass(),         &ImportNode_Variable);
        Handlers.Add(UK2Node_VariableSet::StaticClass(),         &ImportNode_Variable);
        Handlers.Add(UK2Node_CustomEvent::StaticClass(),         &ImportNode_CustomEvent);
        Handlers.Add(UK2Node_Event::StaticClass(),               &ImportNode_Event);
        Handlers.Add(UK2Node_MacroInstance::StaticClass(),       &ImportNode_MacroInstance);
        Handlers.Add(UK2Node_DynamicCast::StaticClass(),         &ImportNode_DynamicCast);
        Handlers.Add(UK2Node_ComponentBoundEvent::StaticClass(), &ImportNode_ComponentBoundEvent);
    }
    return Handlers;
}

static FNodeExportHandler FindExportHandler(const UEdGraphNode* Node)
{
    const auto& Handlers = GetExportHandlers();
    for (UClass* C = Node->GetClass(); C; C = C->GetSuperClass())
    {
        if (const FNodeExportHandler* Found = Handlers.Find(C))
            return *Found;
    }
    return nullptr;
}

static FNodeImportHandler FindImportHandler(UEdGraphNode* Node)
{
    const auto& Handlers = GetImportHandlers();
    for (UClass* C = Node->GetClass(); C; C = C->GetSuperClass())
    {
        if (const FNodeImportHandler* Found = Handlers.Find(C))
            return *Found;
    }
    return nullptr;
}

} // namespace

bool FBlueprintGraphRAGService::FindBlueprintAssets(
    const FString& RootPackagePath,
    const FString& NamePrefix,
    const bool bRecursivePaths,
    TArray<FAssetData>& OutAssets)
{
    OutAssets.Reset();

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
    AssetRegistry.SearchAllAssets(true);

    FARFilter Filter;
    Filter.PackagePaths.Add(*RootPackagePath);
    Filter.bRecursivePaths = bRecursivePaths;

#if ENGINE_MAJOR_VERSION >= 5
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
#else
    Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
#endif

    TArray<FAssetData> Assets;
    AssetRegistry.GetAssets(Filter, Assets);

    for (const FAssetData& Asset : Assets)
    {
        if (!NamePrefix.IsEmpty() && !Asset.AssetName.ToString().StartsWith(NamePrefix))
        {
            continue;
        }
        OutAssets.Add(Asset);
    }

    OutAssets.Sort([](const FAssetData& A, const FAssetData& B)
    {
        return A.GetObjectPathString() < B.GetObjectPathString();
    });

    return true;
}

bool FBlueprintGraphRAGService::ExportBlueprintToJson(UBlueprint* Blueprint, FString& OutJson, FString& OutError)
{
    OutJson.Reset();
    OutError.Reset();

    if (!Blueprint)
    {
        OutError = TEXT("ExportBlueprintToJson: Blueprint is null.");
        return false;
    }

    TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
    RootObject->SetStringField(KeyBlueprint, Blueprint->GetPathName());

    TArray<TSharedPtr<FJsonValue>> GraphJsonArray;
    TArray<UEdGraph*> AllGraphs;
    Blueprint->GetAllGraphs(AllGraphs);

    for (UEdGraph* Graph : AllGraphs)
    {
        if (!Graph)
        {
            continue;
        }

        TSharedRef<FJsonObject> GraphObject = MakeShared<FJsonObject>();
        GraphObject->SetStringField(KeyGraphName, Graph->GetName());
        GraphObject->SetStringField(KeyGraphGuid, Graph->GraphGuid.ToString());
        GraphObject->SetStringField(
            KeyGraphSchema,
            Graph->GetSchema() ? Graph->GetSchema()->GetClass()->GetPathName() : FString());

        TArray<TSharedPtr<FJsonValue>> NodeJsonArray;
        TArray<TSharedPtr<FJsonValue>> LinkJsonArray;
        TSet<FString> EmittedLinks;

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }

            TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
            NodeObject->SetStringField(KeyNodeGuid, Node->NodeGuid.ToString());
            NodeObject->SetStringField(KeyNodeClass, Node->GetClass()->GetPathName());
            NodeObject->SetStringField(KeyNodeTitle, Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
            NodeObject->SetNumberField(KeyNodePosX, Node->NodePosX);
            NodeObject->SetNumberField(KeyNodePosY, Node->NodePosY);
            NodeObject->SetStringField(KeyNodeComment, Node->NodeComment);

            // Dispatch to node-specific export handler (CallFunction, Variable, Event, etc.)
            if (FNodeExportHandler Handler = FindExportHandler(Node))
            {
                Handler(Node, NodeObject);
            }

            TArray<TSharedPtr<FJsonValue>> PinJsonArray;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin)
                {
                    continue;
                }

                TSharedRef<FJsonObject> PinObject = MakeShared<FJsonObject>();
                PinObject->SetStringField(KeyPinId, Pin->PinId.ToString());
                PinObject->SetStringField(KeyPinName, Pin->PinName.ToString());
                PinObject->SetStringField(KeyPinDirection, PinDirectionToString(Pin->Direction));
                PinObject->SetStringField(KeyPinTypeCat, Pin->PinType.PinCategory.ToString());
                PinObject->SetStringField(KeyPinTypeSub, Pin->PinType.PinSubCategory.ToString());
                PinObject->SetStringField(
                    KeyPinTypeObj,
                    Pin->PinType.PinSubCategoryObject.IsValid()
                        ? Pin->PinType.PinSubCategoryObject.Get()->GetPathName()
                        : FString());
                PinObject->SetStringField(KeyPinDefaultValue, Pin->DefaultValue);
                PinObject->SetStringField(
                    KeyPinDefaultObject,
                    Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString());
                PinObject->SetStringField(KeyPinDefaultText, Pin->DefaultTextValue.ToString());
                ExportPinTypeExtended(Pin->PinType, PinObject);
                PinJsonArray.Add(MakeShared<FJsonValueObject>(PinObject));

                if (Pin->Direction != EGPD_Output)
                {
                    continue;
                }

                for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    if (!LinkedPin || !LinkedPin->GetOwningNode())
                    {
                        continue;
                    }

                    const FString LinkKey = FString::Printf(
                        TEXT("%s|%s|%s|%s"),
                        *Node->NodeGuid.ToString(),
                        *Pin->PinId.ToString(),
                        *LinkedPin->GetOwningNode()->NodeGuid.ToString(),
                        *LinkedPin->PinId.ToString());

                    if (EmittedLinks.Contains(LinkKey))
                    {
                        continue;
                    }
                    EmittedLinks.Add(LinkKey);

                    TSharedRef<FJsonObject> LinkObject = MakeShared<FJsonObject>();
                    LinkObject->SetStringField(KeyLinkFromNodeGuid, Node->NodeGuid.ToString());
                    LinkObject->SetStringField(KeyLinkFromPinId, Pin->PinId.ToString());
                    LinkObject->SetStringField(KeyLinkFromPinName, Pin->PinName.ToString());
                    LinkObject->SetStringField(KeyLinkToNodeGuid, LinkedPin->GetOwningNode()->NodeGuid.ToString());
                    LinkObject->SetStringField(KeyLinkToPinId, LinkedPin->PinId.ToString());
                    LinkObject->SetStringField(KeyLinkToPinName, LinkedPin->PinName.ToString());
                    LinkJsonArray.Add(MakeShared<FJsonValueObject>(LinkObject));
                }
            }

            NodeObject->SetArrayField(KeyPins, PinJsonArray);
            NodeJsonArray.Add(MakeShared<FJsonValueObject>(NodeObject));
        }

        GraphObject->SetArrayField(KeyNodes, NodeJsonArray);
        GraphObject->SetArrayField(KeyLinks, LinkJsonArray);
        GraphJsonArray.Add(MakeShared<FJsonValueObject>(GraphObject));
    }

    RootObject->SetArrayField(KeyGraphs, GraphJsonArray);

    // ── Export Blueprint variables ──────────────────────────────────
    TArray<TSharedPtr<FJsonValue>> VarJsonArray;
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        TSharedRef<FJsonObject> VarObj = MakeShared<FJsonObject>();
        VarObj->SetStringField(KeyVarName, Var.VarName.ToString());
        VarObj->SetStringField(KeyVarGuid, Var.VarGuid.ToString());
        VarObj->SetStringField(KeyVarFriendlyName, Var.FriendlyName);
        VarObj->SetStringField(KeyVarCategory, Var.Category.ToString());
        VarObj->SetStringField(KeyVarDefaultValue, Var.DefaultValue);
        VarObj->SetNumberField(KeyVarPropertyFlags, static_cast<double>(Var.PropertyFlags));
        VarObj->SetStringField(KeyVarRepNotifyFunc, Var.RepNotifyFunc.ToString());
        VarObj->SetObjectField(KeyVarType, ExportEdGraphPinType(Var.VarType));

        TSharedRef<FJsonObject> MetaObj = MakeShared<FJsonObject>();
        for (const FBPVariableMetaDataEntry& Entry : Var.MetaDataArray)
        {
            MetaObj->SetStringField(Entry.DataKey.ToString(), Entry.DataValue);
        }
        VarObj->SetObjectField(KeyVarMetaData, MetaObj);

        VarJsonArray.Add(MakeShared<FJsonValueObject>(VarObj));
    }
    RootObject->SetArrayField(KeyVariables, VarJsonArray);

    TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> JsonWriter =
        TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutJson);

    if (!FJsonSerializer::Serialize(RootObject, JsonWriter))
    {
        OutError = TEXT("ExportBlueprintToJson: Failed to serialize JSON.");
        return false;
    }

    return true;
}

bool FBlueprintGraphRAGService::ExportBlueprintToFile(UBlueprint* Blueprint, const FString& FilePath, FString& OutError)
{
    FString JsonText;
    if (!ExportBlueprintToJson(Blueprint, JsonText, OutError))
    {
        return false;
    }

    IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
    const bool bOk = FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8);
    if (!bOk)
    {
        OutError = FString::Printf(TEXT("ExportBlueprintToFile: Failed to save '%s'."), *FilePath);
        return false;
    }

    return true;
}

bool FBlueprintGraphRAGService::ImportBlueprintFromJson(
    UBlueprint* Blueprint,
    const FString& JsonText,
    const FBlueprintRAGImportOptions& Options,
    FString& OutError)
{
    OutError.Reset();

    if (!Blueprint)
    {
        OutError = TEXT("ImportBlueprintFromJson: Blueprint is null.");
        return false;
    }

    TSharedPtr<FJsonObject> RootObject;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
    if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
    {
        OutError = TEXT("ImportBlueprintFromJson: Invalid JSON payload.");
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* GraphArrayPtr = nullptr;
    if (!RootObject->TryGetArrayField(KeyGraphs, GraphArrayPtr) || !GraphArrayPtr)
    {
        OutError = TEXT("ImportBlueprintFromJson: Missing 'graphs' array.");
        return false;
    }

    for (const TSharedPtr<FJsonValue>& GraphValue : *GraphArrayPtr)
    {
        const TSharedPtr<FJsonObject> GraphObject = GraphValue.IsValid() ? GraphValue->AsObject() : nullptr;
        if (!GraphObject.IsValid())
        {
            continue;
        }

        FString GraphName;
        GraphObject->TryGetStringField(KeyGraphName, GraphName);

        FGuid GraphGuid;
        ParseGuidField(GraphObject, KeyGraphGuid, GraphGuid);

        UEdGraph* TargetGraph = FindGraphByGuidOrName(Blueprint, GraphGuid, GraphName);

        if (!TargetGraph && Options.bCreateMissingGraphs)
        {
            const FName DesiredGraphName = FName(*GraphName);
            TargetGraph = FBlueprintEditorUtils::CreateNewGraph(
                Blueprint,
                DesiredGraphName,
                UEdGraph::StaticClass(),
                UEdGraphSchema_K2::StaticClass());

            if (TargetGraph)
            {
                FBlueprintEditorUtils::AddUbergraphPage(Blueprint, TargetGraph);
                if (GraphGuid.IsValid())
                {
                    TargetGraph->GraphGuid = GraphGuid;
                }
            }
        }

        if (!TargetGraph)
        {
            continue;
        }

        // ── Replace mode: clear all existing nodes before importing ──
        if (Options.bClearGraphBeforeImport)
        {
            for (UEdGraphNode* Existing : TargetGraph->Nodes)
            {
                if (Existing)
                {
                    Existing->BreakAllNodeLinks();
                }
            }
            TArray<UEdGraphNode*> NodesToRemove = TargetGraph->Nodes;
            for (UEdGraphNode* Existing : NodesToRemove)
            {
                if (Existing)
                {
                    TargetGraph->RemoveNode(Existing);
                }
            }
        }

        TMap<FGuid, UEdGraphNode*> ImportedNodeMap;

        const TArray<TSharedPtr<FJsonValue>>* NodeArrayPtr = nullptr;
        if (GraphObject->TryGetArrayField(KeyNodes, NodeArrayPtr) && NodeArrayPtr)
        {
            for (const TSharedPtr<FJsonValue>& NodeValue : *NodeArrayPtr)
            {
                const TSharedPtr<FJsonObject> NodeObject = NodeValue.IsValid() ? NodeValue->AsObject() : nullptr;
                if (!NodeObject.IsValid())
                {
                    continue;
                }

                FGuid NodeGuid;
                ParseGuidField(NodeObject, KeyNodeGuid, NodeGuid);

                UEdGraphNode* TargetNode = FindNodeByGuid(TargetGraph, NodeGuid);

                if (!TargetNode)
                {
                    FString NodeClassPath;
                    NodeObject->TryGetStringField(KeyNodeClass, NodeClassPath);

                    UClass* NodeClass = nullptr;
                    if (!TryLoadClassByPath(NodeClassPath, NodeClass) ||
                        !NodeClass->IsChildOf(UEdGraphNode::StaticClass()))
                    {
                        continue;
                    }

                    TargetNode = NewObject<UEdGraphNode>(TargetGraph, NodeClass, NAME_None, RF_Transactional);
                    if (!TargetNode)
                    {
                        continue;
                    }

                    TargetGraph->AddNode(TargetNode, false, false);
                    TargetNode->CreateNewGuid();
                    if (NodeGuid.IsValid())
                    {
                        TargetNode->NodeGuid = NodeGuid;
                    }
                    TargetNode->PostPlacedNewNode();
                    TargetNode->AllocateDefaultPins();
                }

                double PosX = static_cast<double>(TargetNode->NodePosX);
                double PosY = static_cast<double>(TargetNode->NodePosY);
                NodeObject->TryGetNumberField(KeyNodePosX, PosX);
                NodeObject->TryGetNumberField(KeyNodePosY, PosY);
                TargetNode->NodePosX = static_cast<int32>(PosX);
                TargetNode->NodePosY = static_cast<int32>(PosY);

                FString NodeComment;
                NodeObject->TryGetStringField(KeyNodeComment, NodeComment);
                TargetNode->NodeComment = NodeComment;

                // Dispatch to node-specific import handler
                if (FNodeImportHandler Handler = FindImportHandler(TargetNode))
                {
                    if (!Handler(TargetNode, NodeObject, Blueprint))
                    {
                        UE_LOG(LogTemp, Warning,
                            TEXT("ImportBlueprintFromJson: handler failed for node %s (%s)"),
                            *TargetNode->NodeGuid.ToString(),
                            *TargetNode->GetClass()->GetName());
                    }
                }

                if (NodeGuid.IsValid())
                {
                    ImportedNodeMap.Add(NodeGuid, TargetNode);
                }

                const TArray<TSharedPtr<FJsonValue>>* PinArrayPtr = nullptr;
                if (NodeObject->TryGetArrayField(KeyPins, PinArrayPtr) && PinArrayPtr)
                {
                    for (const TSharedPtr<FJsonValue>& PinValue : *PinArrayPtr)
                    {
                        const TSharedPtr<FJsonObject> PinObject = PinValue.IsValid() ? PinValue->AsObject() : nullptr;
                        if (!PinObject.IsValid())
                        {
                            continue;
                        }

                        FGuid PinGuid;
                        ParseGuidField(PinObject, KeyPinId, PinGuid);

                        FString PinName;
                        FString PinDirectionText;
                        PinObject->TryGetStringField(KeyPinName, PinName);
                        PinObject->TryGetStringField(KeyPinDirection, PinDirectionText);

                        UEdGraphPin* TargetPin = FindPinByIdOrName(
                            TargetNode,
                            PinGuid,
                            PinName,
                            PinDirectionFromString(PinDirectionText));

                        if (!TargetPin)
                        {
                            continue;
                        }

                        ApplyPinDefaults(TargetPin, PinObject);
                        ImportPinTypeFromJson(TargetPin, PinObject);
                    }
                }
            }
        }

        const TArray<TSharedPtr<FJsonValue>>* LinkArrayPtr = nullptr;
        if (GraphObject->TryGetArrayField(KeyLinks, LinkArrayPtr) && LinkArrayPtr)
        {
            const UEdGraphSchema* Schema = TargetGraph->GetSchema();

            for (const TSharedPtr<FJsonValue>& LinkValue : *LinkArrayPtr)
            {
                const TSharedPtr<FJsonObject> LinkObject = LinkValue.IsValid() ? LinkValue->AsObject() : nullptr;
                if (!LinkObject.IsValid())
                {
                    continue;
                }

                FGuid FromNodeGuid;
                FGuid ToNodeGuid;
                FGuid FromPinGuid;
                FGuid ToPinGuid;
                ParseGuidField(LinkObject, KeyLinkFromNodeGuid, FromNodeGuid);
                ParseGuidField(LinkObject, KeyLinkToNodeGuid, ToNodeGuid);
                ParseGuidField(LinkObject, KeyLinkFromPinId, FromPinGuid);
                ParseGuidField(LinkObject, KeyLinkToPinId, ToPinGuid);

                FString FromPinName;
                FString ToPinName;
                LinkObject->TryGetStringField(KeyLinkFromPinName, FromPinName);
                LinkObject->TryGetStringField(KeyLinkToPinName, ToPinName);

                UEdGraphNode* FromNode = ImportedNodeMap.FindRef(FromNodeGuid);
                UEdGraphNode* ToNode = ImportedNodeMap.FindRef(ToNodeGuid);

                if (!FromNode)
                {
                    FromNode = FindNodeByGuid(TargetGraph, FromNodeGuid);
                }
                if (!ToNode)
                {
                    ToNode = FindNodeByGuid(TargetGraph, ToNodeGuid);
                }
                if (!FromNode || !ToNode)
                {
                    continue;
                }

                UEdGraphPin* FromPin = FindPinByIdOrName(FromNode, FromPinGuid, FromPinName, EGPD_Output);
                UEdGraphPin* ToPin = FindPinByIdOrName(ToNode, ToPinGuid, ToPinName, EGPD_Input);
                if (!FromPin || !ToPin || !Schema)
                {
                    continue;
                }

                if (!FromPin->LinkedTo.Contains(ToPin))
                {
                    Schema->TryCreateConnection(FromPin, ToPin);
                }
            }
        }
    }

    // ── Import Blueprint variables ─────────────────────────────────
    if (Options.bImportVariables)
    {
        const TArray<TSharedPtr<FJsonValue>>* VarArrayPtr = nullptr;
        if (RootObject->TryGetArrayField(KeyVariables, VarArrayPtr) && VarArrayPtr)
        {
            TMap<FName, int32> ExistingVarIndex;
            for (int32 i = 0; i < Blueprint->NewVariables.Num(); ++i)
            {
                ExistingVarIndex.Add(Blueprint->NewVariables[i].VarName, i);
            }

            for (const TSharedPtr<FJsonValue>& VarValue : *VarArrayPtr)
            {
                const TSharedPtr<FJsonObject> VarObj =
                    VarValue.IsValid() ? VarValue->AsObject() : nullptr;
                if (!VarObj.IsValid()) continue;

                FString VarNameStr;
                VarObj->TryGetStringField(KeyVarName, VarNameStr);
                if (VarNameStr.IsEmpty()) continue;

                FName VarName(*VarNameStr);

                int32 VarIdx = INDEX_NONE;
                if (int32* Found = ExistingVarIndex.Find(VarName))
                {
                    VarIdx = *Found;
                }
                else
                {
                    VarIdx = Blueprint->NewVariables.AddDefaulted();
                    Blueprint->NewVariables[VarIdx].VarName = VarName;
                }

                FBPVariableDescription& Desc = Blueprint->NewVariables[VarIdx];

                FString GuidStr;
                if (VarObj->TryGetStringField(KeyVarGuid, GuidStr))
                    FGuid::Parse(GuidStr, Desc.VarGuid);

                VarObj->TryGetStringField(KeyVarFriendlyName, Desc.FriendlyName);

                FString CategoryStr;
                if (VarObj->TryGetStringField(KeyVarCategory, CategoryStr))
                    Desc.Category = FText::FromString(CategoryStr);

                FString DefaultVal;
                if (VarObj->TryGetStringField(KeyVarDefaultValue, DefaultVal))
                    Desc.DefaultValue = DefaultVal;

                double PropFlags = 0;
                if (VarObj->TryGetNumberField(KeyVarPropertyFlags, PropFlags))
                    Desc.PropertyFlags = static_cast<uint64>(PropFlags);

                FString RepFunc;
                if (VarObj->TryGetStringField(KeyVarRepNotifyFunc, RepFunc))
                    Desc.RepNotifyFunc = *RepFunc;

                const TSharedPtr<FJsonObject>* TypeObj = nullptr;
                if (VarObj->TryGetObjectField(KeyVarType, TypeObj) && TypeObj)
                {
                    ImportEdGraphPinType(*TypeObj, Desc.VarType);
                }

                const TSharedPtr<FJsonObject>* MetaObj = nullptr;
                if (VarObj->TryGetObjectField(KeyVarMetaData, MetaObj) && MetaObj)
                {
                    Desc.MetaDataArray.Reset();
                    for (const auto& Pair : (*MetaObj)->Values)
                    {
                        FBPVariableMetaDataEntry Entry;
                        Entry.DataKey = *Pair.Key;
                        Entry.DataValue = Pair.Value->AsString();
                        Desc.MetaDataArray.Add(Entry);
                    }
                }
            }
        }
    }

    if (Options.bMarkDirty)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        Blueprint->MarkPackageDirty();
    }

    if (Options.bCompileAfterImport)
    {
#if ENGINE_MAJOR_VERSION >= 5
        FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave);
#else
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
#endif
    }

    return true;
}

bool FBlueprintGraphRAGService::ImportBlueprintFromFile(
    UBlueprint* Blueprint,
    const FString& FilePath,
    const FBlueprintRAGImportOptions& Options,
    FString& OutError)
{
    FString JsonText;
    if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
    {
        OutError = FString::Printf(TEXT("ImportBlueprintFromFile: Failed to read '%s'."), *FilePath);
        return false;
    }
    return ImportBlueprintFromJson(Blueprint, JsonText, Options, OutError);
}

bool FBlueprintGraphRAGService::SaveBlueprintPackage(UBlueprint* Blueprint, FString& OutError)
{
    OutError.Reset();

    if (!Blueprint)
    {
        OutError = TEXT("SaveBlueprintPackage: Blueprint is null.");
        return false;
    }

    UPackage* Package = Blueprint->GetOutermost();
    if (!Package)
    {
        OutError = TEXT("SaveBlueprintPackage: Package is null.");
        return false;
    }

    const FString Filename =
        FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

#if ENGINE_MAJOR_VERSION >= 5
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    const bool bSaved = UPackage::SavePackage(Package, Blueprint, *Filename, SaveArgs);
#else
    const bool bSaved = UPackage::SavePackage(
        Package,
        Blueprint,
        RF_Public | RF_Standalone,
        *Filename,
        GError,
        nullptr,
        false,
        true,
        SAVE_NoError);
#endif

    if (!bSaved)
    {
        OutError = FString::Printf(TEXT("SaveBlueprintPackage: Failed to save '%s'."), *Filename);
        return false;
    }

    return true;
}

bool FBlueprintGraphRAGService::LoadBlueprintPathFromJsonFile(
    const FString& FilePath,
    FString& OutBlueprintPath)
{
    OutBlueprintPath.Reset();

    FString JsonText;
    if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
    {
        return false;
    }

    TSharedPtr<FJsonObject> RootObject;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
    if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
    {
        return false;
    }

    return RootObject->TryGetStringField(TEXT("blueprint"), OutBlueprintPath)
        && !OutBlueprintPath.IsEmpty();
}
