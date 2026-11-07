#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "JsonBlueprintSyncCommandlet.generated.h"

/**
 * Syncs Blueprint class defaults with JSON files.
 *
 *   UnrealEditor-Cmd.exe <Project>.uproject -run=JsonBlueprintSync -export|-import|-verify [-dir=/Game/...] [-jsondir=<path>] [-nosave]
 *
 * -export  writes the JSON for every Blueprint under -dir (only files that change).
 * -import  applies every JSON file under -dir to its Blueprint, parents first, then saves the changed packages (unless -nosave).
 * -verify  fails if any JSON file is missing, differs from its Blueprint, or has no Blueprint.
 */
UCLASS()
class UJsonBlueprintSyncCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UJsonBlueprintSyncCommandlet();

	virtual int32 Main(const FString& Params) override;
};