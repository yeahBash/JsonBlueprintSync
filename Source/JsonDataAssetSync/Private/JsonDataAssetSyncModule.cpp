#include "JsonDataAssetSyncModule.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Modules/ModuleManager.h"
#include "SJsonSyncPanel.h"
#include "SJsonSyncStatusBar.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

DEFINE_LOG_CATEGORY(LogJsonDataAssetSync);

#define LOCTEXT_NAMESPACE "JsonDataAssetSync"

void FJsonDataAssetSyncModule::StartupModule()
{
	if (IsRunningCommandlet() || !FSlateApplication::IsInitialized())
	{
		return;
	}
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(JsonDataAssetSync::GetSyncTabName(), FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
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
	bTabRegistered = true;

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([this]()
	{
		SJsonSyncStatusBar::Register(this);
	}));
}

void FJsonDataAssetSyncModule::ShutdownModule()
{
	if (bTabRegistered && UObjectInitialized() && UToolMenus::TryGet())
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}
	if (bTabRegistered && FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(JsonDataAssetSync::GetSyncTabName());
	}
	bTabRegistered = false;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FJsonDataAssetSyncModule, JsonDataAssetSync);
