#include "JsonBlueprintSyncModule.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Modules/ModuleManager.h"
#include "SJsonSyncPanel.h"
#include "Styling/AppStyle.h"
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
	bTabRegistered = true;
}

void FJsonBlueprintSyncModule::ShutdownModule()
{
	if (bTabRegistered && FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(JsonBlueprintSync::GetSyncTabName());
	}
	bTabRegistered = false;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FJsonBlueprintSyncModule, JsonBlueprintSync);
