# JsonBlueprintSync

An Unreal Engine editor plugin that keeps the values of **data assets** in JSON files next to the project. **The JSON is the
source of truth**: the `.uasset` follows it.

- When you save a data asset in the editor, its JSON file is written.
- When the JSON files change on disk (e.g. after a `git pull`), the changes are applied to the assets, either automatically or key by
  key in the JSON Sync tab.

`.uasset` files are binary, so this gives you diffs, code review and merges for data. Only values that differ from the class
defaults are stored, so a file holds just what the asset itself sets.

Blueprints and Widget Blueprints are not synced.

## Requirements

- Unreal Engine 5.8 (the version it is developed and tested with).
- A C++ project, so the plugin can be compiled with it. Blueprint-only projects need a prebuilt copy (see below).
- Editor only: it is never built into or shipped with the game.

## Installation

1. Add the plugin to the project's `Plugins/` folder, for example as a git submodule:

   ```
   git submodule add https://github.com/yeahBash/JsonBlueprintSync.git Plugins/JsonBlueprintSync
   ```

   Teammates then need `git clone --recurse-submodules`, or `git submodule update --init` in an existing clone.

2. Enable it in the `.uproject` for the editor only:

   ```json
   "Plugins": [
     { "Name": "JsonBlueprintSync", "Enabled": true, "TargetAllowList": [ "Editor" ] }
   ]
   ```

3. Make git keep the JSON files' line endings as LF. Add this to the project's `.gitattributes` (or scope it to the JSON folder):

   ```
   *.json text eol=lf
   ```

   Files are compared byte for byte, and CRLF checkouts on Windows would otherwise show every file as changed.

4. Write the JSON of the existing data assets once, with the commandlet's `-export` (see below), and commit the JSON folder
   (`<Project>/JsonBlueprintSync/` by default) alongside the assets.

### Prebuilt copy for Blueprint-only projects

```
<Engine>\Engine\Build\BatchFiles\RunUAT.bat BuildPlugin -Plugin="<path>\JsonBlueprintSync.uplugin" -Package="<output folder>" -TargetPlatforms=Win64
```

Copy the output folder to the project's `Plugins/JsonBlueprintSync/`. Binaries only work with the engine version they were built with.

## What is synced

- Every data asset: an asset whose class derives from `UDataAsset`, including `UPrimaryDataAsset` and Blueprint subclasses of either.
  Only a package's main asset is synced.
- Under `/Game` and the content of the project's own plugins (e.g. `/MyPlugin`). Engine and engine plugin content is not synced,
  and neither is the content of a plugin that is disabled.
- Not in a folder listed in **Excluded Folders** (see Settings), e.g. a marketplace pack.
- Only properties editable on the asset in the Details panel. See Limitations for what is skipped.

## Modes

Set in **Project Settings > Plugins > JSON Blueprint Sync**. Saving a data asset writes its JSON in both modes.

| Mode | JSON changed on disk |
|---|---|
| **Automatic** (default) | Applied when the editor starts (or on Refresh), and the asset is saved. Not applied when a key conflicts or the class differs: those are listed in the JSON Sync tab. |
| **Manual** | Listed in the JSON Sync tab, where you apply the changes key by key. Applying is an undo step and does not save the asset. |

The automatic apply saves the asset at once (checking it out first if source control needs it), so the `.uasset` on disk, which is
what the game uses, matches the JSON. It is not an undo step: Ctrl+Z cannot quietly bring back the old values and have the next save
write them over a teammate's change. To back out a JSON change, edit the values and save (which writes the JSON), or revert the JSON
file in source control. If the save does not happen (e.g. you decline the checkout), a warning says so and the asset is left
modified: save it yourself.

Two checks keep this safe. They matter only for a Refresh or `JsonBlueprintSync.Sync` later in the session; at editor start
neither applies. An asset is not applied automatically, but listed in the JSON Sync tab with a log line, if:

- it has unsaved changes, because the automatic save would save them too;
- it is in the undo history of this session (you edited it, or undid an apply), because undoing those steps would bring back
  the values from before the apply.

An asset whose `.uasset` changed while its JSON did not is never changed automatically, in either mode: see **asset changed**
below.

## JSON format

One file per data asset, at `<JsonDir>/<package path>.json`. For example, `/Game/Features/Light/DA_Lamp` maps to
`JsonBlueprintSync/Game/Features/Light/DA_Lamp.json`, and `/MyPlugin/DA_Lamp` to `JsonBlueprintSync/MyPlugin/DA_Lamp.json`.

```json
{
  "schema": 2,
  "asset": "/Game/Features/Light/DA_Lamp",
  "parent": "/Script/MyGame.LampData",
  "overrides": {
    "self": { "bStartOn": "True", "FlickerSpeed": "2.5", "Tint": "(R=1.000000,G=0.800000,B=0.600000,A=1.000000)" }
  }
}
```

- **Values are Unreal property text**: the same format as copy/paste in the Details panel, stored as JSON strings. Structs,
  enums, object references and soft paths round-trip exactly. A value that does not parse is reported as an error and nothing in
  that file is applied.
- **`overrides.self`** holds every value the asset sets, i.e. every value that differs from its class defaults. A missing
  `overrides` or `self` means that nothing is overridden. Any other field under `overrides` is an error.
- **The file is complete: a missing key means the class default.** Deleting a line and applying the file resets that property to
  the class's value.
- **A struct lists only the fields that differ from the class default**, as Unreal saves it, e.g. `"(A=5.000000)"`. A missing
  field takes the class default. Structs that Unreal saves as a unit (colours, vectors, rotators, quaternions) list every field.
  Arrays, sets and maps are replaced whole.
- **`parent`** is the class of the data asset. It is not applied. If it differs from the asset's class, the values in the file are
  relative to other defaults. The editor then lists the asset as **class differs** and applies nothing automatically, and the
  commandlet logs a warning.
- Output is deterministic: sorted keys, two-space indent, LF line endings, trailing newline, no timestamps. Applying a file and
  writing it again gives the same bytes.
- Files from version 1 of the format (`"schema": 1`, with a `reset` list), and files with the older `defaults` and `scope` fields
  instead of `overrides`, are not read. Re-export them: save the asset, or run the commandlet with `-export`.

Applying a value does not change other assets: data assets have no children.

## Syncing in the editor

`UJsonBlueprintSyncSubsystem` handles syncing while the editor runs:

| Event | What happens |
|---|---|
| Editor start (asset registry loaded) | Data assets whose JSON file or `.uasset` changed since the last sync are compared with their JSON. In Automatic mode, JSON changes without conflicts are applied and saved. The rest are listed, and a notification offers **Review** (opens the JSON Sync tab) or **Later**. Missing pairs are reported (see below). |
| Data asset saved | Its JSON file is written. **Exception:** if the JSON has changes that are not reviewed yet, the file is left alone and a notification says so. The asset stays in the JSON Sync tab until you apply it there. A JSON that is only out of date (the asset changed, the JSON did not) is written as usual. |
| Data asset renamed | The JSON file moves with it. |
| Data asset deleted | The JSON file is deleted. |

The console command `JsonBlueprintSync.Sync` reruns the start-up check, e.g. after a `git pull` with the editor open. Nothing is
checked during Play In Editor; a notification says so.

### Missing pairs

The start-up check (and `JsonBlueprintSync.Sync`) also looks for:

- **a JSON file without its data asset**: the asset does not exist, or is not a data asset (e.g. a file left from an older version
  of the plugin that synced Blueprints). Delete the file if the asset was deleted on purpose, or restore the asset.
- **a data asset without its JSON file**, e.g. one added while the plugin was off, or before it was installed. Save the asset, or
  run the commandlet with `-export` once. To leave a folder out on purpose, add it to **Excluded Folders**.

Each one is logged as a warning with its fix, and one notification gives the counts. Nothing is changed.

### Notifications

Problems are shown as notifications, not only in the Output Log (category `LogJsonBlueprintSync`):

- JSON files that could not be checked: unreadable, or the asset cannot be loaded. One notification per check.
- Missing pairs (see above).
- `Saved/JsonBlueprintSync/cache.json` cannot be read (at start-up) or written (once, until a write works again).
- A JSON file cannot be written, or cannot be deleted when its asset is renamed or deleted.
- An asset is renamed but its JSON cannot be read, so the file is not moved.
- An apply is undone (the asset is listed again).
- An asset is saved but its JSON is not written, because it has unreviewed changes or changed again after you reviewed it.

### The JSON Sync tab

Open it from **Window > JSON Sync**, with **Review** on a notification, or by clicking the JSON Sync entry in the status bar at the
bottom of the editor, just left of the unsaved assets. That entry reads **All JSON Synced**, or **N JSON to Review** (in yellow) while
assets are listed; its tooltip names them. The top list shows the assets whose JSON differs, with a status:

- **N keys**: the JSON changed keys. In Automatic mode these are applied at start-up and rarely stay listed.
- **N keys, conflicts**: some keys are conflicts (see below).
- **N keys, asset changed**: the asset changed and the JSON did not, e.g. a teammate committed the `.uasset` without its JSON, or it
  was saved while the plugin was off. The JSON is the source of truth, so this is never fixed automatically. Choose one:
  - **Write JSON** keeps the asset's values: it writes them to the JSON file. Saving the asset does the same.
  - **Apply JSON** reverts the asset to the JSON: every listed key gets its JSON value, as one undo step. The asset is not saved.
- **N keys, class differs**: the JSON names another class than the asset has. The JSON's values were relative to the other
  class's defaults. Usually **All from Asset**, then **Apply**, is right.
- **Cannot read the JSON**: the only action is **Write JSON**, which writes the asset's values to the file. Or fix the file and
  press **Refresh**.

Select an asset to see the keys that differ, as in a merge tool: the JSON value, the asset value, which side changed each key,
and which value the key will use (**Use**: JSON in blue, Asset in green, Choose in orange). The used value is highlighted; the other
one is dimmed. Selected rows are highlighted only while several are selected (for the right-click below).

| Changed in | Meaning | Uses by default |
|---|---|---|
| **JSON** | Only the file changed. | JSON |
| **Asset** | Only the asset changed (e.g. you edited it but have not saved yet). | Asset |
| **Conflict** (orange) | Both sides changed, or there is no record of what the asset was before, so the plugin cannot tell. | nothing: **Choose** |

`(default)` means the key is not set on that side, so it has the class default.

To choose the value a key uses:

- **click** its JSON value or its asset value, or
- select keys (Ctrl/Shift+click) and **right-click**: **Use JSON**, **Use Asset**, or **Reset** (back to the default), or
- use **All from JSON**, **All from Asset** or **Reset** below the list.

Your choices are kept while you switch between assets and when the tab refreshes. A choice is dropped if the key's JSON or asset
value changes meanwhile.

Once every conflict is chosen (the counter below the list shows how many are left), apply:

- **Apply** sets the keys that use JSON to their JSON values. The keys that use the asset keep its values. All the keys are one
  undo step. The asset is left modified and **not saved**: save it to keep the changes, which also writes its JSON with the
  chosen values. If nothing in the asset changes (every key uses the asset), the JSON is written at once.
- **Apply and Save** does the same and then saves the asset, which writes its JSON. If source control needs the file checked out,
  it asks first. If the save does not happen, a notification says so and the asset is left modified, as after Apply.
- After either one:
  - **Ctrl+Z** of the apply puts the asset back in the tab if it has not been saved yet, and saving it
    then does not write its JSON, so the JSON change is not lost. **Ctrl+Y** applies it again. After Apply and Save, Ctrl+Z is
    an ordinary edit: the asset is modified, and saving it writes its values to the JSON.
  - If the JSON file changes again before you save (e.g. a `git pull`), the save does not write the JSON. The asset is listed
    again, with the JSON you reviewed as the base: the file's new changes are JSON changes, and the values you kept are asset
    changes. A Refresh before the save does the same.
- **Show in Content Browser** selects the asset. Double-clicking an asset opens its editor.

### How the plugin tells which side changed

When a difference is found, the plugin works out the **base**: the values both sides had at the last sync. It keeps the base in
`Saved/JsonBlueprintSync/Base/` until the difference is resolved. A key is a JSON change if the asset still has the base value, an
asset change if the file has the base value, and a conflict otherwise.

- Only the JSON changed: the base is the asset as it is now, so every differing key is a JSON change.
- Only the `.uasset` changed: the base is the JSON file, so every differing key is an asset change (**asset changed**).

There is no base, so every differing key shows as **Conflict**, when:

- both the JSON and the `.uasset` changed since the last sync, or
- the asset had unsaved changes when the file changed, or
- the JSON changed and names another class than the asset has, or
- the plugin has never synced the asset before, or
- the asset was saved while its JSON had unreviewed changes (unless you had just applied it: see Apply above).

**Changes to the class.** A JSON file lists only what its asset overrides, so changing a class default (in C++ or in the data
asset's Blueprint class) leaves the JSON correct: the values the asset does not set follow the class. Its JSON goes out of date
without its `.uasset` changing only when:

- the class default now equals the value the asset overrides. The key is no longer an override, but the file lists it until the
  asset is saved. Applying it changes nothing.
- the class removes or renames a property that the asset overrides. The file names a key that no longer exists, so applying it
  reports an error and applies nothing in that file.

Nothing detects these, because the `.uasset` does not change. Saving the asset fixes its file; `-verify` reports them.

### Local state

Sync state is kept in `Saved/JsonBlueprintSync/`, which is local and should not be committed. `cache.json` stores, per asset, the
hash of the JSON and the package's saved hash as of the last sync, and whether the asset is waiting for review. An asset whose two
hashes both match is skipped without loading it; the package hash comes from the asset registry. Deleting the folder is safe: on the
next start every file is compared again, without a base.

## Settings

**Project Settings > Plugins > JSON Blueprint Sync**, stored in the project's `Config/DefaultEditor.ini` so the whole team shares
them:

```ini
[/Script/JsonBlueprintSync.JsonBlueprintSyncSettings]
; Folder for the JSON files, relative to the project folder.
JsonDir=JsonBlueprintSync
; Automatic or Manual.
Mode=Automatic
; Content folders whose data assets are not synced at all: no JSON is written, read or checked for them.
+ExcludedFolders=(Path="/Game/ThirdParty")
```

To turn the plugin off, disable it.

## Commandlet

For command-line use and CI:

```
UnrealEditor-Cmd.exe <Project>.uproject -run=JsonBlueprintSync -export|-import|-verify [-dir=/Game/...] [-jsondir=<path>] [-nosave]
```

- `-export` writes JSON for every synced data asset under `-dir` (default `/Game`), and warns about JSON files without their asset.
- `-import` applies every JSON file under `-dir` and saves the data assets it changed, whatever the mode. `-nosave` skips saving.
- `-verify` exits with an error if any JSON file differs from its data asset, is missing, or has no asset. Use it in CI to catch
  assets that were changed without their JSON.
- `-jsondir` overrides the `JsonDir` setting.
- Data assets in excluded folders are skipped.

In Git Bash, set `MSYS_NO_PATHCONV=1` so `/Game/...` arguments are not turned into Windows paths.

## Git diff and merge for assets

The JSON files give a readable diff of the values a change sets. For the `.uasset` files themselves:

- **Diff**: the git and TortoiseGit diff of assets is part of **UAssReader**, a standalone program that reads asset files without
  the editor or the project's code and writes their stored values as merge text, one line per value. Its repository (private, as
  it is engine code) has the program, the scripts and the setup in its README.
- **Merge**: merging two versions of an asset through that text, and applying the result in the editor, is the separate editor
  plugin **UAssPainMerger** (https://github.com/yeahBash/UAssPainMerger).

This plugin doesn't need either of them. For a data asset, merging its JSON file is usually enough: the editor applies the merged
JSON.

## Limitations

- Only data assets are synced, and only a package's main asset.
- Only properties visible in the Details panel (`CPF_Edit`) are stored. Transient, deprecated, read-only (`EditConst`) and
  class-only (`EditDefaultsOnly`) properties are skipped, as are fixed-size C-style arrays.
- **Instanced sub-objects** (properties marked `Instanced` and their contents) are skipped: they stay in the `.uasset` only. Their
  changes are not in the JSON, and a JSON apply leaves them as they are.

## Tests

Automation tests are under `JsonBlueprintSync.*`:

```
UnrealEditor-Cmd.exe <Project>.uproject -ExecCmds="Automation RunTests JsonBlueprintSync" -TestExit="Automation Test Queue Empty" -unattended -nullrhi -nosplash
```

They cover the library and the subsystem's decision rules (which base to use, when Automatic mode applies on its own, missing
pairs), not the editor subsystem itself, the JSON Sync tab or the commandlet. [Docs/ManualTests.md](Docs/ManualTests.md)
is a step-by-step checklist for those.

## How it works

[Docs/Internals.md](Docs/Internals.md) describes each part of the plugin, how the parts work together, and the caveats and corner
cases.

## License

MIT. See [LICENSE](LICENSE).
