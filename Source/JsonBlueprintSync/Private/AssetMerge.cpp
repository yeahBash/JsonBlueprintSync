#include "AssetMerge.h"

#include "JsonBlueprintSync.h"
#include "JsonBlueprintSyncModule.h"
#include "Blueprint/WidgetTree.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/StructOnScope.h"
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
		const TCHAR* TypeSuffix = TEXT(".(type)");

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
			// Instanced structs whose type changes: applying the type gives their members new values, so every member line
			// becomes a row, also the ones that read the same on both sides. The reader writes the type before the members.
			TArray<FString> RetypedPrefixes;
			for (const FMergeLine& Line : Merged.Lines)
			{
				MergedPaths.Add(Line.Path);
				const FMergeLine* const* CurrentLine = CurrentLines.Find(Line.Path);
				if (CurrentLine && (*CurrentLine)->Value == Line.Value
					&& !RetypedPrefixes.ContainsByPredicate([&Line](const FString& Prefix) { return Line.Path.StartsWith(Prefix, ESearchCase::CaseSensitive); }))
				{
					continue;
				}
				if (Line.Path.EndsWith(TypeSuffix, ESearchCase::CaseSensitive))
				{
					RetypedPrefixes.Add(Line.Path.LeftChop(FCString::Strlen(TypeSuffix) - 1));
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
		return CheckMergeFiles(FindMergeFiles(GetMergeDir()), true);
	}

	TArray<FMergeFile> CheckMergeFiles(TConstArrayView<FMergeFileLocation> Locations, bool bCheckUnsaved)
	{
		TArray<FMergeFile> Files;
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
			const bool bDirty = bCheckUnsaved && Package && Package->IsDirty();

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
		/** One property value in its own memory (all elements of a static array). */
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

		bool HasProperty(const UObject* Object, const FProperty* Property)
		{
			const UStruct* Owner = Property->GetOwnerStruct();
			return Owner && Object->GetClass()->IsChildOf(Owner);
		}

		/**
		 * Text the reader quoted (QuoteText: \" \\ \n \r \t), starting at the quote at Pos. Pos ends after the closing quote.
		 */
		bool ReadQuoted(const FString& Text, int32& Pos, FString& Out)
		{
			if (Pos >= Text.Len() || Text[Pos] != TEXT('"'))
			{
				return false;
			}
			++Pos;
			while (Pos < Text.Len())
			{
				const TCHAR Char = Text[Pos++];
				if (Char == TEXT('"'))
				{
					return true;
				}
				if (Char == TEXT('\\') && Pos < Text.Len())
				{
					const TCHAR Escaped = Text[Pos++];
					Out += Escaped == TEXT('n') ? TEXT('\n') : Escaped == TEXT('r') ? TEXT('\r') : Escaped == TEXT('t') ? TEXT('\t') : Escaped;
				}
				else
				{
					Out += Char;
				}
			}
			return false;
		}

		/**
		 * A merge text value into a value of Property. The reader quotes strings and names; Unreal's import, not delimited,
		 * would keep the quotes as part of them.
		 */
		bool ImportMergeValue(const FProperty* Property, const FString& Text, void* Dest, UObject* Owner, FString& OutError)
		{
			if ((Property->IsA<FStrProperty>() || Property->IsA<FNameProperty>()) && Text.StartsWith(TEXT("\""), ESearchCase::CaseSensitive))
			{
				int32 Pos = 0;
				FString Unquoted;
				if (!ReadQuoted(Text, Pos, Unquoted) || Pos != Text.Len())
				{
					OutError = FString::Printf(TEXT("cannot parse '%s'"), *Text);
					return false;
				}
				return ImportValue(Property, Unquoted, Dest, Owner, OutError);
			}
			return ImportValue(Property, Text, Dest, Owner, OutError);
		}

		/** A part of a merge text path: Pair.B, Arr[2].B, Map["k"], Set["C"], Instance.(type). */
		struct FPathToken
		{
			enum class EKind : uint8 { Name, Index, Key, Special };
			EKind Kind = EKind::Name;
			/** Name: the name. Key: the key, unescaped. Special: "(type)" or "(devnotes)". */
			FString Text;
			/** Key: as written, with the quotes. */
			FString Quoted;
			int32 Index = 0;
		};

		bool SplitPath(const FString& Path, TArray<FPathToken>& Out)
		{
			const int32 Len = Path.Len();
			int32 Pos = 0;
			auto AddName = [&Path, &Out, &Pos, Len]()
			{
				const int32 Start = Pos;
				while (Pos < Len && Path[Pos] != TEXT('.') && Path[Pos] != TEXT('['))
				{
					++Pos;
				}
				FPathToken& Token = Out.AddDefaulted_GetRef();
				Token.Text = Path.Mid(Start, Pos - Start);
				return !Token.Text.IsEmpty();
			};
			if (!AddName())
			{
				return false;
			}
			while (Pos < Len)
			{
				const TCHAR Char = Path[Pos++];
				if (Char == TEXT('.'))
				{
					if (Pos < Len && Path[Pos] == TEXT('('))
					{
						const int32 Close = Path.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
						if (Close == INDEX_NONE)
						{
							return false;
						}
						FPathToken& Token = Out.AddDefaulted_GetRef();
						Token.Kind = FPathToken::EKind::Special;
						Token.Text = Path.Mid(Pos, Close + 1 - Pos);
						Pos = Close + 1;
					}
					else if (!AddName())
					{
						return false;
					}
				}
				else if (Char == TEXT('[') && Pos < Len && Path[Pos] == TEXT('"'))
				{
					// A key as the reader quotes it, with backslash escapes, so "]" and "." inside belong to the key.
					const int32 Start = Pos;
					FString Key;
					if (!ReadQuoted(Path, Pos, Key) || Pos >= Len || Path[Pos] != TEXT(']'))
					{
						return false;
					}
					FPathToken& Token = Out.AddDefaulted_GetRef();
					Token.Kind = FPathToken::EKind::Key;
					Token.Text = MoveTemp(Key);
					Token.Quoted = Path.Mid(Start, Pos - Start);
					++Pos;
				}
				else if (Char == TEXT('['))
				{
					const int32 Close = Path.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
					const FString Digits = Close == INDEX_NONE ? FString() : Path.Mid(Pos, Close - Pos);
					FPathToken Token;
					Token.Kind = FPathToken::EKind::Index;
					if (Digits.IsEmpty() || !FChar::IsDigit(Digits[0]) || !LexTryParseString(Token.Index, *Digits) || Token.Index < 0)
					{
						return false;
					}
					Out.Add(MoveTemp(Token));
					Pos = Close + 1;
				}
				else
				{
					return false;
				}
			}
			return true;
		}

		enum class EStepKind : uint8 { Member, Element, Entry, Instance };

		/** One step from a value into a part of it. */
		struct FStep
		{
			EStepKind Kind = EStepKind::Member;
			/** Member: the property. Element: the array. Entry: the map. Instance: the FInstancedStruct property. */
			const FProperty* Property = nullptr;
			/** Member: the index in a static array. Element: the array index. */
			int32 Index = 0;
			/** Entry: the key, a value of the map's key property. */
			TSharedPtr<FOwnedValue> Key;
			/** Instance: the struct that the instanced struct holds in the object the row is for. */
			const UScriptStruct* Struct = nullptr;
		};

		/** What a row changes at the end of its steps. */
		enum class ELeaf : uint8
		{
			/** The value. For a map entry also whether the entry is there. */
			Value,
			/** "Array.Num": the size. The value compared and written is the whole array. */
			Num,
			/** "Instance.(type)": the struct type. The value compared and written is the whole instanced struct. */
			Type,
			/** "Set[element]": whether the set has the element. */
			SetElement,
		};

		struct FLocation
		{
			TArray<FStep> Steps;
			ELeaf Leaf = ELeaf::Value;
			/** The property of the value at the end of the steps (one element of a static array). */
			const FProperty* ValueProperty = nullptr;
			/** SetElement: the element, a value of the set's element property. */
			TSharedPtr<FOwnedValue> Element;
		};

		/** The object's property: for PreEditChange and as the member property of the change event. */
		FProperty* GetMemberProperty(const FLocation& Location)
		{
			return const_cast<FProperty*>(Location.Steps[0].Property);
		}

		/** The innermost named property on the way, for the change event. */
		FProperty* GetChangedProperty(const FLocation& Location)
		{
			for (int32 Index = Location.Steps.Num() - 1; Index >= 0; --Index)
			{
				if (Location.Steps[Index].Kind == EStepKind::Member)
				{
					return const_cast<FProperty*>(Location.Steps[Index].Property);
				}
			}
			return GetMemberProperty(Location);
		}

		/** The location of the map entry at step Index. */
		FLocation GetEntryLocation(const FLocation& Location, int32 Index)
		{
			FLocation Entry;
			Entry.Steps.Append(Location.Steps.GetData(), Index + 1);
			Entry.ValueProperty = CastFieldChecked<FMapProperty>(Location.Steps[Index].Property)->ValueProp;
			return Entry;
		}

		bool ReadKey(const FProperty* KeyProperty, const FPathToken& Token, UObject* Owner, TSharedPtr<FOwnedValue>& Out, FString& OutError)
		{
			Out = MakeShared<FOwnedValue>(KeyProperty);
			// Unquoted, the key is the text Unreal imports: a string or name as it is, other keys as their export text.
			FString Error;
			if (!ImportValue(KeyProperty, Token.Text, Out->Get(), Owner, Error))
			{
				OutError = FString::Printf(TEXT("Can't read the key %s: %s"), *Token.Quoted, *Error);
				return false;
			}
			return true;
		}

		enum class EResolved : uint8
		{
			Ok,
			/** The path goes into an instanced struct the object doesn't have here, or whose type has no such member. */
			Gone,
			Failed,
		};

		/**
		 * The steps of a path in Object. Only values the Details panel can edit, and not objects of their own (instanced
		 * subobjects). Object's values are read only to see what type an instanced struct holds.
		 */
		EResolved ResolvePath(UObject* Object, const FString& Path, FLocation& Out, FString& OutError)
		{
			TArray<FPathToken> Tokens;
			if (!SplitPath(Path, Tokens))
			{
				OutError = FString::Printf(TEXT("Can't read the path \"%s\"."), *Path);
				return EResolved::Failed;
			}
			auto Fail = [&OutError](FString Message)
			{
				OutError = MoveTemp(Message);
				return EResolved::Failed;
			};
			auto OwnsObjects = [](const FProperty* Property)
			{
				return Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance);
			};
			auto OwnsObjectsMessage = [](const FString& Name)
			{
				return FString::Printf(TEXT("%s holds objects of its own. Make the change by hand."), *Name);
			};

			const EPropertyFlags HiddenHere = IsTemplate(Object) ? CPF_DisableEditOnTemplate : CPF_DisableEditOnInstance;
			const UStruct* Struct = Object->GetClass();
			// The value reached so far, or null when the next name is a member of Struct.
			const FProperty* Value = nullptr;
			FString ValueName;
			// The value in Object, or null where Object doesn't have it.
			void* Ptr = Object;
			bool bInInstance = false;
			for (int32 Index = 0; Index < Tokens.Num();)
			{
				const FPathToken& Token = Tokens[Index];
				const bool bLast = Index == Tokens.Num() - 1;
				if (!Value)
				{
					if (Token.Kind != FPathToken::EKind::Name)
					{
						return Fail(FString::Printf(TEXT("Can't read the path \"%s\"."), *Path));
					}
					const FProperty* Property = FindFProperty<FProperty>(Struct, FName(*Token.Text));
					if (!Property)
					{
						OutError = FString::Printf(TEXT("%s has no property \"%s\". Was it renamed or removed?"), *Struct->GetName(), *Token.Text);
						return bInInstance ? EResolved::Gone : EResolved::Failed;
					}
					const EPropertyFlags Hidden = Out.Steps.IsEmpty() ? HiddenHere : CPF_None;
					if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated | Hidden))
					{
						return Fail(FString::Printf(TEXT("%s can't be edited in the Details panel. Make the change by hand, or take the other side's file and merge again."),
							*Property->GetName()));
					}
					if (OwnsObjects(Property))
					{
						return Fail(FString::Printf(TEXT("%s holds an object of its own. Make the change by hand."), *Property->GetName()));
					}
					FStep& Step = Out.Steps.AddDefaulted_GetRef();
					Step.Property = Property;
					++Index;
					if (Property->ArrayDim > 1 && Index < Tokens.Num() && Tokens[Index].Kind == FPathToken::EKind::Index)
					{
						// A static array: the reader writes element 0 as "Name" and the others as "Name[i]".
						Step.Index = Tokens[Index].Index;
						if (Step.Index >= Property->ArrayDim)
						{
							return Fail(FString::Printf(TEXT("%s has only %d elements."), *Property->GetName(), Property->ArrayDim));
						}
						++Index;
					}
					Ptr = Ptr ? Property->ContainerPtrToValuePtr<void>(Ptr, Step.Index) : nullptr;
					Value = Property;
					ValueName = Property->GetName();
					continue;
				}

				if (const FArrayProperty* Array = CastField<FArrayProperty>(Value))
				{
					if (OwnsObjects(Array->Inner))
					{
						return Fail(OwnsObjectsMessage(ValueName));
					}
					if (bLast && Token.Kind == FPathToken::EKind::Name && Token.Text == TEXT("Num"))
					{
						Out.Leaf = ELeaf::Num;
						break;
					}
					if (Token.Kind != FPathToken::EKind::Index)
					{
						return Fail(FString::Printf(TEXT("%s is an array, so [index] or .Num comes after it."), *ValueName));
					}
					FStep& Step = Out.Steps.AddDefaulted_GetRef();
					Step.Kind = EStepKind::Element;
					Step.Property = Array;
					Step.Index = Token.Index;
					if (Ptr)
					{
						FScriptArrayHelper Helper(Array, Ptr);
						Ptr = Helper.IsValidIndex(Token.Index) ? Helper.GetRawPtr(Token.Index) : nullptr;
					}
					Value = Array->Inner;
					++Index;
					continue;
				}
				if (const FMapProperty* Map = CastField<FMapProperty>(Value))
				{
					if (OwnsObjects(Map->KeyProp) || OwnsObjects(Map->ValueProp))
					{
						return Fail(OwnsObjectsMessage(ValueName));
					}
					if (Token.Kind != FPathToken::EKind::Key)
					{
						return Fail(FString::Printf(TEXT("%s is a map, so [\"key\"] comes after it."), *ValueName));
					}
					FStep& Step = Out.Steps.AddDefaulted_GetRef();
					Step.Kind = EStepKind::Entry;
					Step.Property = Map;
					if (!ReadKey(Map->KeyProp, Token, Object, Step.Key, OutError))
					{
						return EResolved::Failed;
					}
					Ptr = Ptr ? FScriptMapHelper(Map, Ptr).FindValueFromHash(Step.Key->Get()) : nullptr;
					Value = Map->ValueProp;
					++Index;
					continue;
				}
				if (const FSetProperty* Set = CastField<FSetProperty>(Value))
				{
					if (OwnsObjects(Set->ElementProp))
					{
						return Fail(OwnsObjectsMessage(ValueName));
					}
					if (!bLast || Token.Kind != FPathToken::EKind::Key)
					{
						return Fail(FString::Printf(TEXT("%s is a set, so only [\"element\"] comes after it."), *ValueName));
					}
					if (!ReadKey(Set->ElementProp, Token, Object, Out.Element, OutError))
					{
						return EResolved::Failed;
					}
					Out.Leaf = ELeaf::SetElement;
					break;
				}
				if (const FStructProperty* StructProperty = CastField<FStructProperty>(Value))
				{
					if (StructProperty->Struct == FInstancedStruct::StaticStruct())
					{
						if (bLast && Token.Kind == FPathToken::EKind::Special && Token.Text == TEXT("(type)"))
						{
							Out.Leaf = ELeaf::Type;
							break;
						}
						if (Token.Kind != FPathToken::EKind::Name)
						{
							return Fail(FString::Printf(TEXT("%s is an instanced struct, so .(type) or .Member comes after it."), *ValueName));
						}
						FInstancedStruct* Instance = static_cast<FInstancedStruct*>(Ptr);
						const UScriptStruct* Type = Instance ? Instance->GetScriptStruct() : nullptr;
						if (!Type)
						{
							OutError = FString::Printf(TEXT("%s holds no struct here, so it has no \"%s\"."), *ValueName, *Token.Text);
							return EResolved::Gone;
						}
						FStep& Step = Out.Steps.AddDefaulted_GetRef();
						Step.Kind = EStepKind::Instance;
						Step.Property = StructProperty;
						Step.Struct = Type;
						Ptr = Instance->GetMutableMemory();
						Struct = Type;
						bInInstance = true;
					}
					else
					{
						if (Token.Kind != FPathToken::EKind::Name)
						{
							return Fail(FString::Printf(TEXT("%s is a struct, so .Member comes after it."), *ValueName));
						}
						Struct = StructProperty->Struct;
					}
					// The name is read as a member of Struct.
					Value = nullptr;
					continue;
				}
				if (Token.Kind == FPathToken::EKind::Special && Token.Text == TEXT("(devnotes)"))
				{
					return Fail(TEXT("A text's developer notes can't be applied. Change them by hand."));
				}
				return Fail(FString::Printf(TEXT("%s isn't a struct or a container, so nothing comes after it."), *ValueName));
			}
			Out.ValueProperty = Value;
			return EResolved::Ok;
		}

		enum class EPresence : uint8
		{
			/** The path leads nowhere in the object: no such property, array element, or instanced struct type. */
			Missing,
			/** A map entry on the way, or the set element, isn't there. */
			Absent,
			Present,
		};

		/** Memory for the defaults that stand in during a walk. */
		struct FStandIns
		{
			TArray<TSharedPtr<FOwnedValue>> Values;
			TArray<TSharedPtr<FStructOnScope>> Structs;

			void* Add(const FProperty* Property) { return Values.Add_GetRef(MakeShared<FOwnedValue>(Property))->Get(); }
			void* Add(const UScriptStruct* Struct) { return Structs.Add_GetRef(MakeShared<FStructOnScope>(Struct))->GetStructMemory(); }
		};

		enum class EWalk : uint8
		{
			Read,
			/** Adds the map entries on the way that aren't there, with default values. */
			Write,
			/**
			 * For the value a missing line stands for: where the object (which may be null) doesn't have the property, the array
			 * element or the instanced struct's type, the default value of what would be there. Map entries stay absent.
			 */
			Defaults,
		};

		struct FWalked
		{
			EPresence Presence = EPresence::Missing;
			void* Ptr = nullptr;
			/** Absent: the step of the map entry that isn't there. */
			int32 AbsentStep = INDEX_NONE;
		};

		/** Follows the first NumSteps steps (all for INDEX_NONE) in Object. */
		FWalked Walk(UObject* Object, const FLocation& Location, EWalk Mode, FStandIns& StandIns, int32 NumSteps = INDEX_NONE)
		{
			FWalked Result;
			const int32 Count = NumSteps == INDEX_NONE ? Location.Steps.Num() : NumSteps;
			void* Ptr = Object;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FStep& Step = Location.Steps[Index];
				switch (Step.Kind)
				{
				case EStepKind::Member:
					if (Index == 0 && (!Object || !HasProperty(Object, Step.Property)))
					{
						if (Mode != EWalk::Defaults)
						{
							return Result;
						}
						Ptr = static_cast<uint8*>(StandIns.Add(Step.Property)) + Step.Property->GetElementSize() * Step.Index;
					}
					else
					{
						Ptr = Step.Property->ContainerPtrToValuePtr<void>(Ptr, Step.Index);
					}
					break;
				case EStepKind::Element:
				{
					const FArrayProperty* Array = CastFieldChecked<FArrayProperty>(Step.Property);
					FScriptArrayHelper Helper(Array, Ptr);
					if (Helper.IsValidIndex(Step.Index))
					{
						Ptr = Helper.GetRawPtr(Step.Index);
					}
					else if (Mode == EWalk::Defaults)
					{
						Ptr = StandIns.Add(Array->Inner);
					}
					else
					{
						return Result;
					}
					break;
				}
				case EStepKind::Entry:
				{
					FScriptMapHelper Helper(CastFieldChecked<FMapProperty>(Step.Property), Ptr);
					void* Value = Helper.FindValueFromHash(Step.Key->Get());
					if (!Value && Mode == EWalk::Write)
					{
						Value = Helper.FindOrAdd(Step.Key->Get());
					}
					if (!Value)
					{
						Result.Presence = EPresence::Absent;
						Result.AbsentStep = Index;
						return Result;
					}
					Ptr = Value;
					break;
				}
				case EStepKind::Instance:
				{
					FInstancedStruct* Instance = static_cast<FInstancedStruct*>(Ptr);
					if (Instance->GetScriptStruct() == Step.Struct)
					{
						Ptr = Instance->GetMutableMemory();
					}
					else if (Mode == EWalk::Defaults)
					{
						Ptr = StandIns.Add(Step.Struct);
					}
					else
					{
						return Result;
					}
					break;
				}
				}
			}
			Result.Presence = EPresence::Present;
			Result.Ptr = Ptr;
			return Result;
		}

		/** What an object has at a location. */
		struct FState
		{
			EPresence Presence = EPresence::Missing;
			/** A copy of the value, if present (not for a set element). */
			TSharedPtr<FOwnedValue> Value;
			int32 AbsentStep = INDEX_NONE;
		};

		FState ReadState(UObject* Object, const FLocation& Location, EWalk Mode = EWalk::Read)
		{
			FStandIns StandIns;
			const FWalked Walked = Walk(Object, Location, Mode, StandIns);
			FState State;
			State.Presence = Walked.Presence;
			State.AbsentStep = Walked.AbsentStep;
			if (Walked.Presence != EPresence::Present)
			{
				return State;
			}
			if (Location.Leaf == ELeaf::SetElement)
			{
				const FScriptSetHelper Helper(CastFieldChecked<FSetProperty>(Location.ValueProperty), Walked.Ptr);
				if (Helper.FindElementIndexFromHash(Location.Element->Get()) == INDEX_NONE)
				{
					State.Presence = EPresence::Absent;
				}
				return State;
			}
			State.Value = MakeShared<FOwnedValue>(Location.ValueProperty);
			Location.ValueProperty->CopySingleValue(State.Value->Get(), Walked.Ptr);
			return State;
		}

		bool SameState(const FLocation& Location, const FState& A, const FState& B)
		{
			if (A.Presence == EPresence::Missing || A.Presence != B.Presence || A.AbsentStep != B.AbsentStep)
			{
				return false;
			}
			return !A.Value || !B.Value || Location.ValueProperty->Identical(A.Value->Get(), B.Value->Get(), PPF_None);
		}

		/**
		 * Archetype instances of Object (any depth) that still have Old at the location, and so does every archetype between them
		 * and Object. PropertyAccessUtil::GetArchetypeInstancesInheritingPropertyValue does the same for a whole property; this
		 * compares only the value at the location, so a child that overrides another member, element or entry still gets this one.
		 */
		TArray<UObject*> FindInheritors(UObject* Object, const FLocation& Location, const FState& Old)
		{
			TArray<UObject*> Inheritors;
			if (!Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
			{
				return Inheritors;
			}
			TArray<UObject*> Instances;
			Object->GetArchetypeInstances(Instances);

			TMap<const UObject*, bool> HasOldCache;
			auto HasOld = [&](UObject* Candidate)
			{
				if (const bool* Known = HasOldCache.Find(Candidate))
				{
					return *Known;
				}
				const bool bHasOld = SameState(Location, ReadState(Candidate, Location), Old);
				HasOldCache.Add(Candidate, bHasOld);
				return bHasOld;
			};
			for (UObject* Candidate : Instances)
			{
				UObject* Iter = Candidate;
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

		/** Writes the state the way the Details panel does, so undo, editor refresh and construction scripts follow. */
		void WriteState(UObject* Object, const FLocation& Location, const FState& New)
		{
			FProperty* Member = GetMemberProperty(Location);
			Object->Modify();
			Object->PreEditChange(Member);
			FStandIns StandIns;
			const FStep& Last = Location.Steps.Last();
			if (Location.Leaf == ELeaf::Value && New.Presence != EPresence::Present)
			{
				// Only a map entry can be absent: remove it.
				const FWalked Map = Walk(Object, Location, EWalk::Read, StandIns, Location.Steps.Num() - 1);
				if (Map.Presence == EPresence::Present && Last.Kind == EStepKind::Entry)
				{
					FScriptMapHelper(CastFieldChecked<FMapProperty>(Last.Property), Map.Ptr).RemovePair(Last.Key->Get());
				}
			}
			else
			{
				const FWalked Walked = Walk(Object, Location, EWalk::Write, StandIns);
				if (Walked.Presence == EPresence::Present && Location.Leaf == ELeaf::SetElement)
				{
					FScriptSetHelper Helper(CastFieldChecked<FSetProperty>(Location.ValueProperty), Walked.Ptr);
					const bool bHas = Helper.FindElementIndexFromHash(Location.Element->Get()) != INDEX_NONE;
					if (New.Presence == EPresence::Present && !bHas)
					{
						Helper.AddElement(Location.Element->Get());
					}
					else if (New.Presence != EPresence::Present && bHas)
					{
						Helper.RemoveElement(Location.Element->Get());
					}
				}
				else if (Walked.Presence == EPresence::Present && New.Value)
				{
					Location.ValueProperty->CopySingleValue(Walked.Ptr, New.Value->Get());
				}
			}
			FPropertyChangedEvent Event(GetChangedProperty(Location), EPropertyChangeType::ValueSet);
			Event.SetActiveMemberProperty(Member);
			Object->PostEditChangeProperty(Event);
		}

		/** The state the row asks for. False with the reason if it can't be made. */
		bool MakeNewState(UObject* Object, const FLocation& Location, const FMergeRow& Row, const FState& Current, FState& New, FString& OutError)
		{
			const FProperty* Property = Location.ValueProperty;
			// The value to change: the current one, or a default one where a map entry isn't there yet.
			auto StartValue = [Property, &Current]()
			{
				TSharedPtr<FOwnedValue> Value = MakeShared<FOwnedValue>(Property);
				if (Current.Value)
				{
					Property->CopySingleValue(Value->Get(), Current.Value->Get());
				}
				return Value;
			};
			const bool bEntry = Location.Leaf == ELeaf::Value && Location.Steps.Last().Kind == EStepKind::Entry;

			switch (Row.Action)
			{
			case EMergeAction::Add:
				if (Location.Leaf != ELeaf::SetElement)
				{
					OutError = TEXT("Only a set element can be (added).");
					return false;
				}
				New.Presence = EPresence::Present;
				return true;
			case EMergeAction::Remove:
				if (Location.Leaf != ELeaf::SetElement && !bEntry)
				{
					OutError = TEXT("Only a map entry or a set element can be (removed).");
					return false;
				}
				New.Presence = EPresence::Absent;
				New.AbsentStep = bEntry ? Location.Steps.Num() - 1 : INDEX_NONE;
				return true;
			case EMergeAction::Reset:
				// What the object would have without its own line: the archetype's value at the same location. For an array's
				// Num or an instanced struct's type that's the whole array or instanced struct, since Unreal saves those whole.
				New = ReadState(Object->GetArchetype(), Location, EWalk::Defaults);
				return true;
			case EMergeAction::Set:
				break;
			case EMergeAction::NotApplicable:
				OutError = Row.Reason;
				return false;
			}

			if (!Row.Merged.IsSet())
			{
				OutError = TEXT("The row has no merged value.");
				return false;
			}
			const FString& Text = *Row.Merged;
			New.Presence = EPresence::Present;
			switch (Location.Leaf)
			{
			case ELeaf::Value:
			{
				if (Text == TEXT("()"))
				{
					// The reader's line for a map entry whose struct value has only default members.
					New.Value = MakeShared<FOwnedValue>(Property);
					return true;
				}
				New.Value = StartValue();
				FString Error;
				if (!ImportMergeValue(Property, Text, New.Value->Get(), Object, Error))
				{
					OutError = FString::Printf(TEXT("Can't read the merged value: %s"), *Error);
					return false;
				}
				return true;
			}
			case ELeaf::Num:
			{
				int32 Num = 0;
				if (!LexTryParseString(Num, *Text) || Num < 0 || Num > (1 << 20))
				{
					OutError = FString::Printf(TEXT("Can't read the merged size \"%s\"."), *Text);
					return false;
				}
				// New elements get default values; the rows for them follow.
				New.Value = StartValue();
				FScriptArrayHelper(CastFieldChecked<FArrayProperty>(Property), New.Value->Get()).Resize(Num);
				return true;
			}
			case ELeaf::Type:
			{
				const UScriptStruct* Type = nullptr;
				if (Text != TEXT("None"))
				{
					// Native structs are loaded with their module; a user-defined struct may still need loading.
					Type = FindObject<UScriptStruct>(nullptr, *Text);
					if (!Type && !Text.StartsWith(TEXT("/Script/"), ESearchCase::CaseSensitive) && FPackageName::IsValidObjectPath(Text))
					{
						Type = LoadObject<UScriptStruct>(nullptr, *Text, nullptr, LOAD_NoWarn | LOAD_Quiet);
					}
					if (!Type)
					{
						OutError = FString::Printf(TEXT("There is no struct %s. Was it renamed or removed?"), *Text);
						return false;
					}
				}
				New.Value = StartValue();
				FInstancedStruct& Instance = *static_cast<FInstancedStruct*>(New.Value->Get());
				if (Instance.GetScriptStruct() != Type)
				{
					// Members without a line are the archetype's if it holds the same type, else the struct's defaults, as Unreal
					// saves them. The rows for the members follow.
					const FState Inherited = ReadState(Object->GetArchetype(), Location, EWalk::Defaults);
					const FInstancedStruct* InheritedInstance = Inherited.Value ? static_cast<const FInstancedStruct*>(Inherited.Value->Get()) : nullptr;
					if (Type && InheritedInstance && InheritedInstance->GetScriptStruct() == Type)
					{
						Instance = *InheritedInstance;
					}
					else
					{
						Instance.InitializeAs(Type);
					}
				}
				return true;
			}
			case ELeaf::SetElement:
				OutError = TEXT("A set element is (added) or (removed).");
				return false;
			}
			return false;
		}

		FRowResult ApplyRow(const UPackage* Package, const FMergeRow& Row)
		{
			FRowResult Result;
			if (Row.Action == EMergeAction::NotApplicable)
			{
				Result.Message = Row.Reason;
				return Result;
			}
			UObject* Object = FindObject<UObject>(nullptr, *Row.Object);
			if (!Object || Object->GetPackage() != Package)
			{
				Result.Message = FString::Printf(TEXT("The asset has no object %s."), *Row.Object);
				return Result;
			}

			const TCHAR* Gone = TEXT("The asset doesn't have this value now, which is what the merge asks for.");
			const TCHAR* NotThereYet = TEXT("The asset doesn't have this value yet. Apply the row that makes room for it first: the array's Num, or the instanced struct's type.");
			FLocation Location;
			switch (ResolvePath(Object, Row.Path, Location, Result.Message))
			{
			case EResolved::Failed:
				return Result;
			case EResolved::Gone:
				if (Row.Action == EMergeAction::Reset)
				{
					Result.Result = ERowResult::Unchanged;
					Result.Message = Gone;
				}
				return Result;
			case EResolved::Ok:
				break;
			}

			FState Current = ReadState(Object, Location);
			if (Current.Presence == EPresence::Missing)
			{
				// An element past the end of the array, for instance: a reset of it is already done, a value for it needs room.
				if (Row.Action == EMergeAction::Reset)
				{
					Result.Result = ERowResult::Unchanged;
					Result.Message = Gone;
				}
				else
				{
					Result.Message = NotThereYet;
				}
				return Result;
			}
			FState New;
			if (!MakeNewState(Object, Location, Row, Current, New, Result.Message))
			{
				return Result;
			}
			if (New.Presence == EPresence::Absent && New.AbsentStep != INDEX_NONE
				&& (New.AbsentStep < Location.Steps.Num() - 1 || Location.Leaf != ELeaf::Value))
			{
				// The archetype doesn't have the map entry the value is in, so the entry goes.
				Location = GetEntryLocation(Location, New.AbsentStep);
				Current = ReadState(Object, Location);
			}
			if (SameState(Location, Current, New))
			{
				Result.Result = ERowResult::Unchanged;
				return Result;
			}

			// Find the inheritors before writing: they are the ones that still have the old state.
			const TArray<UObject*> Inheritors = FindInheritors(Object, Location, Current);
			WriteState(Object, Location, New);
			for (UObject* Inheritor : Inheritors)
			{
				WriteState(Inheritor, Location, New);
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
}
