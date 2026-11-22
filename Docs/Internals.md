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

The plugin is a single editor module, `JsonBlueprintSync`. It is never built into a game target.

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
                                        │ (no state, no UI)         │  paths, hashes, asset kinds
                                        └───────────────────────────┘
        UJsonBlueprintSyncSettings (Project Settings, DefaultEditor.ini) is read by all of them.
```

The same data comes in three forms:

| Form | Type | Where |
|---|---|---|
| The asset | `UBlueprint`, `UWidgetBlueprint`, `UDataAsset` and their CDO, component templates and widgets | memory, `.uasset` |
| Document | `FAssetDefaults`: `Self`, `Components`, `Widgets` maps of property text (`overrides.self`, `.components`, `.widgets`) | memory |
| JSON | text written by `ToJson`, read by `FromJson` | `<JsonDir>/Game/...json` |

`Capture` turns an asset into a document, and `Apply` turns a document back into asset values. `ToJson` and `FromJson` convert
between a document and text. The subsystem and the tab compare documents through their **flat** form: one map of key to value, where
the keys are `Property`, `Component.Property`, `Widget.Property` and `Widget.Slot.Property` (`Flatten`).

Terms used below:

- **Archetype**: what a value is compared with. For a Blueprint's own defaults it is the parent class CDO; for a component it is the
  component class CDO or the template inherited from the parent; for a widget it is the widget class CDO; for a data asset it is its
  class CDO.
- **Override**: a value that differs from the archetype. Only overrides are stored. A key missing from the document means "the
  archetype's value".
- **Base**: the values both sides had at the last sync, stored when a difference is first detected (see
  [2.6](#26-sync-state-syncstate)). It is the asset as it was then if only the JSON changed, or the JSON file if only the `.uasset`
  changed. It tells which side changed a key.
- **Saved hash**: `UPackage::GetSavedHash()`, a hash of the `.uasset` file that Unreal records on save and reads from the file
  header on load. The asset registry has it for every package without loading it (`FAssetPackageData::GetPackageSavedHash`).
- **Pending**: the asset's JSON has changes that are not applied or discarded yet. The asset is listed in the JSON Sync tab and
  saving it does not write its JSON.
- **Resolved**: the user applied or overwrote the JSON in this session, but the asset has not been saved since. The plugin
  remembers which file the user reviewed, so a file that changes again before the save is not overwritten.
- **Undone**: an apply that was undone with Ctrl+Z. The asset is pending again, as before the apply.
- **Synced kind**: the asset is a Blueprint, Widget Blueprint or data asset, and the mode for that kind is not None.

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
    JsonBlueprintSyncModule.cpp      tab registration
    JsonBlueprintSyncSettings.cpp
    SyncState.h / .cpp               three-way diff, merge, local cache
    JsonBlueprintSyncSubsystem.h / .cpp   editor runtime
    SJsonSyncPanel.h / .cpp          the JSON Sync tab
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
| UnrealEd | `UBaseWidgetBlueprint`, `FScopedTransaction`, `UEditorLoadingAndSavingUtils`, `GEditor` |
| BlueprintGraph, Kismet, KismetCompiler | `FBlueprintEditorUtils`, `FKismetEditorUtilities` (compile, mark modified) |
| AssetRegistry | asset lookup, class ancestry for unloaded Blueprint classes, rename and remove events |
| Json, JsonUtilities | parsing JSON (writing is done by hand, see [2.5](#serialization-tojson)) |
| EditorSubsystem | `UEditorSubsystem` |
| Slate, SlateCore, InputCore | the tab and notifications. InputCore provides `EKeys`, which the list views reference. |
| WorkspaceMenuStructure | puts the tab under the Window menu |
| UMG | `UWidget`, `UPanelSlot`, `UWidgetTree` |
| UMGEditor | only the tests use it, to build Widget Blueprints |

### 2.3 Module: JsonBlueprintSyncModule

`JsonBlueprintSyncModule.h` declares:

- the log category `LogJsonBlueprintSync`, which every message uses (filter the Output Log by it);
- `JsonBlueprintSync::GetSyncTabName()`, the id `"JsonBlueprintSync"` of the tab;
- `FJsonBlueprintSyncModule`.

`StartupModule` registers a nomad tab spawner. The tab is titled "JSON Sync", is placed in the Level Editor group of the Window menu
(`WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory()`), and holds an `SJsonSyncPanel`. Registration is skipped in commandlets
and when Slate is not initialized (e.g. `-nullrhi` runs), so headless runs do not touch the UI. `ShutdownModule` unregisters it.

Caveat: the editor remembers open tabs in its layout. If the plugin is disabled while the tab was open, the editor shows an empty
"unrecognized tab" placeholder on next start. That is standard Unreal behaviour.

### 2.4 Settings: JsonBlueprintSyncSettings

`UJsonBlueprintSyncSettings` (`config = Editor, defaultconfig`) is shown in Project Settings > Plugins > JSON Blueprint Sync and
saved to `Config/DefaultEditor.ini` under `[/Script/JsonBlueprintSync.JsonBlueprintSyncSettings]`, so it is shared through
source control.

| Property | Default | Notes |
|---|---|---|
| `JsonDir` | `JsonBlueprintSync` | Relative to the project folder; an absolute path also works. Empty falls back to the default. |
| `Blueprints` | `WriteJsonOnly` | `InvalidEnumValues = "TwoWay"` hides Two Way in the drop-down. |
| `WidgetBlueprints` | `WriteJsonOnly` | Same. |
| `DataAssets` | `TwoWay` | |

`GetMode(EAssetKind)` is the only way the rest of the code reads modes:

- `Blueprint` and `WidgetBlueprint` map `TwoWay` to `WriteJsonOnly`, which covers a value typed into the ini by hand.
- `None` (not a synced kind) is always `None`.

Every caller reads the settings at the moment it needs them, never once at start-up. A change in Project Settings therefore takes
effect immediately: the next save, sync or rename uses the new mode or folder.

### 2.5 Library: JsonBlueprintSync

`Public/JsonBlueprintSync.h` and `Private/JsonBlueprintSync.cpp`. It holds no state and shows no UI: the subsystem, the commandlet
and the tests all build on it.

#### Asset kinds

`GetAssetKind(const UObject*)` checks `UBaseWidgetBlueprint` first, then `UBlueprint`, then `UDataAsset`. The order matters
because a Widget Blueprint is also a Blueprint.

`GetAssetKind(const FAssetData&)` does the same without loading the asset. It looks the class up with `FindObject<UClass>` and,
if the class is not loaded (typically the Blueprint class of a data asset), asks the asset registry for its ancestor classes
(`GetAncestorClassNames`). Caveat: the answer is only as good as the registry. Before the registry has finished its initial scan, a
data asset of a Blueprint class may not be recognized. The subsystem only calls it after `OnFilesLoaded`.

`UPrimaryDataAsset`, and Blueprint subclasses of `UDataAsset` or `UPrimaryDataAsset`, are data assets. A `UDataTable`,
`UCurveTable` or any other asset type is `None` and is never synced.

#### Paths

- `GetDefaultJsonRoot()`: `JsonDir`, made absolute against the project folder and normalized (forward slashes, no trailing
  slash).
- `GetJsonPath(Root, "/Game/A/BP_B")` returns `<Root>/Game/A/BP_B.json`. The package path is used as-is, so the folder layout
  mirrors the Content Browser.
- `GetPackageName(Root, Path)` is the inverse. It fails if the file is outside the root, is not `.json`, or does not give a valid
  long package name. The root comparison is case-insensitive, matching Windows file systems.
- `FindJsonFiles(Root, "/Game")` returns every `*.json` under `<Root>/Game`, recursively, normalized and sorted.
- `LoadAsset("/Game/A/BP_B")` loads the object `/Game/A/BP_B.BP_B`, the package's main asset.

#### Which properties are synced

`IsSyncable(Property, Role)` decides. A property is synced only if all of these hold:

- it has `CPF_Edit` (it is visible in the Details panel);
- it is not `Transient`, `DuplicateTransient`, `EditConst` (read-only), `Deprecated`, `Instanced`, does not contain instanced
  references, and is not `PersistentInstance`;
- it is not hidden for the role:
  - **Template** (a Blueprint CDO, a component template, a Designer widget) is edited like class defaults, so properties with
    `DisableEditOnTemplate` (`EditInstanceOnly`) are skipped.
  - **Instance** (a data asset) is edited like an instance, so properties with `DisableEditOnInstance` (`EditDefaultsOnly`, a
    Blueprint variable without Instance Editable) are skipped;
- `ArrayDim == 1`: fixed-size C arrays such as `int32 Values[4]` are skipped;
- it is not an object reference whose class derives from `UActorComponent`. Those references wire components together, and the
  components themselves are synced under `components`.

The same rule is used when capturing and when applying. A file that names a skipped property is therefore an error, not a silent
no-op.

#### Capture

`Capture(Asset)` builds the document from the asset in memory. For each object it walks every property (`TFieldIterator`, which
includes inherited ones), skips the unsynced ones, and stores the value only if it differs from the archetype
(`FProperty::Identical`). The value is written with `ExportTextItem_Direct`, the Details-panel copy format.

Sources by asset kind:

| Section | Blueprint | Widget Blueprint | Data asset |
|---|---|---|---|
| `parent` | parent class path | parent class path | the asset's class path |
| `overrides.self` | `GeneratedClass` CDO vs. super-class CDO (Template) | same | the asset vs. `GetArchetype()` = class CDO (Instance) |
| `overrides.components` | see below | none (widgets are not components) | none |
| `overrides.widgets` | none | every widget in `WidgetTree` and its `Slot` | none |

**Components.** `ForEachComponent` visits three sources, in order:

1. **This Blueprint's own SCS nodes**, i.e. the components added in this Blueprint's Components panel. They are compared with the
   component class CDO.
2. **SCS nodes of parent Blueprints**, i.e. components inherited from a Blueprint parent. The child stores its changes as an override
   template in its `UInheritableComponentHandler`. If the child has no override, the template is null and nothing is captured.
   The archetype is the closest override above the child (a middle parent's), else the parent's original template. A child therefore
   stores only what differs from what it inherits.
3. **Native components**, the default subobjects of the CDO that derive from `UActorComponent`, compared with their archetype
   (the parent class CDO's subobject). These are captured but have not been tested on real assets yet.

The key is the SCS variable name for 1 and 2, and the subobject name for 3. `FindComponent` returns the first match by exact name.

**Widgets.** `ForEachWidget` walks the Widget Blueprint's design-time `WidgetTree` (the Designer's templates, including named-slot
content). Each widget is compared with its class CDO. If the widget has a `Slot` (its layout in the parent panel), the slot is
emitted as `"<Widget>.Slot"` and compared with the slot class CDO. Unreal object names cannot contain `.`, so a widget can never be
named like a slot entry.

Caveats:

- Values come from memory, not from disk. Capturing an asset with unsaved edits captures the unsaved values.
- A Blueprint's defaults come from its generated class. A variable added in the Blueprint editor exists only after a compile, so it
  is not captured (and cannot be applied) until the Blueprint is compiled.
- A Blueprint with no generated class (it has never compiled) captures only `asset` and `parent`.
- Floats are exported in Unreal's canonical text: `2.500000`, not `2.5`. Hand-written values are accepted in any form Unreal can
  parse, but the next export rewrites them in canonical form.

#### Serialization: ToJson

`ToJson` writes the JSON by hand, not through `FJsonSerializer`, so that the output is byte-stable:

- the top-level order is fixed: `schema`, `asset`, `parent`, `overrides`. Inside `overrides`: `self` (always, `{}` if empty),
  then `components` and `widgets` only if they are not empty;
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
6. `overrides` is optional (missing means nothing is overridden) and must be an object with no fields other than `self`,
   `components` and `widgets`, each optional. `self` is an object of string values; `components` and `widgets` are objects of
   objects of string values. A number or a bool where a string is expected is an error ("value must be a string of Unreal property
   text"). Errors name the field by its path, e.g. `overrides.self.Damage` or `overrides.components.Light`. Empty component and
   widget objects are dropped.

Caveats:

- JSON allows duplicate keys. Unreal's reader keeps the last one, without an error.
- Values are not checked here, only their JSON type. Property names and values are validated by `Apply`.

#### Comparing: Flatten and ValuesEqual

- `Flatten` merges the three sections into one map keyed as described in the [overview](#1-overview).
- `ValuesEqual(A, B)` is true if the flat maps have the same keys and the values are equal case-sensitively. It ignores `asset` and
  `parent`.

Caveats:

- Values are compared **as text**. A file with `"Speed": "2.5"` differs from an asset that exports `2.500000`, although the values
  are the same. This is harmless: applying finds the values identical and changes nothing, and the next write puts the canonical
  text in the file.
- `TMap<FString, ...>` is case-insensitive in Unreal, so the flat maps are too. A hand-typed key `"speed"` is matched with the
  asset's `Speed` when comparing, but `Apply` rejects it (see below). The commandlet's `-verify` compares bytes and catches it.

#### Apply

`Apply(Asset, Doc)` makes the asset's synced values match the document **exactly**. Every key in the document is set, and every
override the asset has that the document does not list is reset to the archetype. It runs in two passes.

**Pass 1: validate and parse, writing nothing.**

1. Check the asset kind, and that `Doc.Asset` equals the asset's package name ("the JSON is for X, not Y").
2. A Blueprint must have a generated class ("compile it first").
3. `Current = Capture(Asset)` lists the asset's current overrides.
4. If `Doc.Parent` is set and differs from the asset's parent, add a warning only.
5. For each object (`self`, then each component name and each widget name in either the document or `Current`), find the
   target object. An unknown component or widget is an error. Then, for each property:
   - In the document: find the property by exact, case-sensitive name. A wrong case gives "unknown property (did you mean
     'Speed'?)". Check `IsSyncable`, then parse the text onto a **copy of the archetype's value** (`PrepareWrite`).
   - In `Current` but not in the document: prepare a write of the archetype's value (a reset).

   Parsing (`ImportValue`) is strict:
   - `ImportText_Direct` must succeed;
   - nothing but whitespace may follow the value ("unexpected text after the value");
   - nothing may be written to the error device. During the import the function raises `LogExec` to Verbose, because Unreal reports
     unknown struct fields only at that verbosity. Without this, `(X=1)` for an `FLinearColor` would be accepted silently.
6. If any error was collected, return without writing anything. One bad key blocks the whole file.

**Pass 2: write.** For each prepared write whose value differs from the target's current value:

- **Inherited component without an override yet**: create the override through the child's `UInheritableComponentHandler`
  (`CreateOverridenComponentTemplate`). An earlier write in the same pass may already have created it, so the handler is checked
  first.
- `WriteWithPropagation`:
  1. Before writing, collect the archetype instances that still have the target's old value
     (`PropertyAccessUtil::GetArchetypeInstancesInheritingPropertyValue`). These are child Blueprint CDOs, data assets of the
     Blueprint class, placed actors and their component instances in loaded levels, and so on.
  2. Write the target, then each of those instances, each time calling `Modify`, `PreEditChange`, the copy and
     `PostEditChangeProperty(ValueSet)`. This is the sequence the Details panel uses, so undo, editor refresh and
     construction-script reruns behave the same.

  Instances that override the value differ from the old value, so they are not collected and keep their override.

Consequences:

- Because missing keys are reset, `Apply` with a *partial* document would wipe the other overrides. The subsystem never does
  that: it always builds a complete document with `TakeKeys` (see [2.6](#26-sync-state-syncstate)).
- Struct values are parsed onto the archetype's value. A struct text that lists only some fields leaves the others at the
  **archetype's** values, not at the asset's current ones.
- Arrays, sets and maps are replaced as a whole by the parsed text.
- `Apply` does not compile, mark the asset modified, save, or open a transaction. Callers do that.
- `Modify()` on placed actors marks their level dirty, and propagation marks child Blueprints' packages dirty. Applying one
  Blueprint can therefore dirty several packages.

#### Import, MarkModified, GetApplyOrder

- `MarkModified`: `FBlueprintEditorUtils::MarkBlueprintAsModified` for Blueprints (package dirty, Blueprint status "needs compile"),
  and `MarkPackageDirty` for data assets.
- `Import` = `Apply`, then, only if something changed, `MarkModified` and, for Blueprints, `CompileBlueprint`. Only the
  commandlet and the tests use it. The editor does not compile, because a compile replaces the CDO and the change could no longer
  be undone.
- `GetApplyOrder`: a Blueprint's order is the number of Blueprint classes above it (0 for a Blueprint whose parent is native).
  Data assets are `MAX_int32`. Sorting by it applies parents before children and Blueprints before data assets. This matters because
  a child's values are deltas on its parent: applying the parent first lets propagation run before the child's own values are
  applied on top.

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
result, which resets it to the archetype. This is how a partial choice becomes the complete document that `Apply` needs:
`Apply(TakeKeys(Capture(Asset), File, KeysThatUseJson))`.

Details:

- A key is split at the **last** `.`: `Title.Slot.ZOrder` → object `Title.Slot`, property `ZOrder`. A key without a `.` is a
  `self` key.
- Whether an object name is a widget or a component is decided by which section has it, in `Source` or `Current`. A Blueprint has
  either components or widgets, never both, so there is no ambiguity in practice.
- Objects left with no values are removed.

#### ChooseBase(Inputs)

ChooseBase decides where the base of a newly detected difference comes from. Its inputs are facts the subsystem gathers:

| Input | Meaning |
|---|---|
| `bPending` | already pending (a base may be stored) |
| `bHasEntry` | the asset has a cache entry, i.e. it was synced on this machine |
| `bJsonChanged` | the file's hash differs from the entry's `JsonHash` |
| `bAssetChanged` | the package's saved hash differs from the entry's `AssetHash` (false if either is unknown) |
| `bAssetDirty` | the asset has unsaved edits |
| `bParentDiffers` | the file's `parent` is set and differs from the asset's |

The rules, first match wins:

| Rule | Result | Why |
|---|---|---|
| pending | `Stored` | keep the base from the first detection |
| no entry | `None` | nothing is known about the last sync |
| JSON unchanged | `File` | the file is what was synced, so the asset changed every key that differs. This holds even if the asset is dirty or reparented. |
| parent differs, asset changed, or asset dirty | `None` | both sides moved, or the file's values are relative to another archetype |
| otherwise | `Current` | the asset is what was synced, so the JSON changed every key that differs |

#### CanAutoApply(bHasBase, bParentDiffers, Keys)

True only if there is a base, the parents agree, no key is `Both`, and at least one key is `Json`. The last condition keeps a JSON
that is only out of date (every key `Asset`) in the list: applying it would change nothing, and the user should overwrite the JSON
instead.

#### FSyncCache

The cache is local state in `<ProjectSaved>/JsonBlueprintSync/`. It is never committed: each machine has its own.

```
Saved/JsonBlueprintSync/
  cache.json                      { "version": 2, "entries": { "/Game/A/BP_B": { "json": "<md5>", "asset": "<io hash>", "pending": true } } }
  Base/Game/A/BP_B.json           base snapshot of a pending asset (same format as the synced files)
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
if only the JSON changed, or the parsed file if only the `.uasset` changed. It is stored as a schema-2 document. It stays fixed while the asset is pending: later checks reuse it instead of capturing again. That is what lets
the tab see edits made to the asset after the detection as `Asset` or `Both`. It is deleted when the pending state ends. Deleting
the `Base` folder by hand is safe: pending assets then show every key as a conflict.

### 2.7 Editor subsystem: JsonBlueprintSyncSubsystem

`UJsonBlueprintSyncSubsystem` is a `UEditorSubsystem`. `ShouldCreateSubsystem` returns false in commandlets, so the commandlet
and the subsystem never run together.

#### State

| Member | Lives | Holds |
|---|---|---|
| `Cache` | disk (`Saved/`) | `FSyncCache`: JSON hash, saved hash and pending flag per package, plus base files |
| `Pending` | memory | package → `FPending { Kind, File (parsed doc, unset if broken), Error, Base (unset if unknown), FileHash (of the file compared) }`. Mirrors the cache's pending entries once a sync has run. |
| `Resolved` | memory, this session only | package → `FResolved { Kind, FileHash, ReviewedFile, Before, TransactionId }` for assets applied or overwritten but not saved since. `FileHash` and `ReviewedFile` are the file the user reviewed; `Before` is the pending state before the apply (unset for Overwrite JSON); `TransactionId` is the apply transaction, if it changed the asset. |
| `Undone` | memory, this session only | the same records for applies that were undone, so that a redo can resolve them again and Two Way does not re-apply the same file |
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

`OnFilesLoaded` shows the error toast for an unreadable cache, if any, then calls `SyncFromJson`.

`Deinitialize` undoes all of this and closes the toast.

#### SyncFromJson: the start-up check

This runs at start-up, on `JsonBlueprintSync.Sync` and on the tab's Refresh button.

1. Refuses during Play In Editor, with a warning toast.
2. For every JSON file under `<JsonRoot>/Game`:
   1. Map the file to a package. Failure, or an unreadable file, is an error ("cannot read the file").
   2. Get the package's saved hash (`GetSavedHash`): from the package if it is loaded, else from the asset registry. The asset is
      **changed** if the entry has an `AssetHash`, the saved hash is known, and they differ.
   3. **Skip** a resolved asset whose file is still the reviewed one (same hash as `FResolved::FileHash`): its next save writes
      the JSON.
   4. **Skip** the file, without loading anything, if the asset is not resolved, the cache has a non-pending entry with the same
      JSON hash and the asset has not changed. This is what keeps start-up cheap. An entry without an `AssetHash` (from an older plugin version) adopts the
      current saved hash here, so detection works from then on.
   5. Find the package's main asset in the registry. If there is none, report an error ("no asset").
   6. If the asset's kind is not synced (mode None), drop any pending, resolved or undone state it had and move on.
   7. A resolved asset whose file changed after the review becomes pending with the reviewed file as the base
      (`MarkChangedAfterReview`, see [OnPackageSaved](#onpackagesaved-write-on-save-and-the-save-guard)). Any other asset goes to
      `Detect` with the asset-changed flag.
   8. If the asset became or stayed pending and its mode is Two Way, remember it for auto-apply, unless it is in `Undone` with the
      same file hash: the user undid the apply of this very file. A file that changed since is applied as usual.
3. Pending assets whose JSON file no longer exists are dropped: there is nothing left to review.
4. Save the cache, log a summary, and show one error toast if any file could not be checked: unreadable, no asset, or (from
   `Detect`) the asset cannot be loaded. Each is logged as an error. Nothing is recorded for them, so the next sync tries again.
5. **Two Way auto-apply.** For each remembered asset, get the view (see below). The asset is applied only if the file parses and
   `CanAutoApply` agrees:
   - there is a base;
   - the parents agree;
   - no key is `Both`;
   - at least one key is `Json` (a JSON that is only out of date stays listed).

   The keys applied are the `Json` keys; `Asset` keys keep the asset's value. All such assets are applied in one batch
   (`ApplyBatch`), as one undo step.
6. Show the pending toast (Review / Later) if anything is pending, and broadcast `OnPendingChanged`, which refreshes the tab.

#### Detect: one changed file

`Detect(Package, Text, Hash, Kind, bAssetChanged, Errors)` loads the asset, captures it as `Current` and parses the file. It runs
when the JSON or the `.uasset` changed (or the asset is pending). If the asset cannot be loaded, it adds an error to `Errors` and
returns.

**If the file parses, its `parent` agrees with the asset's, and `ValuesEqual(File, Current)`**, the two sides agree, e.g. a teammate
committed both, or only the asset's graph changed. The asset is removed from `Pending`, and then:

| Asset dirty? | Was pending? | Cache result |
|---|---|---|
| no | – | base deleted, entry = `{file hash, current saved hash, not pending}` |
| yes | yes | base deleted, entry = `{old hashes, not pending}` |
| yes | no | unchanged |

Only a match with the **saved** asset is recorded. Recording a match with unsaved values would hide the difference if the editor
were closed without saving. Then the asset on disk would differ from the file, but the hash would say "in sync".

**Otherwise**, they differ (or the file is broken, or the parent differs). `ChooseBase` picks the base (see
[2.6](#choosebaseinputs)):

| Result | Base |
|---|---|
| `Stored` | the stored base file (none if it is missing) |
| `File` | the parsed file, saved as the base file. Every differing key shows as `Asset`: the JSON is out of date. |
| `Current` | `Current`, saved as the base file. Every differing key shows as `Json`. |
| `None` | none: every key is a conflict |

A parent mismatch is logged as a warning. The entry becomes `{old hashes or "", pending}`. The asset is added to `Pending` (with the
file's hash) and removed from `Resolved`. The differing keys are
logged. A broken file is logged as a warning and is pending with its error.

Note that Detect loads every asset whose JSON or `.uasset` changed. After a large pull this can take a while at start-up.

#### GetPendingView

GetPendingView builds what the tab shows for one pending asset: kind, error, `bHasBase`, `DiffKeys(Base, File, Capture(asset))`,
and `JsonParent` / `AssetParent` when the parents differ. It captures the asset **now**, so the view reflects the asset's current
in-memory values. `IsJsonOutOfDate()` is true when there is a base and every key is `Asset`.

#### ApplyJson, ApplyBatch and Resolve

`ApplyJson(Package, Keys, bSave)` is the tab's **Apply** (`bSave` false) and **Apply and Save** (`bSave` true): `ApplyBatch` with one
request, then `OnPendingChanged`. With `bSave`:

- `ApplyBatch` shows no success toast; `ApplyJson` shows its own.
- If the asset is dirty after the apply, it is saved with `FEditorFileUtils::PromptForCheckoutAndSave(bCheckDirty, no prompt)`, which
  checks the file out first if source control needs it. The save goes through `OnPackageSaved` like a Ctrl+S: the asset is
  resolved and the file is the reviewed one, so the JSON is written and `Resolved` is cleared. Toast: "Applied the JSON to … and
  saved it."
- If the save fails or is declined, the asset stays dirty and resolved, as after a plain Apply. Toast (warning): "Applied the JSON
  to …, but it was not saved."
- If the asset is not dirty (every key used the asset, and it had no unsaved edits), `Resolve` has already written the JSON, so
  nothing is saved. Toast: "Wrote the JSON of …; the asset did not change."

`ApplyBatch(Requests)`:

1. For each request, load the asset and build `TakeKeys(Capture(asset), File, Keys)`. A package that is not pending, has a broken
   file, or cannot be loaded fails.
2. Sort by `GetApplyOrder` (stable): parents first, data assets last.
3. Inside one `FScopedTransaction("Apply JSON")`, call `Apply` for each. Warnings and errors go to the log. A failed asset is skipped,
   stays pending, and the others go on. Changed assets are `MarkModified`. **Nothing is compiled and nothing is saved.** If nothing
   changed, the transaction is cancelled, so no empty undo step is left. The transaction's id (`GUndo->GetContext().TransactionId`)
   is kept for the assets it changed.
4. `Resolve` each asset that succeeded, with the transaction id if the apply changed it, then save the cache.
5. Toasts: "Applied JSON to … Not saved; Ctrl+Z reverts." and/or "Cannot apply the JSON of …".

`Resolve(Package, Asset, TransactionId)`:

1. Remove the package from `Pending` and `Undone`, delete its base, and clear the pending flag. The hash is kept, because it still
   describes the last file that matched the saved asset.
2. If the asset is now dirty, add it to `Resolved` with the reviewed file (the pending state's `File` and `FileHash`), the pending
   state itself as `Before`, and the transaction id. Its next save writes the JSON, including the keys where the user kept the
   asset's value, as long as the file is still the reviewed one.
3. If it is not dirty (nothing changed, e.g. every key used the asset), write the JSON **now**. Otherwise no save would ever
   write it. In effect, applying with every key on Asset is the same as Overwrite JSON.

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

A Two Way asset in `Undone` is not auto-applied again while its file is the one that was undone. `Undone` is not saved, so the next
editor start applies it again.

#### OverwriteJson

OverwriteJson loads the asset and clears the pending state the same way as `Resolve`. It then writes `Capture(asset)` to the file
and, if the asset is dirty, adds it to `Resolved` with the file now on disk as the reviewed file (the one just written, or the old
one if the write failed). It has no transaction, so there is nothing to undo. It also works when the file is broken. The tab offers
it only then (**Overwrite JSON**, shown only for "Cannot read the JSON"), as the way out of an unparsable file besides fixing it.
For a readable file, **All from Asset** + **Apply** does the same through `Resolve`. The save guard also calls it for a JSON that is
only out of date.

Caveat: if the asset has unsaved edits, the file gets the **unsaved** values. If you then close the editor without saving, the file
has values the `.uasset` does not. The next start detects this: the hash still differs, so the file is compared again and listed as
JSON changes.

#### WriteJson

WriteJson calls `SaveFileIfChanged(Path, Capture(asset))`. A write failure shows "Cannot write … Is it read-only?". Then, **only if
the asset is not dirty**, it records `{hash of what was written, package saved hash, not pending}`, removes the asset from
`Resolved` and `Undone` and saves the cache. The save event fires after Unreal clears the dirty flag and sets the new saved hash, so a normal save
records the sync.

#### OnPackageSaved: write on save and the save guard

The save event fires for every saved package. The handler returns at the first condition that applies:

1. Autosaves, procedural saves (e.g. during cooking) and cooking are ignored.
2. The package is not under `/Game/`, or is a temp package: ignored.
3. `Package->FindAssetInPackage()` is not a synced kind, or its mode is None: ignored.
4. **Pending** (in `Pending` or flagged in the cache):
   - if the file's hash still equals the entry's `JsonHash` (`IsJsonUnchanged`), the JSON has no changes of its own and is only
     out of date: `OverwriteJson`, which clears the pending state and writes the file;
   - otherwise skip writing, and show "Saved X, but did not write its JSON: the JSON has changes you have not reviewed" with a
     Review button.
5. **Resolved**: the file may differ from the last sync, but it must still be the reviewed file.
   - If the file exists and its hash differs from `FResolved::FileHash`, it changed after the review (a pull with the editor open
     and no Refresh, a hand edit). `MarkChangedAfterReview` makes the asset pending with the **reviewed file as the base**: keys
     the new file changed are `Json`, the values the user kept or edited since are `Asset`, and a key both changed is `Both`. The
     base is stored, so this survives a restart. The toast reads "Saved X, but did not write its JSON: the JSON changed again
     after you reviewed it", with a Review button.
   - Otherwise (same file, or no file) `WriteJson`.
6. **Save guard**: if the file exists and its hash differs from the cache entry (or there is no entry), the file changed since the last sync without the plugin seeing it (a pull with the editor open and no Refresh, a hand
   edit, another tool). Writing now would silently discard that change. So:
   - if the file parses and its values equal the asset's, write: nothing would be lost, and this only normalizes the file;
   - otherwise, mark it **pending without a base** (`MarkPending`), show the skipped-save toast and refresh the tab. There is no
     base because the asset has been edited since the file changed, so either side may have changed any key.
7. Otherwise, `WriteJson`.

Resolved assets get step 5 instead of the save guard: after an apply the file still differs from the cached hash, and the save
that follows is meant to write it.

#### OnAssetRenamed

This also covers moves.

1. Only for synced kinds whose old package is under `/Game` and actually changed.
2. Drop the old package's pending and resolved state. A pending rename is lost; the renamed asset is not pending.
3. If there is no old file, just remove the old cache entry. If it exists but cannot be read or parsed (e.g. a merge conflict),
   it is left at the old path, the old cache entry is removed, and a warning toast says so. Saving the renamed asset writes a new
   file; the old one gives a "no asset" error at every sync until it is fixed or deleted.
4. Otherwise, write the file at the new path with `asset` updated, **unless a file already exists there**. In that case the renamed
   package was already saved and its JSON is newer. Then delete the old file and remove the old cache entry. No entry is created for
   the new package: its next save creates it (see [weak spots](#6-weak-spots)).

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
  | "N JSON files could not be checked" | Error | a sync found unreadable files, files with no asset, or assets that cannot be loaded |
  | "cannot read cache.json" | Error | at the first sync, if `Initialize` could not read the cache |
  | "cannot write cache.json" | Error | the first failed cache save; later failures are only logged until a save works |
  | "cannot write / delete <file>" | Error | a JSON file cannot be written (save, Overwrite JSON, rename) or deleted (rename, delete) |
  | "renamed A to B, but its JSON cannot be read" | Warning | rename of an asset whose JSON does not parse |
  | "not checked during Play In Editor" | Warning | the console command or Refresh during PIE |
  | "Cannot apply the JSON of …" | Error | an apply failed |
  | "Applied the JSON to …, but it was not saved" | Warning | Apply and Save could not save the asset (declined checkout, failed save) |

- `ShowPendingNotification` replaces the previous "N asset(s) differ" toast with a sticky one: up to eight names, then "and N more",
  with Review (opens the tab) and Later (closes the toast) buttons. It is shown after every `SyncFromJson` that leaves something
  pending, including the Refresh button.
- `NotifySaveSkipped` shows a 10 s toast with a Review button, and logs a warning.

### 2.8 The JSON Sync tab: SJsonSyncPanel

`SJsonSyncPanel` is a compound widget laid out like a merge tool:

- a summary line and a **Refresh** button (= `SyncFromJson`);
- on the left (30%), the **asset list**: Asset, Type, Status. Status, first match wins: "Cannot read the JSON" (orange),
  "N keys, parent differs" (orange), "N keys, JSON out of date", "N keys, conflicts" (orange), "N keys". Double-clicking an asset
  opens its editor;
- on the right (70%):
  - a detail line: the package, and, first match wins, the parse error, the two parents, the "JSON has no changes of its own" note,
    the "unknown which side changed, every key is a conflict" note, or how to choose;
  - the **key list** (multi-select): Key, JSON value, Asset value, Changed in (JSON / Asset / Conflict, with tooltips; Conflict in
    orange), Use (JSON / Asset / **Choose** in orange). `(default)` in grey means the key is not set on that side. The value the
    key uses has a highlighted background (the theme's primary colour); the other one is dimmed;
  - below it: **All from JSON**, **All from Asset**, **Reset**, the counter "N conflicts to choose" (orange), **Show in Content
    Browser**, **Overwrite JSON** (only for an unreadable file), **Apply** and **Apply and Save** (primary).

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

`FindSyncedPackages(Dir)` runs a synchronous registry scan of `Dir`. It then filters by class `UBlueprint` or `UDataAsset`
(recursive, which includes Widget Blueprints and Blueprint data asset classes), keeps only main assets, and drops kinds whose mode is
None. Unlike the subsystem, it is not limited to `/Game`: `-dir=/MyPlugin` works.

| Action | What it does | Exit code 1 when |
|---|---|---|
| `-export` | `SaveFileIfChanged(Capture(asset))` for each synced asset. Warns about JSON files with no synced asset. | an asset cannot be loaded or a file cannot be written |
| `-verify` | Compares each file **byte for byte** (line endings normalized) with `ToJson(Capture(asset))`. | a file is missing, broken, or differs; or a JSON file has no synced asset |
| `-import` | Loads every JSON file under `Dir`, sorts by apply order, and runs `Import` (Apply + mark modified + compile). The whole file is applied, **whatever the mode**, except None. It then saves every dirty Blueprint and data asset package unless `-nosave` is given. | a file is broken or has no asset, an apply fails, or saving fails |

Caveats:

- `-import` ignores Write JSON Only and Two Way. It is meant for deliberate bulk use (CI, a scripted migration), not for teammates'
  everyday sync.
- `-import` saves **every** dirty Blueprint and data asset in memory, not only the imported ones. This catches children the values
  propagated to, but also any asset that got dirty on load. It does not save levels. Placed instances in levels that were not
  loaded need no saving: Unreal stores instances as deltas on their archetype, so they pick up the new default on load.
- `-verify` is strict about formatting. A hand-edited file with the same values but different spacing or key order fails, as does a
  float written as `2.5`. Re-export to fix it.
- The commandlet does not read or write the editor's cache. The next editor start handles its changes: the file hashes no longer
  match the cache, so the files are compared again, match their assets, and are recorded.
- In Git Bash, set `MSYS_NO_PATHCONV=1`, or `/Game/...` arguments are turned into Windows paths.

### 2.10 Automation tests

`Private/Tests/JsonBlueprintSyncTests.cpp` holds 14 tests under `JsonBlueprintSync.*`. They are compiled only with
`WITH_DEV_AUTOMATION_TESTS`. They build their own Blueprints, Widget Blueprints and data assets in `/Temp` packages marked
`PKG_InMemoryOnly`, so they never touch project content or the JSON folder.

| Test | Covers |
|---|---|
| `Format` | output format, key order, escaping, round trip, rejected documents (bad JSON, schema 1 and 3, the old `defaults` layout, missing asset, `reset` field, unknown fields at the top level and in `overrides`, non-object `overrides` or component, number value), a file without `overrides`, path mapping, hash ignoring line endings |
| `RoundTrip` | capture → import → capture is stable; re-importing the export changes nothing |
| `Propagation` | a parent import reaches the child CDO and placed instances, but not values they override |
| `RejectsBadValues` | unknown property, wrong case, bad array, unknown struct field, trailing text, unknown component or property, wrong asset; nothing applied in each case |
| `MissingKeyResets` | keys missing from the document go back to the archetype; children follow |
| `Merge` | `DiffKeys` classification (Json, Asset, Both, no base), `TakeKeys` + `Apply` keeps the local change |
| `TakeKeys` | take, remove, drop empty objects, slot keys split at the last dot; `ValuesEqual` ignores asset and parent |
| `ChangedAfterReview` | with the reviewed file as the base, a later file change is `Json`, a kept asset value is `Asset`, both is `Both`; `CanAutoApply` on each |
| `Cache` | cache and base round trip (including the asset hash), an old cache version is dropped, a broken cache reports failure |
| `BaseChoice` | `ChooseBase` for every rule, `CanAutoApply` (base, parent, conflict, out of date, no keys), an out-of-date JSON diffs as `Asset` keys |
| `Settings` | defaults, Two Way refused for Blueprints and Widget Blueprints, None |
| `DataAsset` | Blueprint data asset class: capture, apply, class-only variables and components rejected, reset |
| `InheritedComponent` | child override created through the inheritable component handler, exported as a delta, reset back to the parent |
| `Widget` | widgets and slots captured, applied to the Designer templates and the compiled tree, reset, unknown widget rejected |

What the tests do **not** cover: the subsystem itself (save guard, Detect, start-up and saved hashes, Two Way auto-apply, undo and
redo, rename, delete), the tab, the commandlet, and native (C++) components. Its decision rules are covered through `ChooseBase` and
`CanAutoApply`. [ManualTests.md](ManualTests.md) covers those by hand.

## 3. End-to-end flows

### Saving an asset (normal case)

1. You press Ctrl+S on `BP_Lamp`.
2. Unreal saves the package and clears the dirty flag.
3. `OnPackageSaved`: it is a synced kind, not pending, and the file's hash matches the cache.
4. `WriteJson`: capture, write the file if different, record the hash.

### A teammate's change arrives (editor closed during the pull)

1. `git pull` brings `BP_Lamp.uasset` and `BP_Lamp.json` together.
2. Editor start → `SyncFromJson` → the hash differs from the cache → `Detect` → the asset (the new `.uasset`) matches the file →
   the new hash is recorded. Nothing is listed.

If the teammate committed only the JSON, the JSON hash changed and the saved hash did not. `Detect` sees a difference, takes the
asset as the base, and the asset becomes pending with every key as `Json`. For a Blueprint, it is listed for review. For a data asset
in Two Way mode, it is applied automatically and left unsaved.

If the teammate committed only the `.uasset`, the JSON hash is unchanged but the saved hash differs. `Detect` takes the file as the
base, so every differing key is `Asset`, and the asset is listed as "JSON out of date", even in Two Way mode. Saving the asset or
Overwrite JSON writes the JSON. If its values did not change (e.g. only the graph did), the new saved hash is recorded and nothing is
listed.

If the teammate committed both but they disagree, both hashes changed: pending without a base, every key a conflict.

### Reparenting a Blueprint

1. You reparent `BP_Child` from `BP_A` to `BP_B`. The compile keeps the child's own overrides and takes the rest from `BP_B`.
2. You save: the JSON is written with the new `parent` and the deltas against `BP_B`. Nothing else is needed.
3. A teammate pulls both files: they match. If they pull only the `.uasset`, the JSON is out of date and names `BP_A`: listed as
   "parent differs", with the keys as `Asset` changes. Apply with every key on Asset (or saving the asset) fixes it.
4. If the JSON also changed (both sides moved, or the JSON still describes the old parent), there is no base: every key is a
   conflict and Two Way applies nothing.

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

1. `BP_A` → `BP_B`: the JSON is rewritten at the new path with `asset` updated, and the old file and cache entry are removed.
2. The next save of `BP_B` finds no cache entry. The guard compares values; they are equal, so the JSON is written and recorded.

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
        │ parent, never synced, or save guard           │ Apply / Apply and Save / Overwrite JSON
        ▼                                               ▼
  ┌──────────────────┐  Apply / Overwrite        ┌──────────────┐
  │ PENDING, no base │──────────────────────────▶│ RESOLVED     │  (asset dirty)
  │ (all Conflict)   │                           │ next save    │
  └──────────────────┘                           │ writes JSON  │
        ▲  both sides become equal: back to SYNCED └──────┬───────┘
        │  (if the asset is saved)                        │ save
        │                                                 ▼
        └──── Two Way, no conflict: auto-applied ──▶ RESOLVED ──▶ SYNCED
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
  Conflict, and Two Way does not apply them.
- *JSON file deleted, asset kept*: nothing happens until the asset is saved, which writes the file again. `-verify` reports it
  missing.
- *Asset deleted (by a pull), JSON kept*: the start-up check reports "no asset" as an error on every start until the file is
  removed.
- *JSON for an asset whose mode is None*: ignored silently. Its pending state is dropped.
- *Git merge conflict markers in a JSON file*: the file does not parse. The asset is listed as "Cannot read the JSON". Fix the file
  and Refresh, or use Overwrite JSON (the only action the tab offers for it).
- *Only whitespace or formatting changed in the JSON*: the hash changes, `ValuesEqual` is true, so at start-up the new hash is just
  recorded, and on save the file is rewritten in canonical form.
- *Wrong `asset` field* (e.g. a file copied by hand): the editor never checks it, because `ValuesEqual` ignores it and `TakeKeys`
  keeps the asset's own name. `-import` rejects it and `-verify` reports it. The next write from the editor fixes it.
- *`parent` mismatch* (a reparent, or a data asset whose class changed): the editor lists the asset as "parent differs" and
  applies nothing automatically, even when the values are equal. If only the `.uasset` changed, the keys are `Asset` changes and
  saving the asset fixes the JSON; if the JSON changed too, there is no base. Applying keys that use JSON merges onto the asset's own
  parent. `-import` only warns. An empty `parent` in a hand-written file makes no claim.
- *`.uasset` changed, JSON not* (committed alone, or edited with the plugin's mode None): detected at start-up through the saved
  hash. Listed as "JSON out of date" if the values differ, recorded silently if they do not.
- *Change inherited from a parent Blueprint or the C++ class*: the child's `.uasset` does not change, so nothing is detected. The
  child's JSON is out of date until the child is saved. `-verify` catches it.
- *Cache from an older plugin version* (no `asset` field): the first start adopts the current saved hash for entries whose JSON
  matches. A `.uasset` that changed before that first start is not noticed.
- *Loaded, dirty package at Refresh*: the saved hash is taken from the package in memory, not from the file. A `.uasset` replaced on
  disk while it is loaded is therefore not noticed until Unreal reloads it or the editor restarts.

**Applying**

- *One bad value*: nothing in that asset is applied, it stays pending, and the Output Log lists the problems. Other assets in the
  same batch are still applied.
- *Unknown key in the JSON* (e.g. a renamed variable): using JSON for it, the apply fails. Using the asset, the apply succeeds, and
  the next save removes the key from the JSON.
- *Every key uses the asset*: the asset keeps every value, and the JSON is overwritten at once (asset not dirty) or on the next
  save (Apply and Save: at once).
- *Apply and Save, then Ctrl+Z*: the save already wrote the merged JSON and cleared `Resolved`, so the undo is an ordinary edit.
  The asset is modified with its old values, and saving it writes them to the JSON. Nothing is lost silently: both files are on
  disk and the change shows in git.
- *Apply and Save, checkout declined*: nothing is saved. The asset stays modified and resolved, as after Apply.
- *Ctrl+Z after Apply or a Two Way auto-apply*: the values go back and the asset is pending again, with the base it had
  before, and a warning toast. Saving it does not write the JSON. Ctrl+Y resolves it again.
- *Ctrl+Z of a Two Way auto-apply, then a restart*: `Undone` is not kept, so the next start applies the file again. Choose All
  from Asset and Apply in the tab to keep the asset's values.
- *Ctrl+Z after Apply, then edits, then Ctrl+Y*: Unreal drops the redo history when you edit, so there is nothing to redo. The
  asset stays pending.
- *Ctrl+Z of a later edit (not the apply)*: nothing changes in the sync state. The asset stays resolved.
- *Apply, then close the editor without saving*: the cache still has the old hash, so the next start detects the file again and
  lists it (or Two Way applies it again).
- *Apply, then the JSON changes again before saving*: the save (or a Refresh before it) notices that the file is no longer the
  reviewed one. The asset is pending again with the reviewed file as the base, so the new changes are `Json` keys (using JSON) and the
  values the user kept are `Asset` keys. The save does not write the JSON. A Two Way asset is applied again at the Refresh if
  nothing conflicts.
- *Apply, then Refresh with nothing new*: the asset is skipped; it is not listed again.
- *Applying a parent Blueprint*: child Blueprints, data assets of the class and placed actors that inherit the value are modified
  too, and their packages and levels become dirty. Overrides are kept.
- *Widget Blueprints*: the Designer updates at once. The compiled widget tree updates on the next compile, which PIE does for dirty
  Blueprints.
- *Blueprints in general*: no compile after apply. The Blueprint shows as needing a compile, and the values survive the compile.

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
  old files. A file that appears in the new folder with no matching hash is compared once.
- *Two Way written in the ini for Blueprints*: treated as Write JSON Only.
- *Mode set to None*: saving stops writing, the start-up check ignores the files, and existing JSON files stay on disk.
- *Mode None, then back on*: assets saved while the mode was None have a new saved hash. If the JSON did not change, the next
  check lists them as "JSON out of date"; if it did, both sides changed and every key is a conflict. Entries without a saved hash
  (from an older plugin version) still take a base from the saved asset in that case, listing the asset's own edits as **JSON**
  changes, using JSON.

**Rename and delete**

- *Renaming a folder*: one rename event per asset, so every JSON file moves.
- *Renaming a pending asset*: the pending state is dropped. The JSON is moved as it is, with the reviewed or unreviewed JSON changes
  still in it, and the next save compares it with the asset (save guard).
- *Renaming an asset whose mode is None*: its JSON (if any) is not moved.
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
4. **A new pending toast on every Refresh** while anything is pending.
5. **Inherited changes are not detected.** Editing a parent Blueprint or a C++ default changes a child's values without changing
   its `.uasset`, so the child's JSON goes out of date silently until the child is saved. Run `-verify` in CI to catch this.
6. **A changed `.uasset` is loaded at start-up** to compare it, even if only its graph changed. After pulling many assets, start-up
   is slower, once.
7. **`-import` saves every dirty Blueprint and data asset in memory**, not only those it changed.
8. **Native (C++) components** are captured and applied with the same code as Blueprint components, but have not been tested on
   real assets.
9. **An undone Two Way apply is applied again after a restart** ([corner cases](#5-corner-cases)): the plugin only remembers undos
   for the session.
