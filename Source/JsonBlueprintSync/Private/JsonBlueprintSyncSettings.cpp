#include "JsonBlueprintSyncSettings.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(JsonBlueprintSyncSettings)

#define LOCTEXT_NAMESPACE "JsonBlueprintSync"

UJsonBlueprintSyncSettings::UJsonBlueprintSyncSettings()
{
	SectionName = TEXT("JsonBlueprintSync");
}

EJsonSyncMode UJsonBlueprintSyncSettings::GetMode(JsonBlueprintSync::EAssetKind Kind) const
{
	auto NoTwoWay = [](EJsonSyncMode Mode) { return Mode == EJsonSyncMode::TwoWay ? EJsonSyncMode::WriteJsonOnly : Mode; };
	switch (Kind)
	{
	case JsonBlueprintSync::EAssetKind::Blueprint: return NoTwoWay(Blueprints);
	case JsonBlueprintSync::EAssetKind::WidgetBlueprint: return NoTwoWay(WidgetBlueprints);
	case JsonBlueprintSync::EAssetKind::DataAsset: return DataAssets;
	default: return EJsonSyncMode::None;
	}
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
	return LOCTEXT("SettingsDescription", "Keeps Blueprint, Widget Blueprint and data asset values in JSON files next to the project.");
}
#endif

#undef LOCTEXT_NAMESPACE
