#include "JsonBlueprintSyncModule.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogJsonBlueprintSync);

void FJsonBlueprintSyncModule::StartupModule()
{
	UE_LOG(LogJsonBlueprintSync, Log, TEXT("JsonBlueprintSync module started"));
}

void FJsonBlueprintSyncModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FJsonBlueprintSyncModule, JsonBlueprintSync);
