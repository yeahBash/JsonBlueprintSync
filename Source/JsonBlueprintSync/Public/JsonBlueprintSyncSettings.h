#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "JsonBlueprintSync.h"

#include "JsonBlueprintSyncSettings.generated.h"

/** How an asset type is synced with its JSON file. */
UENUM()
enum class EJsonSyncMode : uint8
{
	/** Not synced: no JSON is written or read. */
	None,
	/** Saving the asset writes its JSON. JSON changes are listed in the JSON Sync tab and only applied when you choose to. */
	WriteJsonOnly UMETA(DisplayName = "Write JSON Only"),
	/** Saving the asset writes its JSON, and JSON changes are applied automatically on editor start (as long as they do not conflict). */
	TwoWay UMETA(DisplayName = "Two Way"),
};

/** Project Settings > Plugins > JSON Blueprint Sync. Stored in Config/DefaultEditor.ini, so the whole team shares it. */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "JSON Blueprint Sync"))
class JSONBLUEPRINTSYNC_API UJsonBlueprintSyncSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UJsonBlueprintSyncSettings();

	/** Folder for the JSON files, relative to the project folder. /Game/A/BP_B is written to <JsonDir>/Game/A/BP_B.json. */
	UPROPERTY(config, EditAnywhere, Category = "General")
	FString JsonDir = TEXT("JsonBlueprintSync");

	/** Blueprints (Actor, Object and other Blueprint classes). Two Way is not available: the .uasset is the source of truth. */
	UPROPERTY(config, EditAnywhere, Category = "Modes", meta = (InvalidEnumValues = "TwoWay"))
	EJsonSyncMode Blueprints = EJsonSyncMode::WriteJsonOnly;

	/** Widget Blueprints. Two Way is not available: the .uasset is the source of truth. */
	UPROPERTY(config, EditAnywhere, Category = "Modes", meta = (InvalidEnumValues = "TwoWay"))
	EJsonSyncMode WidgetBlueprints = EJsonSyncMode::WriteJsonOnly;

	/** Data assets (instances of UDataAsset subclasses, including Blueprint subclasses). */
	UPROPERTY(config, EditAnywhere, Category = "Modes")
	EJsonSyncMode DataAssets = EJsonSyncMode::TwoWay;

	/** The effective mode for an asset kind. Two Way set by hand in the ini for Blueprints is treated as Write JSON Only. */
	EJsonSyncMode GetMode(JsonBlueprintSync::EAssetKind Kind) const;

	virtual FName GetCategoryName() const override;
#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
#endif
};
