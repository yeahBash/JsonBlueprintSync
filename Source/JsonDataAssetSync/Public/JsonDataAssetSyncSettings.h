#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"

#include "JsonDataAssetSyncSettings.generated.h"

/** What happens to a JSON change on editor start or Refresh. Saving a data asset writes its JSON in both modes. */
UENUM()
enum class EJsonSyncMode : uint8
{
	/** JSON changes are applied to the asset and saved, as long as nothing conflicts and the asset has no unsaved edits. */
	Automatic,
	/** JSON changes are listed in the JSON Sync tab, where you apply them key by key (an undo step; the asset is not saved). */
	Manual,
};

/** Project Settings > Plugins > JSON Data Asset Sync. Stored in Config/DefaultEditor.ini, so the whole team shares it. */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "JSON Data Asset Sync"))
class JSONDATAASSETSYNC_API UJsonDataAssetSyncSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UJsonDataAssetSyncSettings();

	/** Folder for the JSON files, relative to the project folder. /Game/A/DA_B is written to <JsonDir>/Game/A/DA_B.json. */
	UPROPERTY(config, EditAnywhere, Category = "General")
	FString JsonDir = TEXT("JsonDataAssetSync");

	/** What happens to JSON changes found on editor start or Refresh, e.g. after a git pull. */
	UPROPERTY(config, EditAnywhere, Category = "General")
	EJsonSyncMode Mode = EJsonSyncMode::Automatic;

	/**
	 * Content folders whose data assets are not synced at all, e.g. /Game/ThirdParty for marketplace packs: no JSON is written,
	 * read or checked for them.
	 */
	UPROPERTY(config, EditAnywhere, Category = "General", meta = (LongPackageName))
	TArray<FDirectoryPath> ExcludedFolders;

	virtual FName GetCategoryName() const override;
#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
#endif
};
