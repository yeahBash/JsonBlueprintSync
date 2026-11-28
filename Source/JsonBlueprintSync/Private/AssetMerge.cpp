#include "AssetMerge.h"

#include "JsonBlueprintSync.h"
#include "JsonBlueprintSyncModule.h"
#include "Blueprint/WidgetTree.h"
#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace JsonBlueprintSync::AssetMerge
{
	namespace
	{
		const TCHAR* Separator = TEXT(" = ");
		const TCHAR* NotDecoded = TEXT("!notdecoded");
		const TCHAR* AddedValue = TEXT("(added)");
		const TCHAR* RemovedValue = TEXT("(removed)");

		bool IsConflictMarker(const FString& Line)
		{
			return Line.StartsWith(TEXT("<<<<<<<"), ESearchCase::CaseSensitive)
				|| Line.StartsWith(TEXT("|||||||"), ESearchCase::CaseSensitive)
				|| Line.StartsWith(TEXT("======="), ESearchCase::CaseSensitive)
				|| Line.StartsWith(TEXT(">>>>>>>"), ESearchCase::CaseSensitive);
		}

		/** The first " = " outside quotes and brackets: a map key may hold " = ", e.g. Map["a = b"]. */
		int32 FindSeparator(const FString& Line)
		{
			int32 Depth = 0;
			bool bInQuotes = false;
			for (int32 Index = 0; Index < Line.Len(); ++Index)
			{
				const TCHAR C = Line[Index];
				if (bInQuotes)
				{
					if (C == TEXT('\\'))
					{
						++Index;
					}
					else if (C == TEXT('"'))
					{
						bInQuotes = false;
					}
				}
				else if (C == TEXT('"'))
				{
					bInQuotes = true;
				}
				else if (C == TEXT('[') || C == TEXT('('))
				{
					++Depth;
				}
				else if (C == TEXT(']') || C == TEXT(')'))
				{
					Depth = FMath::Max(0, Depth - 1);
				}
				else if (Depth == 0 && FCString::Strncmp(&Line[Index], Separator, 3) == 0)
				{
					return Index;
				}
			}
			return INDEX_NONE;
		}

		void SplitLines(const FString& Text, TArray<FString>& OutLines)
		{
			Text.ParseIntoArray(OutLines, TEXT("\n"), false);
			for (FString& Line : OutLines)
			{
				Line.RemoveFromEnd(TEXT("\r"), ESearchCase::CaseSensitive);
			}
		}

		void ClassifyRow(FMergeRow& Row)
		{
			const bool bNotDecoded = (Row.Current.IsSet() && Row.Current->StartsWith(NotDecoded, ESearchCase::CaseSensitive))
				|| (Row.Merged.IsSet() && Row.Merged->StartsWith(NotDecoded, ESearchCase::CaseSensitive));
			if (Row.Path == TEXT("!graph"))
			{
				Row.Action = EMergeAction::NotApplicable;
				Row.Reason = TEXT("A graph changed. Redo the change by hand, or take the other side's file and merge again.");
			}
			else if (Row.Path == TEXT("!native"))
			{
				Row.Action = EMergeAction::NotApplicable;
				Row.Reason = TEXT("Native data the reader can't read field by field changed. Redo the change by hand.");
			}
			else if (Row.Path.StartsWith(TEXT("!")))
			{
				Row.Action = EMergeAction::NotApplicable;
				Row.Reason = TEXT("Not a value.");
			}
			else if (bNotDecoded)
			{
				Row.Action = EMergeAction::NotApplicable;
				Row.Reason = TEXT("The reader can't decode this value. Redo the change by hand.");
			}
			else if (!Row.Merged.IsSet())
			{
				Row.Action = EMergeAction::Reset;
			}
			else if (*Row.Merged == AddedValue)
			{
				Row.Action = EMergeAction::Add;
			}
			else if (*Row.Merged == RemovedValue)
			{
				Row.Action = EMergeAction::Remove;
			}
			else
			{
				Row.Action = EMergeAction::Set;
			}
		}

		void DiffSection(const FMergeSection& Current, const FMergeSection& Merged, TArray<FMergeRow>& Rows)
		{
			TMap<FString, const FMergeLine*> CurrentLines;
			for (const FMergeLine& Line : Current.Lines)
			{
				CurrentLines.Add(Line.Path, &Line);
			}
			TSet<FString> MergedPaths;
			for (const FMergeLine& Line : Merged.Lines)
			{
				MergedPaths.Add(Line.Path);
				const FMergeLine* const* CurrentLine = CurrentLines.Find(Line.Path);
				if (CurrentLine && (*CurrentLine)->Value == Line.Value)
				{
					continue;
				}
				FMergeRow& Row = Rows.AddDefaulted_GetRef();
				Row.Object = Merged.Object;
				Row.Path = Line.Path;
				Row.Merged = Line.Value;
				if (CurrentLine)
				{
					Row.Current = (*CurrentLine)->Value;
				}
				ClassifyRow(Row);
			}
			for (const FMergeLine& Line : Current.Lines)
			{
				if (!MergedPaths.Contains(Line.Path))
				{
					FMergeRow& Row = Rows.AddDefaulted_GetRef();
					Row.Object = Current.Object;
					Row.Path = Line.Path;
					Row.Current = Line.Value;
					ClassifyRow(Row);
				}
			}
		}
	}

	const FMergeLine* FMergeSection::FindLine(const FString& Path) const
	{
		return Lines.FindByPredicate([&Path](const FMergeLine& Line) { return Line.Path == Path; });
	}

	const FMergeSection* FMergeText::FindSection(const FString& Object) const
	{
		return Sections.FindByPredicate([&Object](const FMergeSection& Section) { return Section.Object == Object; });
	}

	bool ParseMergeText(const FString& Text, FMergeText& Out)
	{
		Out = FMergeText();
		TArray<FString> Lines;
		SplitLines(Text, Lines);

		TSet<FString> SeenSections;
		TSet<FString> SeenPaths;
		FMergeSection* Section = nullptr;
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			const FString& Line = Lines[Index];
			const int32 LineNumber = Index + 1;
			auto AddError = [&Out, LineNumber](const FString& Problem)
			{
				Out.Errors.Add(FString::Printf(TEXT("Line %d: %s"), LineNumber, *Problem));
			};

			if (Line.IsEmpty())
			{
				continue;
			}
			if (IsConflictMarker(Line))
			{
				Out.ConflictLines.Add(LineNumber);
				continue;
			}
			if (Line.StartsWith(TEXT("#")))
			{
				if (!Section)
				{
					FString Comment = Line.Mid(1).TrimStart();
					if (Comment.RemoveFromStart(TEXT("Asset:"), ESearchCase::CaseSensitive))
					{
						Out.Asset = Comment.TrimStartAndEnd();
					}
					else if (Out.Format.IsEmpty())
					{
						Out.Format = Comment.TrimEnd();
					}
				}
				continue;
			}
			if (Line.StartsWith(TEXT("[")))
			{
				int32 Close = Line.Find(TEXT("] "), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
				FString Class;
				if (Close != INDEX_NONE)
				{
					Class = Line.Mid(Close + 2).TrimStartAndEnd();
				}
				else if (Line.EndsWith(TEXT("]")))
				{
					Close = Line.Len() - 1;
				}
				const FString Object = Close != INDEX_NONE ? Line.Mid(1, Close - 1) : FString();
				if (Object.IsEmpty())
				{
					AddError(TEXT("not a section line, \"[<object>] <class>\""));
					Section = nullptr;
					continue;
				}
				if (SeenSections.Contains(Object))
				{
					AddError(FString::Printf(TEXT("[%s] is there twice"), *Object));
				}
				SeenSections.Add(Object);
				SeenPaths.Reset();
				Section = &Out.Sections.AddDefaulted_GetRef();
				Section->Object = Object;
				Section->Class = Class;
				Section->LineNumber = LineNumber;
				continue;
			}
			if (!Section)
			{
				AddError(TEXT("a value outside a section"));
				continue;
			}
			const int32 Split = FindSeparator(Line);
			if (Split == INDEX_NONE)
			{
				AddError(TEXT("no \" = \" in the line"));
				continue;
			}
			FMergeLine& Value = Section->Lines.AddDefaulted_GetRef();
			Value.Path = Line.Left(Split);
			Value.Value = Line.Mid(Split + 3);
			Value.LineNumber = LineNumber;
			if (Section->Object != IgnoredSection)
			{
				if (SeenPaths.Contains(Value.Path))
				{
					AddError(FString::Printf(TEXT("%s is there twice in [%s]"), *Value.Path, *Section->Object));
				}
				SeenPaths.Add(Value.Path);
			}
		}
		return Out.Errors.IsEmpty() && Out.ConflictLines.IsEmpty();
	}

	const TCHAR* LexToString(EMergeAction Action)
	{
		switch (Action)
		{
		case EMergeAction::Set: return TEXT("Set");
		case EMergeAction::Add: return TEXT("Add");
		case EMergeAction::Remove: return TEXT("Remove");
		case EMergeAction::Reset: return TEXT("Reset");
		case EMergeAction::NotApplicable: return TEXT("Not applicable");
		}
		return TEXT("");
	}

	TArray<FMergeRow> DiffMergeText(const FMergeText& Current, const FMergeText& Merged)
	{
		TArray<FMergeRow> Rows;
		for (const FMergeSection& MergedSection : Merged.Sections)
		{
			if (MergedSection.Object == IgnoredSection)
			{
				continue;
			}
			const FMergeSection* CurrentSection = Current.FindSection(MergedSection.Object);
			if (!CurrentSection)
			{
				FMergeRow& Row = Rows.AddDefaulted_GetRef();
				Row.Object = MergedSection.Object;
				Row.Merged = MergedSection.Class;
				Row.Action = EMergeAction::NotApplicable;
				Row.Reason = TEXT("The object is only in merged.txt. Add it by hand, or take the other side's file and merge again.");
			}
			else if (CurrentSection->Class != MergedSection.Class)
			{
				FMergeRow& Row = Rows.AddDefaulted_GetRef();
				Row.Object = MergedSection.Object;
				Row.Current = CurrentSection->Class;
				Row.Merged = MergedSection.Class;
				Row.Action = EMergeAction::NotApplicable;
				Row.Reason = TEXT("The object's class changed. Change it by hand, or take the other side's file and merge again.");
			}
			else
			{
				DiffSection(*CurrentSection, MergedSection, Rows);
			}
		}
		for (const FMergeSection& CurrentSection : Current.Sections)
		{
			if (CurrentSection.Object != IgnoredSection && !Merged.FindSection(CurrentSection.Object))
			{
				FMergeRow& Row = Rows.AddDefaulted_GetRef();
				Row.Object = CurrentSection.Object;
				Row.Current = CurrentSection.Class;
				Row.Action = EMergeAction::NotApplicable;
				Row.Reason = TEXT("The object isn't in merged.txt. Remove it by hand, or take the other side's file and merge again.");
			}
		}
		return Rows;
	}

	FString GetMergeDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("JsonBlueprintSync") / TEXT("Merge"));
	}

	TArray<FMergeFileLocation> FindMergeFiles(const FString& MergeDir)
	{
		FString Root = FPaths::ConvertRelativePathToFull(MergeDir);
		FPaths::NormalizeDirectoryName(Root);

		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("merged.txt"), true, false);

		TArray<FMergeFileLocation> Locations;
		for (const FString& File : Files)
		{
			FString Folder = FPaths::GetPath(FPaths::ConvertRelativePathToFull(File));
			FPaths::NormalizeDirectoryName(Folder);
			if (Folder.Len() > Root.Len() + 1 && Folder.StartsWith(Root + TEXT("/")))
			{
				FMergeFileLocation& Location = Locations.AddDefaulted_GetRef();
				Location.PackageName = Folder.Mid(Root.Len());
				Location.MergedFile = FPaths::ConvertRelativePathToFull(File);
			}
		}
		Locations.Sort([](const FMergeFileLocation& A, const FMergeFileLocation& B) { return A.PackageName < B.PackageName; });
		return Locations;
	}

	bool FindReader(FString& OutReader, FString& OutError)
	{
		const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		int32 ReturnCode = -1;
		FString StdOut;
		FString StdErr;
		if (!FPlatformProcess::ExecProcess(TEXT("git"), TEXT("config --get unrealassetjson.path"), &ReturnCode, &StdOut, &StdErr, *ProjectDir))
		{
			OutError = TEXT("Couldn't run git. Is it installed and on PATH?");
			return false;
		}
		OutReader = StdOut.TrimStartAndEnd();
		if (ReturnCode != 0 || OutReader.IsEmpty())
		{
			OutError = TEXT("Set the reader's path with: git config unrealassetjson.path <path to UnrealAssetJson.exe>");
			return false;
		}
		if (!FPaths::FileExists(OutReader))
		{
			OutError = FString::Printf(TEXT("The reader set in git config unrealassetjson.path doesn't exist: %s"), *OutReader);
			return false;
		}
		return true;
	}

	bool RunReader(const FString& Reader, const FString& AssetFile, FString& OutText, FString& OutError)
	{
		const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("JsonBlueprintSync"));
		IFileManager::Get().MakeDirectory(*Dir, true);
		FString OutFile = Dir / FString::Printf(TEXT("Reader-%s.txt"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		FString InFile = FPaths::ConvertRelativePathToFull(AssetFile);
		FPaths::MakePlatformFilename(OutFile);
		FPaths::MakePlatformFilename(InFile);

		const FString Params = FString::Printf(TEXT("-merge -s=\"%s\" -f \"%s\""), *OutFile, *InFile);
		int32 ReturnCode = -1;
		FString StdOut;
		FString StdErr;
		const bool bRan = FPlatformProcess::ExecProcess(*Reader, *Params, &ReturnCode, &StdOut, &StdErr);
		const bool bRead = bRan && ReturnCode == 0 && FFileHelper::LoadFileToString(OutText, *OutFile);
		IFileManager::Get().Delete(*OutFile, false, false, true);
		if (!bRead)
		{
			FString Output = (StdOut + StdErr).TrimStartAndEnd();
			constexpr int32 MaxOutput = 2000;
			if (Output.Len() > MaxOutput)
			{
				Output = TEXT("...") + Output.Right(MaxOutput);
			}
			OutError = !bRan
				? FString::Printf(TEXT("Couldn't start the reader %s"), *Reader)
				: FString::Printf(TEXT("The reader failed on %s (exit code %d)%s%s"), *InFile, ReturnCode, Output.IsEmpty() ? TEXT("") : TEXT(":\n"), *Output);
			return false;
		}
		return true;
	}

	const TCHAR* LexToString(EMergeStatus Status)
	{
		switch (Status)
		{
		case EMergeStatus::Ready: return TEXT("Ready");
		case EMergeStatus::Done: return TEXT("Done");
		case EMergeStatus::HasConflictMarkers: return TEXT("Has conflict markers");
		case EMergeStatus::HasErrors: return TEXT("Has errors");
		case EMergeStatus::FormatDiffers: return TEXT("Format differs");
		case EMergeStatus::AssetNotFound: return TEXT("Asset not found");
		case EMergeStatus::ReaderNotSetUp: return TEXT("Reader not set up");
		case EMergeStatus::ReaderFailed: return TEXT("Reader failed");
		case EMergeStatus::UnsavedChanges: return TEXT("Asset has unsaved changes");
		}
		return TEXT("");
	}

	FMergeFile CheckMergeFile(const FMergeFileLocation& Location, const FString& AssetFile, bool bAssetDirty, FReadAsset ReadAsset)
	{
		FMergeFile File;
		File.Location = Location;
		File.AssetFile = AssetFile;

		FString MergedString;
		if (!FFileHelper::LoadFileToString(MergedString, *Location.MergedFile))
		{
			File.Status = EMergeStatus::HasErrors;
			File.Message = FString::Printf(TEXT("Can't read %s"), *Location.MergedFile);
			return File;
		}
		FMergeText Merged;
		ParseMergeText(MergedString, Merged);
		if (!Merged.ConflictLines.IsEmpty())
		{
			TArray<FString> Numbers;
			for (int32 Number : Merged.ConflictLines)
			{
				Numbers.Add(FString::FromInt(Number));
			}
			File.Status = EMergeStatus::HasConflictMarkers;
			File.Message = FString::Printf(TEXT("Conflict markers on lines %s. Resolve them in the merge tool."), *FString::Join(Numbers, TEXT(", ")));
			return File;
		}
		if (!Merged.Errors.IsEmpty())
		{
			File.Status = EMergeStatus::HasErrors;
			File.Message = FString::Join(Merged.Errors, TEXT("\n"));
			return File;
		}
		if (Merged.Asset != Location.PackageName)
		{
			File.Status = EMergeStatus::HasErrors;
			File.Message = FString::Printf(TEXT("merged.txt is for \"%s\", but its folder is for %s."), *Merged.Asset, *Location.PackageName);
			return File;
		}
		if (AssetFile.IsEmpty())
		{
			File.Status = EMergeStatus::AssetNotFound;
			File.Message = FString::Printf(TEXT("There's no package %s."), *Location.PackageName);
			return File;
		}

		FString CurrentString;
		FString Error;
		if (!ReadAsset(AssetFile, CurrentString, Error))
		{
			File.Status = EMergeStatus::ReaderFailed;
			File.Message = Error;
			return File;
		}
		FMergeText Current;
		if (!ParseMergeText(CurrentString, Current))
		{
			File.Status = EMergeStatus::ReaderFailed;
			File.Message = TEXT("The reader's text can't be read: ") + FString::Join(Current.Errors, TEXT("\n"));
			return File;
		}
		if (Current.Format != Merged.Format)
		{
			File.Status = EMergeStatus::FormatDiffers;
			File.Message = FString::Printf(TEXT("merged.txt is \"%s\", the reader writes \"%s\". Run the merge again."), *Merged.Format, *Current.Format);
			return File;
		}

		File.Rows = DiffMergeText(Current, Merged);
		const bool bHasChanges = File.Rows.ContainsByPredicate([](const FMergeRow& Row) { return Row.Action != EMergeAction::NotApplicable; });
		if (bAssetDirty)
		{
			File.Status = EMergeStatus::UnsavedChanges;
			File.Message = TEXT("The list is for the saved file. Save or revert the asset first.");
		}
		else
		{
			File.Status = bHasChanges ? EMergeStatus::Ready : EMergeStatus::Done;
		}
		return File;
	}

	TArray<FMergeFile> CheckMergeFiles()
	{
		TArray<FMergeFile> Files;
		const TArray<FMergeFileLocation> Locations = FindMergeFiles(GetMergeDir());
		if (Locations.IsEmpty())
		{
			return Files;
		}

		FString Reader;
		FString ReaderError;
		const bool bHasReader = FindReader(Reader, ReaderError);
		for (const FMergeFileLocation& Location : Locations)
		{
			FString AssetFile;
			if (FPackageName::DoesPackageExist(Location.PackageName, &AssetFile))
			{
				AssetFile = FPaths::ConvertRelativePathToFull(AssetFile);
			}
			else
			{
				AssetFile.Reset();
			}
			const UPackage* Package = FindPackage(nullptr, *Location.PackageName);
			const bool bDirty = Package && Package->IsDirty();

			FMergeFile& File = Files.Add_GetRef(CheckMergeFile(Location, AssetFile, bDirty,
				[&](const FString& InAssetFile, FString& OutText, FString& OutError)
				{
					if (!bHasReader)
					{
						OutError = ReaderError;
						return false;
					}
					return RunReader(Reader, InAssetFile, OutText, OutError);
				}));
			if (!bHasReader && File.Status == EMergeStatus::ReaderFailed)
			{
				File.Status = EMergeStatus::ReaderNotSetUp;
			}
		}
		return Files;
	}

	namespace
	{
		const TCHAR* NotYet = TEXT("Arrays, maps, sets and instanced structs can't be applied yet. Make the change by hand.");

		/** One property value in its own memory. */
		class FOwnedValue
		{
		public:
			explicit FOwnedValue(const FProperty* InProperty)
				: Property(InProperty)
				, Memory(InProperty->AllocateAndInitializeValue())
			{
			}
			~FOwnedValue() { Property->DestroyAndFreeValue(Memory); }
			FOwnedValue(const FOwnedValue&) = delete;
			FOwnedValue& operator=(const FOwnedValue&) = delete;

			void* Get() const { return Memory; }

		private:
			const FProperty* Property;
			void* Memory;
		};

		/** Class defaults, component templates and Designer widgets are edited as templates; a data asset as an instance. */
		bool IsTemplate(const UObject* Object)
		{
			if (Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
			{
				return true;
			}
			for (const UObject* Outer = Object->GetOuter(); Outer; Outer = Outer->GetOuter())
			{
				if (Outer->IsA<UWidgetTree>())
				{
					return true;
				}
			}
			return false;
		}

		/**
		 * The properties along a path such as "Pair.B": the first in the object's class, each next one a member of the struct
		 * before it. False with the reason if the value can't be applied.
		 */
		bool ResolvePath(const UObject* Object, const FString& Path, TArray<const FProperty*>& OutSteps, FString& OutError)
		{
			if (Path.Contains(TEXT("[")) || Path.Contains(TEXT("(")))
			{
				OutError = NotYet;
				return false;
			}
			TArray<FString> Names;
			Path.ParseIntoArray(Names, TEXT("."), false);
			if (Names.IsEmpty())
			{
				OutError = TEXT("The row has no value path.");
				return false;
			}

			const UStruct* Struct = Object->GetClass();
			const EPropertyFlags HiddenHere = IsTemplate(Object) ? CPF_DisableEditOnTemplate : CPF_DisableEditOnInstance;
			for (int32 Index = 0; Index < Names.Num(); ++Index)
			{
				const FString& Name = Names[Index];
				const FProperty* Property = Name.IsEmpty() ? nullptr : FindFProperty<FProperty>(Struct, FName(*Name));
				if (!Property)
				{
					OutError = FString::Printf(TEXT("%s has no property \"%s\". Was it renamed or removed?"), *Struct->GetName(), *Name);
					return false;
				}
				if (Property->ArrayDim != 1 || CastField<FArrayProperty>(Property) || CastField<FMapProperty>(Property) || CastField<FSetProperty>(Property))
				{
					OutError = NotYet;
					return false;
				}
				const EPropertyFlags Hidden = Index == 0 ? HiddenHere : CPF_None;
				if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated | Hidden))
				{
					OutError = FString::Printf(TEXT("%s can't be edited in the Details panel. Make the change by hand, or take the other side's file and merge again."),
						*Property->GetName());
					return false;
				}
				if (Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance))
				{
					OutError = FString::Printf(TEXT("%s holds an object of its own. Make the change by hand."), *Property->GetName());
					return false;
				}
				OutSteps.Add(Property);

				if (Index + 1 < Names.Num())
				{
					const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
					if (!StructProperty)
					{
						OutError = FString::Printf(TEXT("%s isn't a struct, so it has no \"%s\"."), *Property->GetName(), *Names[Index + 1]);
						return false;
					}
					if (StructProperty->Struct == FInstancedStruct::StaticStruct())
					{
						OutError = NotYet;
						return false;
					}
					Struct = StructProperty->Struct;
				}
			}
			return true;
		}

		/** The value at the path in a container: an object, or a value of the first step's property for Steps after the first. */
		void* GetValuePtr(const void* Container, TConstArrayView<const FProperty*> Steps)
		{
			const void* Ptr = Container;
			for (const FProperty* Step : Steps)
			{
				Ptr = Step->ContainerPtrToValuePtr<void>(Ptr);
			}
			return const_cast<void*>(Ptr);
		}

		bool HasProperty(const UObject* Object, const FProperty* Property)
		{
			const UStruct* Owner = Property->GetOwnerStruct();
			return Owner && Object->GetClass()->IsChildOf(Owner);
		}

		/**
		 * Archetype instances of Object (any depth) that still have Old at the path, and so does every archetype between them and
		 * Object. PropertyAccessUtil::GetArchetypeInstancesInheritingPropertyValue does the same for a whole property; this
		 * compares only the value at the path, so a child that overrides another member of the struct still gets this one.
		 */
		TArray<UObject*> FindInheritors(UObject* Object, TConstArrayView<const FProperty*> Steps, const void* Old)
		{
			TArray<UObject*> Inheritors;
			if (!Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
			{
				return Inheritors;
			}
			TArray<UObject*> Instances;
			Object->GetArchetypeInstances(Instances);

			const FProperty* Leaf = Steps.Last();
			TMap<const UObject*, bool> HasOldCache;
			auto HasOld = [&](const UObject* Candidate)
			{
				if (const bool* Known = HasOldCache.Find(Candidate))
				{
					return *Known;
				}
				const bool bHasOld = HasProperty(Candidate, Steps[0]) && Leaf->Identical(GetValuePtr(Candidate, Steps), Old, PPF_None);
				HasOldCache.Add(Candidate, bHasOld);
				return bHasOld;
			};
			for (UObject* Candidate : Instances)
			{
				const UObject* Iter = Candidate;
				while (Iter && Iter != Object && HasOld(Iter))
				{
					Iter = Iter->GetArchetype();
				}
				if (Iter == Object)
				{
					Inheritors.Add(Candidate);
				}
			}
			return Inheritors;
		}

		/** Writes the value at the path the way the Details panel does, so undo, editor refresh and construction scripts follow. */
		void WriteValue(UObject* Object, TConstArrayView<const FProperty*> Steps, const void* NewValue)
		{
			FProperty* Member = const_cast<FProperty*>(Steps[0]);
			FProperty* Leaf = const_cast<FProperty*>(Steps.Last());
			Object->Modify();
			Object->PreEditChange(Member);
			Leaf->CopyCompleteValue(GetValuePtr(Object, Steps), NewValue);
			FPropertyChangedEvent Event(Leaf, EPropertyChangeType::ValueSet);
			Event.SetActiveMemberProperty(Member);
			Object->PostEditChangeProperty(Event);
		}

		FRowResult ApplyRow(const UPackage* Package, const FMergeRow& Row)
		{
			FRowResult Result;
			if (Row.Action == EMergeAction::NotApplicable)
			{
				Result.Message = Row.Reason;
				return Result;
			}
			if (Row.Action == EMergeAction::Add || Row.Action == EMergeAction::Remove)
			{
				Result.Message = NotYet;
				return Result;
			}

			UObject* Object = FindObject<UObject>(nullptr, *Row.Object);
			if (!Object || Object->GetPackage() != Package)
			{
				Result.Message = FString::Printf(TEXT("The asset has no object %s."), *Row.Object);
				return Result;
			}
			TArray<const FProperty*> Steps;
			if (!ResolvePath(Object, Row.Path, Steps, Result.Message))
			{
				return Result;
			}
			const FProperty* Leaf = Steps.Last();
			const void* Current = GetValuePtr(Object, Steps);

			FOwnedValue NewValue(Leaf);
			if (Row.Action == EMergeAction::Set)
			{
				// Onto the current value: a struct text that lists only some fields leaves the others as they are.
				Leaf->CopyCompleteValue(NewValue.Get(), Current);
				FString Error;
				if (!Row.Merged.IsSet() || !ImportValue(Leaf, *Row.Merged, NewValue.Get(), Object, Error))
				{
					Result.Message = FString::Printf(TEXT("Can't read the merged value: %s"), *Error);
					return Result;
				}
			}
			else
			{
				const UObject* Archetype = Object->GetArchetype();
				if (Archetype && HasProperty(Archetype, Steps[0]))
				{
					Leaf->CopyCompleteValue(NewValue.Get(), GetValuePtr(Archetype, Steps));
				}
				else
				{
					// The archetype doesn't have the property (a variable this Blueprint adds): the property's default.
					FOwnedValue Default(Steps[0]);
					Leaf->CopyCompleteValue(NewValue.Get(), GetValuePtr(Default.Get(), MakeArrayView(Steps).RightChop(1)));
				}
			}

			if (Leaf->Identical(Current, NewValue.Get(), PPF_None))
			{
				Result.Result = ERowResult::Unchanged;
				return Result;
			}

			// Find the inheritors before writing: they are the ones that still have the old value.
			FOwnedValue OldValue(Leaf);
			Leaf->CopyCompleteValue(OldValue.Get(), Current);
			const TArray<UObject*> Inheritors = FindInheritors(Object, Steps, OldValue.Get());
			WriteValue(Object, Steps, NewValue.Get());
			for (UObject* Inheritor : Inheritors)
			{
				WriteValue(Inheritor, Steps, NewValue.Get());
			}
			Result.Result = ERowResult::Applied;
			return Result;
		}
	}

	const TCHAR* LexToString(ERowResult Result)
	{
		switch (Result)
		{
		case ERowResult::Applied: return TEXT("Applied");
		case ERowResult::Unchanged: return TEXT("Unchanged");
		case ERowResult::Failed: return TEXT("Failed");
		}
		return TEXT("");
	}

	TArray<FRowResult> ApplyRows(UObject* Asset, TConstArrayView<FMergeRow> Rows)
	{
		TArray<FRowResult> Results;
		bool bChanged = false;
		for (const FMergeRow& Row : Rows)
		{
			const FRowResult& Result = Results.Add_GetRef(ApplyRow(Asset->GetPackage(), Row));
			bChanged |= Result.Result == ERowResult::Applied;
		}
		if (bChanged)
		{
			MarkModified(Asset);
		}
		return Results;
	}

	namespace
	{
		FString DescribeValue(const TOptional<FString>& Value)
		{
			return Value.IsSet() ? *Value : FString(TEXT("(inherited)"));
		}

		void LogMergeFiles()
		{
			const TArray<FMergeFile> Files = CheckMergeFiles();
			UE_LOG(LogJsonBlueprintSync, Display, TEXT("%d merge file(s) in %s"), Files.Num(), *GetMergeDir());
			for (const FMergeFile& File : Files)
			{
				UE_LOG(LogJsonBlueprintSync, Display, TEXT("%s: %s%s%s"), *File.Location.PackageName, LexToString(File.Status),
					File.Message.IsEmpty() ? TEXT("") : TEXT(". "), *File.Message);
				for (const FMergeRow& Row : File.Rows)
				{
					UE_LOG(LogJsonBlueprintSync, Display, TEXT("  %s [%s] %s: %s -> %s%s%s"), LexToString(Row.Action), *Row.Object, *Row.Path,
						*DescribeValue(Row.Current), *DescribeValue(Row.Merged), Row.Reason.IsEmpty() ? TEXT("") : TEXT(". "), *Row.Reason);
				}
			}
		}

		FAutoConsoleCommand MergesCommand(
			TEXT("JsonBlueprintSync.Merges"),
			TEXT("Lists the asset merges saved by the merge tool (Saved/JsonBlueprintSync/Merge) and what each would change in its asset."),
			FConsoleCommandDelegate::CreateStatic(&LogMergeFiles));

		/** JsonBlueprintSync.ApplyMerge <package> [save]: every row of one Ready merge, in one undo step; with save, compile and save. */
		void ApplyMerge(const TArray<FString>& Args)
		{
			if (Args.IsEmpty())
			{
				UE_LOG(LogJsonBlueprintSync, Warning, TEXT("Usage: JsonBlueprintSync.ApplyMerge <package, e.g. /Game/A/BP_B> [save]"));
				return;
			}
			const FString& PackageName = Args[0];
			const bool bSave = Args.ContainsByPredicate([](const FString& Arg) { return Arg.Equals(TEXT("save"), ESearchCase::IgnoreCase); });

			const TArray<FMergeFile> Files = CheckMergeFiles();
			const FMergeFile* File = Files.FindByPredicate([&PackageName](const FMergeFile& Each) { return Each.Location.PackageName == PackageName; });
			if (!File)
			{
				UE_LOG(LogJsonBlueprintSync, Warning, TEXT("There's no merge for %s in %s."), *PackageName, *GetMergeDir());
				return;
			}
			if (!CanApply(File->Status))
			{
				UE_LOG(LogJsonBlueprintSync, Warning, TEXT("%s: %s%s%s"), *PackageName, LexToString(File->Status),
					File->Message.IsEmpty() ? TEXT("") : TEXT(". "), *File->Message);
				return;
			}
			UObject* Asset = LoadAsset(PackageName);
			if (!Asset)
			{
				UE_LOG(LogJsonBlueprintSync, Warning, TEXT("Can't load %s."), *PackageName);
				return;
			}

			TArray<FRowResult> Results;
			{
				FScopedTransaction Transaction(NSLOCTEXT("JsonBlueprintSync", "ApplyMerge", "Apply Merge"));
				Results = ApplyRows(Asset, File->Rows);
				if (!Results.ContainsByPredicate([](const FRowResult& Result) { return Result.Result == ERowResult::Applied; }))
				{
					Transaction.Cancel();
				}
			}
			int32 Applied = 0;
			for (int32 Index = 0; Index < Results.Num(); ++Index)
			{
				const FMergeRow& Row = File->Rows[Index];
				const FRowResult& Result = Results[Index];
				Applied += Result.Result == ERowResult::Applied ? 1 : 0;
				UE_LOG(LogJsonBlueprintSync, Display, TEXT("  %s: %s [%s] %s%s%s"), LexToString(Result.Result), LexToString(Row.Action), *Row.Object,
					*Row.Path, Result.Message.IsEmpty() ? TEXT("") : TEXT(". "), *Result.Message);
			}
			UE_LOG(LogJsonBlueprintSync, Display, TEXT("Applied %d of %d row(s) to %s."), Applied, Results.Num(), *PackageName);

			if (bSave && Applied > 0)
			{
				if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
				{
					FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
				}
				UPackage* Package = Asset->GetPackage();
				const FEditorFileUtils::EPromptReturnCode Saved = FEditorFileUtils::PromptForCheckoutAndSave({ Package }, true, false);
				// With -unattended the editor cancels every save that may need a source-control checkout.
				UE_LOG(LogJsonBlueprintSync, Display, TEXT("%s %s."), *PackageName,
					Saved == FEditorFileUtils::PR_Success && !Package->IsDirty() ? TEXT("saved")
					: FApp::IsUnattended() ? TEXT("was not saved: the editor doesn't save from commands in -unattended runs")
					: TEXT("was not saved"));
			}
		}

		FAutoConsoleCommand ApplyMergeCommand(
			TEXT("JsonBlueprintSync.ApplyMerge"),
			TEXT("JsonBlueprintSync.ApplyMerge <package> [save]: applies the rows of a Ready merge to its asset; with save, compiles and saves it."),
			FConsoleCommandWithArgsDelegate::CreateStatic(&ApplyMerge));
	}
}
