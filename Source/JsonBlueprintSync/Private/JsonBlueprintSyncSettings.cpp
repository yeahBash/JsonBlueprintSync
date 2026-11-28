#include "JsonBlueprintSyncSettings.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(JsonBlueprintSyncSettings)

#define LOCTEXT_NAMESPACE "JsonBlueprintSync"

UJsonBlueprintSyncSettings::UJsonBlueprintSyncSettings()
{
	SectionName = TEXT("JsonBlueprintSync");
}

FName UJsonBlueprintSyncSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

#if WITH_EDITOR
FText UJsonBlueprintSyncSettings::GetSectionText() const
{
	return LOCTEXT("SettingsSection", "JSON Blueprint Sync");
}

FText UJsonBlueprintSyncSettings::GetSectionDescription() const
{
	return LOCTEXT("SettingsDescription", "Keeps data asset values in JSON files next to the project. The JSON is the source of truth.");
}
#endif

#undef LOCTEXT_NAMESPACE
