#include "JsonBlueprintSyncModule.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Modules/ModuleManager.h"
#include "SAssetMergePanel.h"
#include "SAssetMergeStatusBar.h"
#include "SJsonSyncPanel.h"
#include "SJsonSyncStatusBar.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

DEFINE_LOG_CATEGORY(LogJsonBlueprintSync);

#define LOCTEXT_NAMESPACE "JsonBlueprintSync"

void FJsonBlueprintSyncModule::StartupModule()
{
	if (IsRunningCommandlet() || !FSlateApplication::IsInitialized())
	{
		return;
	}
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(JsonBlueprintSync::GetSyncTabName(), FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
		{
			return SNew(SDockTab)
				.TabRole(ETabRole::NomadTab)
				[
					SNew(SJsonSyncPanel)
				];
		}))
		.SetDisplayName(LOCTEXT("TabTitle", "JSON Sync"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Assets whose JSON file has changes: review them and apply the JSON or overwrite it."))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Merge"));
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(JsonBlueprintSync::GetMergeTabName(), FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
		{
			return SNew(SDockTab)
				.TabRole(ETabRole::NomadTab)
				[
					SNew(SAssetMergePanel)
				];
		}))
		.SetDisplayName(LOCTEXT("MergeTabTitle", "Asset Merges"))
		.SetTooltipText(LOCTEXT("MergeTabTooltip", "Asset merges saved by the merge tool: apply the merged values to the assets."))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Integrate"));
	bTabRegistered = true;

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([this]()
	{
		SJsonSyncStatusBar::Register(this);
		SAssetMergeStatusBar::Register(this);
	}));
}

void FJsonBlueprintSyncModule::ShutdownModule()
{
	if (bTabRegistered && UObjectInitialized() && UToolMenus::TryGet())
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}
	if (bTabRegistered && FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(JsonBlueprintSync::GetSyncTabName());
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(JsonBlueprintSync::GetMergeTabName());
	}
	bTabRegistered = false;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FJsonBlueprintSyncModule, JsonBlueprintSync);
