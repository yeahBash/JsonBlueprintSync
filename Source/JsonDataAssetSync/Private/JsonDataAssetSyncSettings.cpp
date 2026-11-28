#include "JsonDataAssetSyncSettings.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(JsonDataAssetSyncSettings)

#define LOCTEXT_NAMESPACE "JsonDataAssetSync"

UJsonDataAssetSyncSettings::UJsonDataAssetSyncSettings()
{
	SectionName = TEXT("JsonDataAssetSync");
}

FName UJsonDataAssetSyncSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

#if WITH_EDITOR
FText UJsonDataAssetSyncSettings::GetSectionText() const
{
	return LOCTEXT("SettingsSection", "JSON Data Asset Sync");
}

FText UJsonDataAssetSyncSettings::GetSectionDescription() const
{
	return LOCTEXT("SettingsDescription", "Keeps data asset values in JSON files next to the project. The JSON is the source of truth.");
}
#endif

#undef LOCTEXT_NAMESPACE
