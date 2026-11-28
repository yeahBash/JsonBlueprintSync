# JsonBlueprintSync internals

How the plugin is built and how its parts work together, file by file, with the caveats and corner cases of each. For what the
plugin does from a user's point of view, see [README.md](../README.md). To check it by hand, see [ManualTests.md](ManualTests.md).

- [1. Overview](#1-overview)
- [2. Files](#2-files)
  - [2.1 JsonBlueprintSync.uplugin](#21-jsonblueprintsyncuplugin)
  - [2.2 JsonBlueprintSync.Build.cs](#22-jsonblueprintsyncbuildcs)
  - [2.3 Module: JsonBlueprintSyncModule](#23-module-jsonblueprintsyncmodule)
  - [2.4 Settings: JsonBlueprintSyncSettings](#24-settings-jsonblueprintsyncsettings)
  - [2.5 Library: JsonBlueprintSync](#25-library-jsonblueprintsync)
  - [2.6 Sync state: SyncState](#26-sync-state-syncstate)
  - [2.7 Editor subsystem: JsonBlueprintSyncSubsystem](#27-editor-subsystem-jsonblueprintsyncsubsystem)
  - [2.8 The JSON Sync tab: SJsonSyncPanel](#28-the-json-sync-tab-sjsonsyncpanel)
  - [2.9 Commandlet: JsonBlueprintSyncCommandlet](#29-commandlet-jsonblueprintsynccommandlet)
  - [2.10 Automation tests](#210-automation-tests)
- [3. End-to-end flows](#3-end-to-end-flows)
- [4. Life of one asset's sync state](#4-life-of-one-assets-sync-state)
- [5. Corner cases](#5-corner-cases)
- [6. Weak spots](#6-weak-spots)

## 1. Overview

The plugin is a single editor module, `JsonBlueprintSync`. It is never built into a game target. It syncs **data assets** only, and
the JSON is the source of truth.

```
            ┌──────────────────┐        ┌───────────────────────────┐
            │  SJsonSyncPanel  │───────▶│ UJsonBlueprintSyncSubsystem│◀── editor events: package saved,
            │  (Window > JSON  │◀───────│ (editor runtime, pending   │    asset renamed/removed,
            │   Sync tab)      │ OnPendingChanged  state, save guard)│    files loaded, console command
            └──────────────────┘        └─────────────┬─────────────┘
                                                      │
 ┌─────────────────────────────┐        ┌─────────────▼─────────────┐
 │ UJsonBlueprintSyncCommandlet│───────▶│ SyncState                 │  DiffKeys, TakeKeys, FSyncCache
 │ (-export / -import / -verify)│        │ (three-way diff, local    │  (Saved/JsonBlueprintSync/)
 └──────────────┬──────────────┘        │  cache, base snapshots)   │
                │                       └─────────────┬─────────────┘
                │                       ┌─────────────▼─────────────┐
                └──────────────────────▶│ JsonBlueprintSync library │  Capture, Apply, ToJson, FromJson,
                                        │ (no state, no UI)         │  paths, roots, hashes, missing pairs
                                        └───────────────────────────┘
        UJsonBlueprintSyncSettings (Project Settings, DefaultEditor.ini) is read by all of them.
```

The same data comes in three forms:

| Form | Type | Where |
|---|---|---|
| The asset | a `UDataAsset` (or subclass) instance | memory, `.uasset` |
| Document | `FAssetDefaults`: `Asset`, `Parent` and the `Self` map of property text (`overrides.self`) | memory |
| JSON | text written by `ToJson`, read by `FromJson` | `<JsonDir>/<Root>/...json` |

`Capture` turns an asset into a document, and `Apply` turns a document back into asset values. `ToJson` and `FromJson` convert
between a document and text. The subsystem and the tab compare documents through their **flat** form, one map of key to value
(`Flatten`). With only `self` left, the keys are property names.

Terms used below:

- **Class default**: what a value is compared with: the asset's archetype, `GetArchetype()`, which for a data asset is its class
  default object.
- **Override**: a value that differs from the class default. Only overrides are stored. A key missing from the document means "the
  class default".
- **Base**: the values both sides had at the last sync, stored when a difference is first detected (see
  [2.6](#26-sync-state-syncstate)). It is the asset as it was then if only the JSON changed, or the JSON file if only the `.uasset`
  changed. It tells which side changed a key.
- **Saved hash**: `UPackage::GetSavedHash()`, a hash of the `.uasset` file that Unreal records on save and reads from the file
  header on load. The asset registry has it for every package without loading it (`FAssetPackageData::GetPackageSavedHash`).
- **Pending**: the asset's JSON has changes that are not applied or discarded yet. The asset is listed in the JSON Sync tab and
  saving it does not write its JSON.
- **Resolved**: the user applied or overwrote the JSON in this session, but the asset has not been saved since. The plugin
  remembers which file the user reviewed, so a file that changes again before the save is not overwritten.
- **Undone**: an apply from the JSON Sync tab that was undone with Ctrl+Z. The asset is pending again, as before the apply.
  (An automatic apply has no undo step, so it never becomes undone.)
- **Synced**: a data asset (`IsDataAsset`) whose package is synced (`IsSyncedPackage`): under a synced root and not in an excluded
  folder.
- **Out of date** (shown as **asset changed**): pending, with a base, and every differing key is an `Asset` change. The `.uasset`
  changed while the JSON did not.

## 2. Files

```
JsonBlueprintSync.uplugin
Source/JsonBlueprintSync/
  JsonBlueprintSync.Build.cs
  Public/
    JsonBlueprintSync.h              library API
    JsonBlueprintSyncModule.h        module class, log category, tab id
    JsonBlueprintSyncSettings.h      project settings
  Private/
    JsonBlueprintSync.cpp            library implementation
    JsonBlueprintSyncModule.cpp      tab and status bar registration
    JsonBlueprintSyncSettings.cpp
    SyncState.h / .cpp               three-way diff, merge, local cache
    JsonBlueprintSyncSubsystem.h / .cpp   editor runtime
    SJsonSyncPanel.h / .cpp          the JSON Sync tab
    SJsonSyncStatusBar.h / .cpp      the status bar entry
    JsonBlueprintSyncCommandlet.h / .cpp  -run=JsonBlueprintSync
    Tests/JsonBlueprintSyncTests.cpp automation tests
```

### 2.1 JsonBlueprintSync.uplugin

- One module, `Type: Editor`, `LoadingPhase: Default`.
- `CanContainContent: false`: the plugin ships no assets.
- The host project enables it with `"TargetAllowList": ["Editor"]`, so even a project that lists it never builds it into a game.

### 2.2 JsonBlueprintSync.Build.cs

Dependencies and why each is needed:

| Module | Used for |
|---|---|
| Core, CoreUObject, Engine | everything |
| DeveloperSettings (public) | `UJsonBlueprintSyncSettings` derives from `UDeveloperSettings` in a public header |
| UnrealEd | `FScopedTransaction`, `FEditorFileUtils`, `UEditorLoadingAndSavingUtils`, `GEditor` |
| BlueprintGraph | only the tests use it, for the pin types of the Blueprint data asset classes they build |
| AssetRegistry | asset lookup, class ancestry for unloaded Blueprint classes, rename and remove events, missing pairs |
| Projects | `IPluginManager`, for the content roots of the project's plugins |
| Json, JsonUtilities | parsing JSON (writing is done by hand, see [2.5](#serialization-tojson)) |
| EditorSubsystem | `UEditorSubsystem` |
| Slate, SlateCore, InputCore | the tab and notifications. InputCore provides `EKeys`, which the list views reference. |
| ToolMenus | adds the status bar entry (`LevelEditor.StatusBar.ToolBar`) |
| WorkspaceMenuStructure | puts the tab under the Window menu |

### 2.3 Module: JsonBlueprintSyncModule

`JsonBlueprintSyncModule.h` declares:

- the log category `LogJsonBlueprintSync`, which every message uses (filter the Output Log by it);
- `JsonBlueprintSync::GetSyncTabName()`, the id `"JsonBlueprintSync"` of the tab;
- `FJsonBlueprintSyncModule`.

`StartupModule` registers a nomad tab spawner. The tab is titled "JSON Sync", is placed in the Level Editor group of the Window menu
(`WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory()`), and holds an `SJsonSyncPanel`. Registration is skipped in commandlets
and when Slate is not initialized (e.g. `-nullrhi` runs), so headless runs do not touch the UI. `ShutdownModule` unregisters it.

It also adds `SJsonSyncStatusBar` to the level editor's status bar, through `UToolMenus::RegisterStartupCallback` and
`ExtendMenu("LevelEditor.StatusBar.ToolBar")`, as the first entry of the `SourceControl` section, whose one widget starts with the unsaved
assets entry. (A section of its own placed before `SourceControl` could land left of the Compile button, which the level editor also
places there.) The menu owner is the module, so `ShutdownModule` removes it with
`UToolMenus::UnregisterOwner`. The widget is a flat button styled like the unsaved assets entry: the `Icons.Merge` icon and
"All JSON Synced" or "N JSON to Review", in the warning colour while anything is pending. It polls the subsystem's
`GetPendingNum()` (the size of `Pending`) on each paint, so it needs no delegate. The tooltip lists up to ten pending assets, built
only when shown. A click calls `OpenSyncTab`. Asset editors have their own status bars; the entry is only in the level editor's.

Caveat: the editor remembers open tabs in its layout. If the plugin is disabled while the tab was open, the editor shows an empty
"unrecognized tab" placeholder on next start. That is standard Unreal behaviour.

### 2.4 Settings: JsonBlueprintSyncSettings

`UJsonBlueprintSyncSettings` (`config = Editor, defaultconfig`) is shown in Project Settings > Plugins > JSON Blueprint Sync and
saved to `Config/DefaultEditor.ini` under `[/Script/JsonBlueprintSync.JsonBlueprintSyncSettings]`, so it is shared through
source control.

| Property | Default | Notes |
|---|---|---|
| `JsonDir` | `JsonBlueprintSync` | Relative to the project folder; an absolute path also works. Empty falls back to the default. |
| `Mode` | `Automatic` | `EJsonSyncMode`: `Automatic` or `Manual`. Decides only what the start-up check (and Refresh) does with JSON changes. |
| `ExcludedFolders` | empty | `TArray<FDirectoryPath>` with `LongPackageName`, so the picker offers content folders. |

Every caller reads the settings at the moment it needs them, never once at start-up. A change in Project Settings therefore takes
effect immediately: the next save, sync or rename uses the new mode, folder or exclusions.

### 2.5 Library: JsonBlueprintSync

`Public/JsonBlueprintSync.h` and `Private/JsonBlueprintSync.cpp`. It holds no state and shows no UI: the subsystem, the commandlet
and the tests all build on it.

#### Data assets

`IsDataAsset(const UObject*)` is true for an instance of a `UDataAsset` subclass that is not a class default object.

`IsDataAsset(const FAssetData&)` does the same without loading the asset. It looks the class up with `FindObject<UClass>` and,
if the class is not loaded (typically the Blueprint class of a data asset), asks the asset registry for its ancestor classes
(`GetAncestorClassNames`). Caveat: the answer is only as good as the registry. Before the registry has finished its initial scan, a
data asset of a Blueprint class may not be recognized. The subsystem only calls it after `OnFilesLoaded`.

`UPrimaryDataAsset`, and Blueprint subclasses of `UDataAsset` or `UPrimaryDataAsset`, are data assets. A Blueprint, a `UDataTable`,
a `UCurveTable` or any other asset type is not, and is never synced.

#### Synced roots and packages

- `GetSyncedRoots()`: `/Game` plus the content of every enabled plugin that was loaded from the project
  (`IPluginManager::GetEnabledPluginsWithContent`, `EPluginLoadedFrom::Project`) and whose mount point exists, sorted. Engine
  content and engine plugins are left out. A disabled plugin is not mounted, so its JSON files are not checked either.
- `IsSyncedPackage(Package)`: not a temp package, under one of the roots followed by `/` (case-insensitive, so `/GameX` is not under
  `/Game`), and not under any `ExcludedFolders` path (trimmed, with a trailing `/`, also case-insensitive).

#### Paths

- `GetDefaultJsonRoot()`: `JsonDir`, made absolute against the project folder and normalized (forward slashes, no trailing
  slash).
- `GetJsonPath(Root, "/Game/A/DA_B")` returns `<Root>/Game/A/DA_B.json`. The package path is used as-is, so the folder layout
  mirrors the Content Browser. A plugin's `/MyPlugin/DA_B` maps to `<Root>/MyPlugin/DA_B.json`.
- `GetPackageName(Root, Path)` is the inverse. It fails if the file is outside the root, is not `.json`, or does not give a valid
  long package name. The root comparison is case-insensitive, matching Windows file systems.
- `FindJsonFiles(Root, "/Game")` returns every `*.json` under `<Root>/Game`, recursively, normalized and sorted.
- `LoadAsset("/Game/A/DA_B")` loads the object `/Game/A/DA_B.DA_B`, the package's main asset.

#### Missing pairs

`FindMissingPairs(JsonRoot)` compares the two sides for every synced root:

- **JSON without asset**: a file under a root that does not map to a package, or whose synced package has no main asset in the
  registry, or whose asset is not a data asset. Files of excluded packages are left out.
- **Asset without JSON**: a main asset of a `UDataAsset` subclass (registry filter, recursive classes and paths) in a synced package
  with no file.

Both lists are sorted. It reads the registry only, so it loads nothing. The subsystem calls it after each sync (see
[ReportMissingPairs](#reportmissingpairs)).

#### Which properties are synced

`IsSyncable(Property)` decides. A property is synced only if all of these hold:

- it has `CPF_Edit` (it is visible in the Details panel);
- it is not `Transient`, `DuplicateTransient`, `EditConst` (read-only), `Deprecated` or `DisableEditOnInstance`
  (`EditDefaultsOnly`, or a Blueprint variable without Instance Editable: a data asset is edited like an instance);
- it is not instanced: not `InstancedReference`, `ContainsInstancedReference` or `PersistentInstance`. An instanced sub-object is
  an object of its own, which the format does not hold, so it stays in the `.uasset`;
- `ArrayDim == 1`: fixed-size C arrays such as `int32 Values[4]` are skipped.

The same rule is used when capturing and when applying. A file that names a skipped property is therefore an error, not a silent
no-op.

#### Capture

`Capture(Asset)` builds the document from the asset in memory: `Asset` is the package name, `Parent` the class path. It walks every
property (`TFieldIterator`, which includes inherited ones), skips the unsynced ones, and stores the value only if it differs from the
class default (`FProperty::Identical`). The value is written with `ExportTextItem_Direct`, the Details-panel copy format, with the
class default as the delta, so a struct lists only the fields that differ.

Caveats:

- Values come from memory, not from disk. Capturing an asset with unsaved edits captures the unsaved values.
- Floats are exported in Unreal's canonical text: `2.500000`, not `2.5`. Hand-written values are accepted in any form Unreal can
  parse, but the next export rewrites them in canonical form.

#### Serialization: ToJson

`ToJson` writes the JSON by hand, not through `FJsonSerializer`, so that the output is byte-stable:

- the top-level order is fixed: `schema`, `asset`, `parent`, `overrides`. `overrides` holds `self` only (always, `{}` if empty);
- keys are sorted **case-sensitively** (ordinal), so `Z` comes before `a`;
- two-space indent, `\n` line endings, a trailing newline, no timestamps;
- strings are escaped as JSON requires (`\"`, `\\`, `\n`, `\t`, other control characters as `\u00XX`). Non-ASCII characters are
  written as-is, in UTF-8.

So `ToJson(FromJson(ToJson(X))) == ToJson(X)`, and exporting the same asset twice gives the same bytes. The tests check both.

#### Parsing: FromJson

`FromJson` uses Unreal's JSON reader, then validates in this order:

1. The text is valid JSON with an object at the root.
2. `schema` is 2. Schema 1 gives "from an older version of the plugin; re-export"; anything else gives "'schema' must be 2".
3. No unknown top-level field. This is how a schema-1 field such as `reset` is caught, and the `defaults` layout from before
   `overrides` (also schema 2) gives "unknown field 'scope'".
4. `asset` is present and non-empty.
5. `parent` is optional.
6. `overrides` is optional (missing means nothing is overridden) and must be an object with no field other than `self`, which is
   optional. `components` and `widgets`, written by older versions of the plugin for Blueprints, are unknown fields. `self` is an
   object of string values. A number or a bool where a string is expected is an error ("value must be a string of Unreal property
   text"). Errors name the field by its path, e.g. `overrides.self.Damage`.

Caveats:

- JSON allows duplicate keys. Unreal's reader keeps the last one, without an error.
- Values are not checked here, only their JSON type. Property names and values are validated by `Apply`.

#### Comparing: Flatten and ValuesEqual

- `Flatten` returns the key → value map of the document.
- `ValuesEqual(A, B)` is true if the maps have the same keys and the values are equal case-sensitively. It ignores `asset` and
  `parent`.

Caveats:

- Values are compared **as text**. A file with `"Speed": "2.5"` differs from an asset that exports `2.500000`, although the values
  are the same. This is harmless: applying finds the values identical and changes nothing, and the next write puts the canonical
  text in the file.
- `TMap<FString, ...>` is case-insensitive in Unreal, so the maps are too. A hand-typed key `"speed"` is matched with the asset's
  `Speed` when comparing, but `Apply` rejects it (see below). The commandlet's `-verify` compares bytes and catches it.

#### Apply

`Apply(Asset, Doc)` makes the asset's synced values match the document **exactly**. Every key in the document is set, and every
override the asset has that the document does not list is reset to the class default. It runs in two passes.

**Pass 1: validate and parse, writing nothing.**

1. The asset must be a data asset ("not a data asset"), and `Doc.Asset` must equal its package name ("the JSON is for X, not Y").
2. `Current = Capture(Asset)` lists the asset's current overrides.
3. If `Doc.Parent` is set and differs from the asset's class, add a warning only ("class differs").
4. For each key in the document: find the property by exact, case-sensitive name. A wrong case gives "unknown property (did you mean
   'Speed'?)". Check `IsSyncable`, then parse the text onto a **copy of the class default's value** (`PrepareWrite`).
5. For each key in `Current` but not in the document: prepare a write of the class default's value (a reset).

   Parsing (`ImportValue`) is strict:
   - `ImportText_Direct` must succeed;
   - nothing but whitespace may follow the value ("unexpected text after the value");
   - nothing may be written to the error device. During the import the function raises `LogExec` to Verbose, because Unreal reports
     unknown struct fields only at that verbosity. Without this, `(X=1)` for an `FLinearColor` would be accepted silently.
6. If any error was collected, return without writing anything. One bad key blocks the whole file.

**Pass 2: write.** For each prepared write whose value differs from the asset's current value: `Modify`, `PreEditChange`, the copy,
and `PostEditChangeProperty(ValueSet)`. This is the sequence the Details panel uses, so undo and editor refresh behave the same.

Consequences:

- Because missing keys are reset, `Apply` with a *partial* document would wipe the other overrides. The subsystem never does
  that: it always builds a complete document with `TakeKeys` (see [2.6](#26-sync-state-syncstate)).
- Struct values are parsed onto the class default's value. A struct text that lists only some fields leaves the others at the
  **class default's** values, not at the asset's current ones.
- Arrays, sets and maps are replaced as a whole by the parsed text.
- Instanced sub-objects are not touched.
- `Apply` does not mark the asset modified, save, or open a transaction. Callers do that. Only the asset's own package changes:
  a data asset has no children to propagate to.

#### Import and MarkModified

- `MarkModified` is `MarkPackageDirty`.
- `Import` = `Apply`, then `MarkModified` if something changed. The commandlet and the tests use it.

#### File helpers and hashing

- `NormalizeLineEndings` turns `\r\n` and `\r` into `\n`.
- `LoadFile(Path)`: if the file is missing it returns false with an **empty** error, so callers can tell "missing" from "broken".
- `SaveFileIfChanged(Path, Doc)` writes UTF-8 without a BOM, only if the text differs from the file with line endings ignored. A
  CRLF checkout of an otherwise identical file is not rewritten. Missing folders are created.
- `HashText` is the MD5 of the UTF-8 text with line endings normalized. The cache stores it, so a CRLF checkout does not count as a
  change.

### 2.6 Sync state: SyncState

`Private/SyncState.h` and `.cpp`: pure functions (`DiffKeys`, `TakeKeys`, `ChooseBase`, `CanAutoApply`) and the local cache. The
subsystem uses them, and the tests check them directly.

#### DiffKeys(Base, File, Current)

DiffKeys lists every key whose value differs between the file and the asset, sorted case-sensitively. For each one it records the
JSON value, the asset value (either can be unset, meaning "not overridden", shown as `(default)`) and which side changed it:

| Base value equals… | Change | The tab uses by default | Meaning |
|---|---|---|---|
| the asset's value | `Json` | JSON | Only the file changed since the base. |
| the file's value | `Asset` | Asset | Only the asset changed since the base. |
| neither | `Both` (shown as **Conflict**) | nothing: the user must choose | Both changed, to different values. |
| no base at all | `Both` | nothing: the user must choose | It cannot be told. |

"Equals" includes "both unset". A key the file removed (unset in the file, set in the base and the asset) is therefore a `Json`
change, and taking it resets the key.

#### TakeKeys(Current, Source, Keys)

TakeKeys returns `Current` with the listed keys taken from `Source`. Where `Source` does not have a key, the key is removed from the
result, which resets it to the class default. This is how a partial choice becomes the complete document that `Apply` needs:
`Apply(TakeKeys(Capture(Asset), File, KeysThatUseJson))`.

#### ChooseBase(Inputs)

ChooseBase decides where the base of a newly detected difference comes from. Its inputs are facts the subsystem gathers:

| Input | Meaning |
|---|---|
| `bPending` | already pending (a base may be stored) |
| `bHasEntry` | the asset has a cache entry, i.e. it was synced on this machine |
| `bJsonChanged` | the file's hash differs from the entry's `JsonHash` |
| `bAssetChanged` | the package's saved hash differs from the entry's `AssetHash` (false if either is unknown) |
| `bAssetDirty` | the asset has unsaved edits |
| `bParentDiffers` | the file's `parent` is set and differs from the asset's class |

The rules, first match wins:

| Rule | Result | Why |
|---|---|---|
| pending | `Stored` | keep the base from the first detection |
| no entry | `None` | nothing is known about the last sync |
| JSON unchanged | `File` | the file is what was synced, so the asset changed every key that differs (**asset changed**). This holds even if the asset is dirty or its class changed. |
| class differs, asset changed, or asset dirty | `None` | both sides moved, or the file's values are relative to another class |
| otherwise | `Current` | the asset is what was synced, so the JSON changed every key that differs |

#### CanAutoApply(bHasBase, bParentDiffers, Keys)

True only if there is a base, the classes agree, no key is `Both`, and at least one key is `Json`. The last condition keeps an asset
that changed while its JSON did not (every key `Asset`) in the list: the JSON is the source of truth, so the plugin does not decide
for the user whether to keep the asset's change (Write JSON) or revert it (Apply JSON).

#### FSyncCache

The cache is local state in `<ProjectSaved>/JsonBlueprintSync/`. It is never committed: each machine has its own.

```
Saved/JsonBlueprintSync/
  cache.json                      { "version": 2, "entries": { "/Game/A/DA_B": { "json": "<md5>", "asset": "<io hash>", "pending": true } } }
  Base/Game/A/DA_B.json           base snapshot of a pending asset (same format as the synced files)
```

`FSyncEntry` has three fields:

- `JsonHash`: the hash of the JSON file the last time the file matched the **saved** asset on this machine. It is empty if that
  has never happened.
- `AssetHash`: the package's saved hash at that same moment, so that a `.uasset` that changed without its JSON is noticed without
  loading it. Empty if unknown: an entry from an older plugin version, or a package that was never saved. It is written only when
  set, and an older cache without it still loads (the version stays 2).
- `bPending`: the asset is waiting for review. This survives an editor restart, and so does the save guard that comes with it.
  While pending, both hashes keep describing the last sync.

Loading:

- A missing file gives an empty cache.
- A `version` other than 2 also gives an empty cache, without an error. The hashes of an old cache are of schema-1 files, which no
  longer load.
- Unreadable or broken JSON makes `Load` return false and leaves the cache empty. The subsystem logs a warning, carries on, and
  shows an error toast at the first sync (see [Notifications](#notifications)).

`Save` writes the entries sorted, and writes `pending` only when it is true. `Remove(Package)` also deletes the base file.

**The base** (`SaveBase`, `LoadBase`, `DeleteBase`) is chosen by `ChooseBase` when a difference is first detected: `Capture(asset)`
if only the JSON changed, or the parsed file if only the `.uasset` changed. It is stored as a schema-2 document. It stays fixed while
the asset is pending: later checks reuse it instead of capturing again. That is what lets the tab see edits made to the asset after
the detection as `Asset` or `Both`. It is deleted when the pending state ends. Deleting the `Base` folder by hand is safe: pending
assets then show every key as a conflict.

### 2.7 Editor subsystem: JsonBlueprintSyncSubsystem

`UJsonBlueprintSyncSubsystem` is a `UEditorSubsystem`. `ShouldCreateSubsystem` returns false in commandlets, so the commandlet
and the subsystem never run together.

#### State

| Member | Lives | Holds |
|---|---|---|
| `Cache` | disk (`Saved/`) | `FSyncCache`: JSON hash, saved hash and pending flag per package, plus base files |
| `Pending` | memory | package → `FPending { File (parsed doc, unset if broken), Error, Base (unset if unknown), FileHash (of the file compared) }`. Mirrors the cache's pending entries once a sync has run. |
| `Resolved` | memory, this session only | package → `FResolved { FileHash, ReviewedFile, Before, TransactionId }` for assets applied or overwritten but not saved since. `FileHash` and `ReviewedFile` are the file the user reviewed; `Before` is the pending state before the apply (unset for Write JSON); `TransactionId` is the apply transaction, if it changed the asset. |
| `Undone` | memory, this session only | the same records for applies that were undone, so that a redo can resolve them again |
| `UndoRedoTransactionId` | memory | the transaction being undone or redone, set by `MatchesContext` just before `PostUndo` / `PostRedo` |
| `PendingRemovals` | memory | packages reported as removed, checked on the next tick |
| `PendingNotification` | memory | the "N asset(s) differ" toast, so it can be replaced |
| `bCacheLoadFailed` | memory | the cache could not be read; the first sync shows a toast and clears it |
| `bCacheSaveFailed` | memory | the last cache save failed and its toast was shown; cleared by the next save that works |

#### Initialize and Deinitialize

`Initialize`:

1. Loads the cache. If it cannot be read, it starts with an empty one and sets `bCacheLoadFailed`. It is too early for a toast.
2. Subscribes to `UPackage::PackageSavedWithContextEvent` and to the asset registry's `OnAssetRenamed` and `OnAssetRemoved`.
3. If the registry is still scanning, waits for `OnFilesLoaded`. Otherwise it waits one tick. Either way it then calls
   `SyncFromJson`.
4. Registers for undo and redo (`GEditor->RegisterForUndo`; the subsystem is an `FEditorUndoClient`).
5. Registers the console command `JsonBlueprintSync.Sync` (→ `SyncFromJson`).
6. Logs the synced roots and the JSON root.

`OnFilesLoaded` shows the error toast for an unreadable cache, if any, then calls `SyncFromJson`.

`Deinitialize` undoes all of this and closes the toast.

#### SyncFromJson: the start-up check

This runs at start-up, on `JsonBlueprintSync.Sync` and on the tab's Refresh button.

1. Refuses during Play In Editor, with a warning toast.
2. For every JSON file under `<JsonRoot>/<root>` for each synced root:
   1. Map the file to a package. A file that does not map, or whose package is not synced (an excluded folder), is skipped here;
      the first is reported as a missing pair. An unreadable file is an error ("cannot read the file").
   2. Get the package's saved hash (`GetSavedHash`): from the package if it is loaded, else from the asset registry. The asset is
      **changed** if the entry has an `AssetHash`, the saved hash is known, and they differ.
   3. **Skip** a resolved asset whose file is still the reviewed one (same hash as `FResolved::FileHash`): its next save writes
      the JSON.
   4. **Skip** the file, without loading anything, if the asset is not resolved, the cache has a non-pending entry with the same
      JSON hash and the asset has not changed. This is what keeps start-up cheap. An entry without an `AssetHash` (from an older
      plugin version) adopts the current saved hash here, so detection works from then on.
   5. Find the package's main asset in the registry. If there is none, or it is not a data asset, drop any cache, pending,
      resolved or undone state it had and move on. The file is reported as a missing pair.
   6. A resolved asset whose file changed after the review becomes pending with the reviewed file as the base
      (`MarkChangedAfterReview`, see [OnPackageSaved](#onpackagesaved-write-on-save-and-the-save-guard)). Any other asset goes to
      `Detect` with the asset-changed flag.
   7. In Automatic mode, if the asset became or stayed pending, remember it for auto-apply.
3. Pending assets whose JSON file no longer exists are dropped: there is nothing left to review.
4. Save the cache, log a summary, and show one error toast if any file could not be checked: unreadable, or (from `Detect`) the
   asset cannot be loaded. Each is logged as an error. Nothing is recorded for them, so the next sync tries again.
5. `ReportMissingPairs` (below).
6. **Automatic apply.** For each remembered asset, get the view (see below). The asset is applied only if the file parses and
   `CanAutoApply` agrees:
   - there is a base;
   - the classes agree;
   - no key is `Both`;
   - at least one key is `Json` (an asset that changed while its JSON did not stays listed);
   - the package is not dirty, because the apply saves it (step 3 below would save the user's unsaved edits too). A dirty asset
     usually has no base anyway, but one that was already pending keeps its stored base;
   - `IsInUndoHistory` is false: no transaction in `GEditor->Trans` (the whole queue, so undo and redo steps) holds an object of
     the asset's package. Otherwise the asset stays listed, with a log line.

   The keys applied are the `Json` keys; `Asset` keys keep the asset's value. All such assets are applied in one batch
   (`ApplyBatch` with `bAutomatic`), with **no transaction**, and **saved**: the JSON is the source of truth, so Ctrl+Z must not
   bring back the old values and let the next save write them over the JSON. The undo-history check is what makes that safe.
   Changing an object outside a transaction while an older transaction holds it would break undo: undoing or redoing that
   transaction restores the object's state from before the apply, so the applied values would be lost and the next save would
   write the old ones. At editor start the history is empty, so the check matters only for a Refresh or `JsonBlueprintSync.Sync`
   later in the session. An asset whose tab apply was undone is caught by it too (the apply is a redo step, or sits under the edits
   made since), so there is no separate check for `Undone`.

   The save keeps the `.uasset` on disk (what the game uses) equal to the JSON, and closes a hole: left unsaved, a Reload or a
   source control revert of the asset would bring back its old values while it is still in `Resolved`, and its next save would
   write them over the JSON.
7. Show the pending toast (Review / Later) if anything is pending, and broadcast `OnPendingChanged`, which refreshes the tab.

In Manual mode step 6 does nothing: every difference stays listed.

#### ReportMissingPairs

ReportMissingPairs calls `FindMissingPairs(GetJsonRoot())`. For each JSON file without its data asset it logs a warning with the
fix (delete the file if the asset was deleted on purpose, or restore the asset); for each data asset without its JSON, a warning
with its fix (save the asset or run `-export`, or add its folder to Excluded Folders). If there is any, it shows one warning toast
with the two counts. It changes nothing. It runs after every sync, so the toast comes back on each Refresh until the pairs are
fixed.

#### Detect: one changed file

`Detect(Package, Text, Hash, bAssetChanged, Errors)` loads the asset, captures it as `Current` and parses the file. It runs when
the JSON or the `.uasset` changed (or the asset is pending). If the asset cannot be loaded, it adds an error to `Errors` and
returns.

**If the file parses, its `parent` agrees with the asset's class, and `ValuesEqual(File, Current)`**, the two sides agree, e.g. a
teammate committed both. The asset is removed from `Pending`, and then:

| Asset dirty? | Was pending? | Cache result |
|---|---|---|
| no | – | base deleted, entry = `{file hash, current saved hash, not pending}` |
| yes | yes | base deleted, entry = `{old hashes, not pending}` |
| yes | no | unchanged |

Only a match with the **saved** asset is recorded. Recording a match with unsaved values would hide the difference if the editor
were closed without saving. Then the asset on disk would differ from the file, but the hash would say "in sync".

**Otherwise**, they differ (or the file is broken, or the class differs). `ChooseBase` picks the base (see
[2.6](#choosebaseinputs)):

| Result | Base |
|---|---|
| `Stored` | the stored base file (none if it is missing) |
| `File` | the parsed file, saved as the base file. Every differing key shows as `Asset`: **asset changed**. |
| `Current` | `Current`, saved as the base file. Every differing key shows as `Json`. |
| `None` | none: every key is a conflict |

A class mismatch is logged as a warning. The entry becomes `{old hashes or "", pending}`. The asset is added to `Pending` (with the
file's hash) and removed from `Resolved`. The differing keys are logged. A broken file is logged as a warning and is pending with
its error.

Note that Detect loads every asset whose JSON or `.uasset` changed. After a large pull this can take a while at start-up.

#### GetPendingView

GetPendingView builds what the tab shows for one pending asset: error, `bHasBase`, `DiffKeys(Base, File, Capture(asset))`, and
`JsonParent` / `AssetParent` when the classes differ. It captures the asset **now**, so the view reflects the asset's current
in-memory values. `IsJsonOutOfDate()` is true when there is a base and every key is `Asset`.

#### ApplyJson, ApplyBatch and Resolve

`ApplyJson(Package, Keys, bSave)` is the tab's **Apply** (`bSave` false), **Apply and Save** (`bSave` true) and **Apply JSON**
(`bSave` false, every listed key): `ApplyBatch` with one request, then `OnPendingChanged`. With `bSave`:

- `ApplyBatch` shows no success toast; `ApplyJson` shows its own.
- If the asset is dirty after the apply, it is saved with `FEditorFileUtils::PromptForCheckoutAndSave(bCheckDirty, no prompt)`, which
  checks the file out first if source control needs it. The save goes through `OnPackageSaved` like a Ctrl+S: the asset is
  resolved and the file is the reviewed one, so the JSON is written and `Resolved` is cleared. Toast: "Applied the JSON to … and
  saved it."
- If the save fails or is declined, the asset stays dirty and resolved, as after a plain Apply. Toast (warning): "Applied the JSON
  to …, but it was not saved."
- If the asset is not dirty (every key used the asset, and it had no unsaved edits), `Resolve` has already written the JSON, so
  nothing is saved. Toast: "Wrote the JSON of …; the asset did not change."

`ApplyBatch(Requests, bNotify, bAutomatic)`:

1. For each request, load the asset and build `TakeKeys(Capture(asset), File, Keys)`. A package that is not pending, has a broken
   file, or cannot be loaded fails.
2. Inside one `FScopedTransaction("Apply JSON")` (none if `bAutomatic`), call `Apply` for each. Warnings and errors go to the log. A
   failed asset is skipped, stays pending, and the others go on. Changed assets are `MarkModified`. **Nothing is saved here.** If
   nothing changed, the transaction is cancelled, so no empty undo step is left. The transaction's id
   (`GUndo->GetContext().TransactionId`) is kept for the assets it changed. Without a transaction, `Modify` only marks the package
   dirty and the id stays invalid.
3. `Resolve` each asset that succeeded, with the transaction id if the apply changed it, then save the cache.
4. If `bAutomatic`, save the changed assets with `FEditorFileUtils::PromptForCheckoutAndSave` (checkout first if source control
   needs it). The save goes through `OnPackageSaved` like any resolved asset, so it writes the JSON. An asset still dirty afterwards
   (checkout declined, save failed) stays resolved, as after a manual Apply.
5. Toasts: "Applied JSON to … Not saved; Ctrl+Z reverts." (automatic: "Applied JSON to … automatically and saved it.", or the
   warning "…automatically, but it was not saved…") and/or "Cannot apply the JSON of …".

`Resolve(Package, Asset, TransactionId)`:

1. Remove the package from `Pending` and `Undone`, delete its base, and clear the pending flag. The hash is kept, because it still
   describes the last file that matched the saved asset.
2. If the asset is now dirty, add it to `Resolved` with the reviewed file (the pending state's `File` and `FileHash`), the pending
   state itself as `Before`, and the transaction id. Its next save writes the JSON, including the keys where the user kept the
   asset's value, as long as the file is still the reviewed one.
3. If it is not dirty (nothing changed, e.g. every key used the asset), write the JSON **now**. Otherwise no save would ever
   write it. In effect, applying with every key on Asset is the same as Write JSON.

#### Undo and redo

The subsystem is an `FEditorUndoClient`. Unreal calls `MatchesContext` with the transaction being undone or redone, then
`PostUndo` or `PostRedo`. `MatchesContext` stores the transaction id and accepts every transaction.

`PostUndo`: every resolved asset whose `TransactionId` is the undone transaction goes back to its `Before` state: `MarkPending`
restores it to `Pending`, sets the cache's pending flag and stores its base again. The record moves to `Undone`. A warning toast
says the asset is back in the tab, and the tab refreshes. Without this, the asset would stay dirty (Unreal's undo does not clear
dirty flags) and its next save would write its old values over the JSON change.

Undoing an unrelated transaction does nothing, even if it touches a resolved asset: an edit made after the apply is the user's.

`PostRedo`: every `Undone` record with the redone transaction is resolved again, if the asset is still pending with the same file
hash. Records for that transaction are dropped either way. This is only logged.

An automatic apply resolves with an invalid transaction id, so no undo matches it. An asset in `Undone` is still in the undo
history, so a Refresh does not apply it automatically. `Undone` is not saved, but the next editor start has an empty history and,
in Automatic mode, applies the file as usual.

#### OverwriteJson

OverwriteJson is the tab's **Write JSON**. It loads the asset and clears the pending state the same way as `Resolve`. It then
writes `Capture(asset)` to the file and, if the asset is dirty, adds it to `Resolved` with the file now on disk as the reviewed file
(the one just written, or the old one if the write failed). It has no transaction, so there is nothing to undo. It also works when
the file is broken. The tab offers it for an unreadable file, as the way out besides fixing it, and for an asset that changed while
its JSON did not, to keep the asset's values. For other readable files, **All from Asset** + **Apply** does the same through
`Resolve`. The save guard also calls it for a JSON that is only out of date.

Caveat: if the asset has unsaved edits, the file gets the **unsaved** values. If you then close the editor without saving, the file
has values the `.uasset` does not. The next start detects this: the hash still differs, so the file is compared again and listed as
JSON changes (and, in Automatic mode, applied).

#### WriteJson

WriteJson calls `SaveFileIfChanged(Path, Capture(asset))`. A write failure shows "Cannot write … Is it read-only?". Then, **only if
the asset is not dirty**, it records `{hash of what was written, package saved hash, not pending}`, removes the asset from
`Resolved` and `Undone` and saves the cache. The save event fires after Unreal clears the dirty flag and sets the new saved hash, so
a normal save records the sync.

#### OnPackageSaved: write on save and the save guard

The save event fires for every saved package. The handler returns at the first condition that applies:

1. Autosaves, procedural saves and cooking are ignored.
2. `Package->FindAssetInPackage()` is not synced (not a data asset, or not in a synced package): ignored.
3. **Pending** (in `Pending` or flagged in the cache):
   - if the file's hash still equals the entry's `JsonHash` (`IsJsonUnchanged`), the JSON has no changes of its own and is only
     out of date: `OverwriteJson`, which clears the pending state and writes the file. Saving keeps the asset's values, the same as
     Write JSON;
   - otherwise skip writing, and show "Saved X, but did not write its JSON: the JSON has changes you have not reviewed" with a
     Review button.
4. **Resolved**: the file may differ from the last sync, but it must still be the reviewed file.
   - If the file exists and its hash differs from `FResolved::FileHash`, it changed after the review (a pull with the editor open
     and no Refresh, a hand edit). `MarkChangedAfterReview` makes the asset pending with the **reviewed file as the base**: keys
     the new file changed are `Json`, the values the user kept or edited since are `Asset`, and a key both changed is `Both`. The
     base is stored, so this survives a restart. The toast reads "Saved X, but did not write its JSON: the JSON changed again
     after you reviewed it", with a Review button.
   - Otherwise (same file, or no file) `WriteJson`.
5. **Save guard**: if the file exists and its hash differs from the cache entry (or there is no entry), the file changed since the
   last sync without the plugin seeing it (a pull with the editor open and no Refresh, a hand edit, another tool). Writing now would
   silently discard that change. So:
   - if the file parses and its values equal the asset's, write: nothing would be lost, and this only normalizes the file;
   - otherwise, mark it **pending without a base** (`MarkPending`), show the skipped-save toast and refresh the tab. There is no
     base because the asset has been edited since the file changed, so either side may have changed any key.
6. Otherwise, `WriteJson`.

Resolved assets get step 4 instead of the save guard: after an apply the file still differs from the cached hash, and the save
that follows is meant to write it.

#### OnAssetRenamed

This also covers moves.

1. Only for data assets whose old package is synced and actually changed.
2. Drop the old package's pending, resolved and undone state. A pending rename is lost; the renamed asset is not pending.
3. If there is no old file, just remove the old cache entry. If it exists but cannot be read or parsed (e.g. a merge conflict),
   it is left at the old path, the old cache entry is removed, and a warning toast says so. Saving the renamed asset writes a new
   file; the old one is reported as a JSON without its asset at every sync until it is fixed or deleted.
4. Otherwise, if the new package is synced, write the file at the new path with `asset` updated, **unless a file already exists
   there**. In that case the renamed package was already saved and its JSON is newer. Then delete the old file and remove the old
   cache entry. No entry is created for the new package: its next save creates it (see [weak spots](#6-weak-spots)). A move into an
   excluded folder only deletes the old file.

#### OnAssetRemoved and ProcessRemovals

The registry reports removals mid-operation, also for in-memory packages and during renames. The subsystem therefore queues the
package and checks it on the next tick. Only if the package file is gone (`!DoesPackageExist`) does it delete the JSON file and
remove the cache entry, the pending state and the resolved state.

Both use `DeleteJsonFile`, which deletes read-only files too and shows an error toast if the file cannot be deleted (e.g. another
program holds it open).

#### Notifications

- `Notify(Text, Level)` logs and shows a toast. `ENotice::Success`: check mark, 6 s, logged as Display. `Warning`: warning icon,
  8 s, logged as Warning. `Error`: red cross, 10 s, logged as Error.
- Toasts for problems:

  | Toast | Level | When |
  |---|---|---|
  | "N JSON files could not be checked" | Error | a sync found unreadable files or assets that cannot be loaded |
  | "N JSON files have no data asset, N data assets have no JSON file" | Warning | a sync found missing pairs |
  | "cannot read cache.json" | Error | at the first sync, if `Initialize` could not read the cache |
  | "cannot write cache.json" | Error | the first failed cache save; later failures are only logged until a save works |
  | "cannot write / delete <file>" | Error | a JSON file cannot be written (save, Write JSON, rename) or deleted (rename, delete) |
  | "renamed A to B, but its JSON cannot be read" | Warning | rename of an asset whose JSON does not parse |
  | "not checked during Play In Editor" | Warning | the console command or Refresh during PIE |
  | "Cannot apply the JSON of …" | Error | an apply failed |
  | "Applied the JSON to …, but it was not saved" | Warning | Apply and Save could not save the asset (declined checkout, failed save) |
  | "Applied JSON to … automatically, but it was not saved" | Warning | the save after an automatic apply did not happen |

- `ShowPendingNotification` replaces the previous "N asset(s) differ" toast with a sticky one: up to eight names, then "and N more",
  with Review (opens the tab) and Later (closes the toast) buttons. It is shown after every `SyncFromJson` that leaves something
  pending, including the Refresh button.
- `NotifySaveSkipped` shows a 10 s toast with a Review button, and logs a warning.

### 2.8 The JSON Sync tab: SJsonSyncPanel

`SJsonSyncPanel` is a compound widget laid out like a merge tool:

- a summary line and a **Refresh** button (= `SyncFromJson`);
- on the left (30%), the **asset list**: Asset, Status. Status, first match wins: "Cannot read the JSON" (orange),
  "N keys, class differs" (orange), "N keys, asset changed" (orange), "N keys, conflicts" (orange), "N keys". Double-clicking an
  asset opens its editor;
- on the right (70%):
  - a detail line: the package, and, first match wins, the parse error, the two classes, the "asset changed, its JSON did not"
    note, the "unknown which side changed, every key is a conflict" note, or how to choose;
  - the **key list** (multi-select): Key, JSON value, Asset value, Changed in (JSON / Asset / Conflict, with tooltips; Conflict in
    orange), Use (JSON in `AccentBlue`, Asset in `AccentGreen`, **Choose** in orange). `(default)` in grey means the key is not set
    on that side. The value the key uses has a highlighted background (the theme's primary colour); the other one is dimmed.
    `SKeyRow::GetBorder` shows the selection only while two or more rows are selected; a single selected row gets the normal row
    background;
  - below it: **All from JSON**, **All from Asset**, **Reset**, the counter "N conflicts to choose" (orange), **Show in Content
    Browser**, **Write JSON** (primary), **Apply JSON**, **Apply** and **Apply and Save** (primary). Which of them show depends on
    the selected asset:

    | Selected asset | Buttons |
    |---|---|
    | unreadable JSON | Write JSON (it discards the file) |
    | asset changed (`IsOutOfDateSelected`: readable, `bSelectedOutOfDate`) | Write JSON (keeps the asset) and Apply JSON (every listed key from the JSON, as one undo step, not saved). The per-key buttons, Apply and Apply and Save are hidden (`GetKeyChoiceVisibility`). |
    | anything else | the per-key buttons, Apply and Apply and Save |

Each key has an `EKeySource`: `Json`, `Asset` or `None` (not chosen). `GetDefaultSource` gives `Json` for a `Json` change, `Asset`
for an `Asset` change and `None` for `Both`. The user changes it by:

- a left click on the JSON or the asset value cell (`SKeyRow::MakeValueCell`). The click is left unhandled, so the row's
  selection still updates;
- the context menu (`OnKeyContextMenuOpening`) on the selected keys: Use JSON, Use Asset, Reset;
- the buttons, for every key. Reset (`None`) puts a key back to its default.

Behaviour:

- It subscribes to `OnPendingChanged` and rebuilds both lists on each broadcast (`Refresh`).
- The selection is kept by package name. If the selected asset is gone, the first one is selected.
- Choices are stored in `Choices`, by package and key, with the JSON and asset values they were made for. They survive a refresh
  and switching to another asset and back. A stored choice whose values no longer match the key's (the file or the asset changed
  it since) is dropped, and the key goes back to its default. Choices for assets no longer listed are dropped.
- Apply and Apply and Save are enabled when no key is `None` and the file can be parsed. They pass the keys that use `Json` to
  `ApplyJson`. The bulk buttons need at least one key and a readable file.
- `bRefreshing` keeps the selection callback from reacting while the list is being rebuilt.

Caveats:

- The tab does not listen to asset edits. After you change the asset, the Asset value and Changed in columns are stale until the
  next refresh (Refresh button, any apply, or selecting another asset and back). Apply still uses the asset's current values for
  the keys that use the asset, because `ApplyBatch` captures the asset again. A choice made on a stale row is dropped at the next
  refresh if the asset value changed.
- Using JSON for a key whose values have meanwhile become equal does no harm: `Apply` finds the value identical and skips it.

### 2.9 Commandlet: JsonBlueprintSyncCommandlet

`UnrealEditor-Cmd.exe <Project>.uproject -run=JsonBlueprintSync -export|-import|-verify [-dir=/Game/...] [-jsondir=<path>] [-nosave]`

`Main` requires exactly one action and a valid content path for `-dir` (default `/Game`; a trailing slash is removed). `-jsondir`
overrides the setting, relative to the project folder.

`FindSyncedPackages(Dir)` runs a synchronous registry scan of `Dir`. It then filters by class `UDataAsset` (recursive, which
includes Blueprint data asset classes), keeps only main assets, and drops packages that `IsSyncedPackage` refuses (excluded folders,
engine content). `-dir=/MyPlugin` works for a project plugin.

| Action | What it does | Exit code 1 when |
|---|---|---|
| `-export` | `SaveFileIfChanged(Capture(asset))` for each synced data asset. Warns about JSON files with no synced asset. | an asset cannot be loaded or a file cannot be written |
| `-verify` | Compares each file **byte for byte** (line endings normalized) with `ToJson(Capture(asset))`. | a file is missing, broken, or differs; or a JSON file has no synced asset |
| `-import` | Loads every JSON file under `Dir` and runs `Import` (Apply + mark modified) for the synced ones; a file whose package is not synced is skipped with a log line. The whole file is applied, **whatever the mode**. It then saves the packages it changed, unless `-nosave` is given. | a file is broken or has no asset, an apply fails, or saving fails |

Caveats:

- `-import` ignores the mode and the review: it is meant for deliberate bulk use (CI, a scripted migration), not for teammates'
  everyday sync.
- `-verify` is strict about formatting. A hand-edited file with the same values but different spacing or key order fails, as does a
  float written as `2.5`. Re-export to fix it.
- The commandlet does not read or write the editor's cache. The next editor start handles its changes: the file hashes no longer
  match the cache, so the files are compared again, match their assets, and are recorded.
- In Git Bash, set `MSYS_NO_PATHCONV=1`, or `/Game/...` arguments are turned into Windows paths.

### 2.10 Automation tests

`Private/Tests/JsonBlueprintSyncTests.cpp` holds 12 tests under `JsonBlueprintSync.*`. They are compiled only with
`WITH_DEV_AUTOMATION_TESTS`. `FTestData` builds a Blueprint data asset class (a subclass of `UPrimaryDataAsset` with a double, a
linear colour, an int array and a class-only variable) and a data asset of it, in `/Temp` packages, so they never touch project
content or the JSON folder. `MissingPairs` uses a JSON folder of its own under `Saved/` and deletes it afterwards.

| Test | Covers |
|---|---|
| `Format` | output format, key order, escaping, round trip, rejected documents (bad JSON, schema 1 and 3, the old `defaults` layout, missing asset, `reset` field, unknown fields at the top level and in `overrides` including `components`, non-object `overrides`, number value), a file without `overrides`, path mapping, hash ignoring line endings |
| `RoundTrip` | capture → import → capture is stable; re-importing the export changes nothing |
| `RejectsBadValues` | unknown property, wrong case, bad array, unknown struct field, trailing text, class-only variable, wrong asset; nothing applied in each case |
| `MissingKeyResets` | keys missing from the document go back to the class default |
| `Merge` | `DiffKeys` classification (Json, Asset, Both, no base), `TakeKeys` + `Apply` keeps the local change |
| `TakeKeys` | take and remove keys; `ValuesEqual` ignores asset and parent |
| `ChangedAfterReview` | with the reviewed file as the base, a later file change is `Json`, a kept asset value is `Asset`, both is `Both`; `CanAutoApply` on each |
| `Cache` | cache and base round trip (including the asset hash), an old cache version is dropped, a broken cache reports failure |
| `BaseChoice` | `ChooseBase` for every rule, `CanAutoApply` (base, class, conflict, asset changed, no keys), an asset-changed JSON diffs as `Asset` keys |
| `Settings` | defaults (Automatic, no exclusions), synced roots, `IsSyncedPackage` (engine, temp, whole-folder match, excluded folders) |
| `DataAsset` | `IsDataAsset` for a data asset, its class Blueprint, a class default, null and a package; `Apply` refuses a non-data asset |
| `MissingPairs` | a JSON without its asset is reported, one in an excluded folder is not, assets are synced and sorted, an asset with its JSON is in neither list |

What the tests do **not** cover: the subsystem itself (save guard, Detect, start-up and saved hashes, automatic apply, undo and
redo, rename, delete, the missing-pair toast), the tab and the commandlet. Its decision rules are covered through `ChooseBase` and
`CanAutoApply`. [ManualTests.md](ManualTests.md) covers those by hand.

## 3. End-to-end flows

### Saving an asset (normal case)

1. You press Ctrl+S on `DA_Lamp`.
2. Unreal saves the package and clears the dirty flag.
3. `OnPackageSaved`: it is synced, not pending, and the file's hash matches the cache.
4. `WriteJson`: capture, write the file if different, record the hash.

### A teammate's change arrives (editor closed during the pull)

1. `git pull` brings `DA_Lamp.uasset` and `DA_Lamp.json` together.
2. Editor start → `SyncFromJson` → the hash differs from the cache → `Detect` → the asset (the new `.uasset`) matches the file →
   the new hash is recorded. Nothing is listed.

If the teammate committed only the JSON, the JSON hash changed and the saved hash did not. `Detect` sees a difference, takes the
asset as the base, and the asset becomes pending with every key as `Json`. In Automatic mode it is applied and saved; in Manual
mode it is listed for review.

If the teammate committed only the `.uasset`, the JSON hash is unchanged but the saved hash differs. `Detect` takes the file as the
base, so every differing key is `Asset`, and the asset is listed as **asset changed**, in both modes. Write JSON (or saving the
asset) keeps the asset's values; Apply JSON reverts the asset to the JSON. If its values did not change, the new saved hash is
recorded and nothing is listed.

If the teammate committed both but they disagree, both hashes changed: pending without a base, every key a conflict.

### The class of a data asset changes

1. A data asset's class is changed (e.g. the asset is replaced by one of another class). Saving it writes the JSON with the new
   `parent` and the overrides against the new class's defaults. Nothing else is needed.
2. A teammate pulls both files: they match. If they pull only the `.uasset`, the JSON names the old class: listed as "class
   differs", with the keys as `Asset` changes. All from Asset and Apply (or saving the asset) fixes it.
3. If the JSON also changed, there is no base: every key is a conflict and nothing is applied automatically.

### The pull happens with the editor open

The plugin does not watch files. Nothing happens until one of these:

- **Refresh** (or `JsonBlueprintSync.Sync`): same as start-up.
- **Saving the asset**: the save guard sees a hash it does not know and does not overwrite the file. If values differ, the asset
  becomes pending without a base.

Caveat: if the pull also changed a `.uasset` that is loaded, the asset in memory still has the old values until Unreal reloads the
package. A Refresh before the reload compares the file with the old in-memory values. Pulling with the editor closed avoids this.

### Reviewing in the tab

1. Select the asset. The keys are listed with their default sources; conflicts show **Choose**.
2. The user chooses a source for each conflict (click, context menu or bulk button).
3. Apply → `TakeKeys(Capture, File, keys that use JSON)` → `Apply` in one transaction → `MarkModified` → `Resolve`.
4. The asset is modified and in `Resolved`. Ctrl+S → `OnPackageSaved` → not pending, resolved, and the file is still the reviewed
   one → `WriteJson` writes the merged values (keys that use JSON from the file, the rest from the asset) and records the hash.
   Apply and Save does step 4 at once.
5. Had the user pressed Ctrl+Z before saving, `PostUndo` would have put the asset back in `Pending` and the save would have skipped
   the JSON. After Apply and Save, the record is already gone: Ctrl+Z is an ordinary edit, and the next save writes the asset's
   values to the JSON.

### Renaming

1. `DA_A` → `DA_B`: the JSON is rewritten at the new path with `asset` updated, and the old file and cache entry are removed.
2. The next save of `DA_B` finds no cache entry. The guard compares values; they are equal, so the JSON is written and recorded.

## 4. Life of one asset's sync state

```
                 save (file unchanged or values equal)
        ┌──────────────────────────────────────────────┐
        ▼                                              │
  ┌───────────┐  only the file, or only the    ┌────────┴────────┐
  │  SYNCED   │  .uasset, changed              │ PENDING, base   │
  │           │───────────────────────────────▶│                 │
  │ H, A      │                                │ (Json / Asset / │
  └─────┬─────┘                                │  Conflict)      │
        │ both changed, asset dirty, other     └────────┬────────┘
        │ class, never synced, or save guard            │ Apply / Apply and Save / Write JSON / Apply JSON
        ▼                                               ▼
  ┌──────────────────┐  Apply / Write JSON       ┌──────────────┐
  │ PENDING, no base │──────────────────────────▶│ RESOLVED     │  (asset dirty)
  │ (all Conflict)   │                           │ next save    │
  └──────────────────┘                           │ writes JSON  │
        ▲  both sides become equal: back to SYNCED └──────┬───────┘
        │  (if the asset is saved)                        │ save
        │                                                 ▼
        └──── Automatic, JSON keys, no conflict: auto-applied and saved ──▶ SYNCED
```

| State | Cache entry | Base file | In `Pending` | In `Resolved` | Saving the asset |
|---|---|---|---|---|---|
| Not tracked | none | no | no | no | guard if a file exists, else writes |
| Synced | `{H, A}` | no | no | no | writes (guard if the file's hash ≠ H) |
| Pending with base | `{H, A, pending}` | yes | yes | no | skipped with a toast, unless the file's hash is still H (then written) |
| Pending without base | `{H or "", A or "", pending}` | no | yes | no | skipped with a toast, unless the file's hash is still H |
| Resolved | `{H, A}` | no | no | yes | writes if the file is still the reviewed one; otherwise pending with the reviewed file as base, and a toast |
| Undone | `{H, A, pending}` | as before the apply | yes | no (in `Undone`) | skipped with a toast, as for pending |

`H` is the JSON hash and `A` the package's saved hash at the last sync. A synced entry is left alone at start-up only while the file
hashes to `H` and the package's saved hash is `A`.

`Pending` is in memory. After a restart it is rebuilt from the cache's pending entries by the start-up check, because a pending entry
is never skipped by the hash check.

## 5. Corner cases

**Detection**

- *Fresh clone, or `Saved/JsonBlueprintSync` deleted*: there is no cache, so every JSON file is compared at the next start, which
  loads every synced asset once. Files that match are recorded. Files that differ are pending **without a base**, all keys
  Conflict, and nothing is applied automatically.
- *JSON file deleted, asset kept*: reported as a data asset without its JSON at every sync, until the asset is saved (which writes
  the file again) or `-export` runs. `-verify` reports it missing.
- *Asset deleted (by a pull), JSON kept*: reported as a JSON without its data asset at every sync, until the file is removed.
- *JSON of a Blueprint, from an older version of the plugin*: its asset is not a data asset, so the file is reported as a JSON
  without its data asset, and any cache entry for it is dropped. Delete the file.
- *JSON in an excluded folder*: ignored, and not reported.
- *Git merge conflict markers in a JSON file*: the file does not parse. The asset is listed as "Cannot read the JSON". Fix the file
  and Refresh, or use Write JSON.
- *Only whitespace or formatting changed in the JSON*: the hash changes, `ValuesEqual` is true, so at start-up the new hash is just
  recorded, and on save the file is rewritten in canonical form.
- *Wrong `asset` field* (e.g. a file copied by hand): the editor never checks it, because `ValuesEqual` ignores it and `TakeKeys`
  keeps the asset's own name. `-import` rejects it and `-verify` reports it. The next write from the editor fixes it.
- *`parent` mismatch* (the asset's class changed): the editor lists the asset as "class differs" and applies nothing
  automatically, even when the values are equal. If only the `.uasset` changed, the keys are `Asset` changes and saving the asset
  fixes the JSON; if the JSON changed too, there is no base. `-import` only warns. An empty `parent` in a hand-written file makes no
  claim.
- *`.uasset` changed, JSON not* (committed alone, or saved while the plugin was off): detected at start-up through the saved hash.
  Listed as **asset changed** if the values differ, in both modes, and never applied automatically. Recorded silently if the values
  are equal.
- *Change to the class defaults* (in C++ or the data asset's Blueprint class): the `.uasset` does not change, so nothing is
  detected. The document holds only the asset's overrides, so it stays correct, except when:
  - the class default now equals an override: the key is still listed but is no longer an override (applying it changes nothing);
  - the class removed or renamed an overridden property: the key is unknown, and applying the file fails.

  These last until the asset is saved. `-verify` catches them.
- *Cache from an older plugin version* (no `asset` field): the first start adopts the current saved hash for entries whose JSON
  matches. A `.uasset` that changed before that first start is not noticed.
- *Loaded, dirty package at Refresh*: the saved hash is taken from the package in memory, not from the file. A `.uasset` replaced on
  disk while it is loaded is therefore not noticed until Unreal reloads it or the editor restarts.

**Applying**

- *One bad value*: nothing in that asset is applied, it stays pending, and the Output Log lists the problems. Other assets in the
  same batch are still applied.
- *Unknown key in the JSON* (e.g. a renamed property): using JSON for it, the apply fails. Using the asset, the apply succeeds, and
  the next save removes the key from the JSON.
- *Every key uses the asset*: the asset keeps every value, and the JSON is overwritten at once (asset not dirty) or on the next
  save (Apply and Save: at once).
- *Apply and Save, then Ctrl+Z*: the save already wrote the merged JSON and cleared `Resolved`, so the undo is an ordinary edit.
  The asset is modified with its old values, and saving it writes them to the JSON. Nothing is lost silently: both files are on
  disk and the change shows in git.
- *Apply and Save, checkout declined*: nothing is saved. The asset stays modified and resolved, as after Apply.
- *Ctrl+Z after Apply or Apply JSON*: the values go back and the asset is pending again, with the base it had before, and a warning
  toast. Saving it does not write the JSON, unless the JSON is still only out of date (then saving keeps the asset's values, as
  Write JSON does). Ctrl+Y resolves it again.
- *Ctrl+Z after an automatic apply*: nothing to undo; the apply is not in the history. Ctrl+Z undoes whatever came before it,
  which is nothing at start-up. To keep the asset's old values, edit them back and save, or revert the JSON in git.
- *Asset edited this session, then a teammate's JSON arrives and you Refresh*: not applied automatically, because the edit is in
  the undo history. It stays listed and you apply it in the tab, which is undoable. The next editor start would apply it
  automatically.
- *Ctrl+Z of a tab Apply, then Refresh*: the apply is a redo step, so the asset stays listed.
- *Ctrl+Z after Apply, then edits, then Ctrl+Y*: Unreal drops the redo history when you edit, so there is nothing to redo. The
  asset stays pending.
- *Ctrl+Z of a later edit (not the apply)*: nothing changes in the sync state. The asset stays resolved.
- *Apply, then close the editor without saving*: the cache still has the old hash, so the next start detects the file again and
  lists it (Automatic mode applies it). An automatic apply is saved at once; if that save failed, the next start applies it again.
- *Apply, then the JSON changes again before saving*: the save (or a Refresh before it) notices that the file is no longer the
  reviewed one. The asset is pending again with the reviewed file as the base, so the new changes are `Json` keys (using JSON) and the
  values the user kept are `Asset` keys. The save does not write the JSON. In Automatic mode it is applied again at the Refresh if
  nothing conflicts.
- *Apply, then Refresh with nothing new*: the asset is skipped; it is not listed again.

**Saving**

- *Save All while assets are pending*: each pending asset is saved, but its JSON is not written, and there is one toast per asset.
- *Asset saved with unsaved edits made after the JSON changed*: the save guard makes it pending without a base. You see every
  differing key as Conflict and decide each one.
- *Read-only JSON file* (e.g. Perforce-style locking): the write fails with a toast. The cache is not updated, so the next save
  tries again.
- *Read-only or locked `cache.json`*: one error toast. Sync state from this session is not kept; the next start compares the
  changed files again, and assets pending only in this session are listed without a base.
- *Autosave*: ignored. JSON is written only on a real save.

**Settings**

- *Changing `JsonDir`*: the next save writes into the new folder. The old folder is left as it is, and the cache's hashes refer to
  old files. A file that appears in the new folder with no matching hash is compared once. Every data asset without a file in the
  new folder is reported as a missing pair until `-export` runs.
- *Switching to Manual*: nothing is applied on start any more; pending assets stay as they are.
- *Adding an excluded folder*: its assets and files are no longer written, read, checked or reported. Existing JSON files stay on
  disk, and their cache entries stay until the folder is synced again.
- *Plugin disabled for a while, then enabled*: assets saved meanwhile have a new saved hash. If the JSON did not change, the next
  check lists them as **asset changed**; if it did, both sides changed and every key is a conflict. Assets added meanwhile are
  reported as having no JSON. Entries without a saved hash (from an older plugin version) still take a base from the saved asset
  in that case, listing the asset's own edits as **JSON** changes, using JSON.

**Rename and delete**

- *Renaming a folder*: one rename event per asset, so every JSON file moves.
- *Renaming a pending asset*: the pending state is dropped. The JSON is moved as it is, with the reviewed or unreviewed JSON changes
  still in it, and the next save compares it with the asset (save guard).
- *Moving an asset into an excluded folder*: its old JSON is deleted and no new one is written.
- *Deleting an asset*: the JSON file is deleted one tick later, once the package file is gone. If the delete is cancelled or the
  package file remains, nothing happens.

**PIE**

- The start-up check, the console command and Refresh refuse to run during PIE, with a warning toast. Saving during PIE still
  writes JSON.

## 6. Weak spots

These are behaviours that may surprise you, ordered roughly by how likely they are to matter. None of them loses data silently except
where noted.

1. **Rename then edit before saving.** The renamed asset has no cache entry. Its first save goes through the save guard. If you
   edited the asset after the rename, the values differ from the moved file, and the save is reported as "did not write its JSON"
   (pending, no base) although nothing came from outside. Resolve it with All from Asset and Apply.
2. **No file watching.** Changes to JSON files are only noticed at start-up, on Refresh or the console command, or by the save
   guard.
3. **The tab's values are not live** while you edit the asset (see [2.8](#28-the-json-sync-tab-sjsonsyncpanel)).
4. **A new pending toast, and a new missing-pairs toast, on every Refresh** while anything is pending or missing.
5. **Some class changes leave the JSON out of date.** Editing a class default does not change the `.uasset`. The JSON lists only
   the overrides, so it usually stays correct. The exceptions (the class default now matches an override, an overridden property
   was removed or renamed) go unnoticed until the asset is saved. Both are harmless or fail loudly. Run `-verify` in CI to catch
   them.
6. **A changed `.uasset` is loaded at start-up** to compare it. After pulling many assets, start-up is slower, once.
7. **Instanced sub-objects are not in the JSON.** Their changes live in the `.uasset` only, so a JSON merge cannot carry them, and
   a `.uasset` change to them alone shows nothing in the tab.
8. **An automatic apply cannot be undone and is saved at once** ([corner cases](#5-corner-cases)), by design. To back out a bad
   JSON, edit the values and save, or revert the JSON in git (the reverted file is a JSON change, so it is applied and saved in
   turn).
