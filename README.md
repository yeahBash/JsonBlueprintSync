# JsonBlueprintSync

An Unreal Engine editor plugin that keeps the values set in **Blueprints, Widget Blueprints and data assets** in JSON files next
to the project:

- When you save an asset in the editor, its JSON file is written.
- When the JSON files change on disk (e.g. after a `git pull`), the editor lists the assets whose JSON differs and lets you apply
  the changes key by key. For data assets this can happen automatically.

`.uasset` files are binary, so this gives you diffs, code review and merges for default values. Only values that differ from the
archetype are stored (the parent class for a Blueprint, the class for a data asset), so a file holds just what the asset itself sets.

Graph logic (nodes and pins) is out of scope.

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

4. Commit the JSON folder (`<Project>/JsonBlueprintSync/` by default) alongside the assets.

### Prebuilt copy for Blueprint-only projects

```
<Engine>\Engine\Build\BatchFiles\RunUAT.bat BuildPlugin -Plugin="<path>\JsonBlueprintSync.uplugin" -Package="<output folder>" -TargetPlatforms=Win64
```

Copy the output folder to the project's `Plugins/JsonBlueprintSync/`. Binaries only work with the engine version they were built with.

## Modes

Each asset type has a mode, set in **Project Settings > Plugins > JSON Blueprint Sync**:

| Mode | Saving the asset | JSON changed on disk |
|---|---|---|
| **None** | Nothing. | Ignored. |
| **Write JSON Only** | Writes the JSON. | Listed in the JSON Sync tab. Applied only when you choose the values and click **Apply**. |
| **Two Way** | Writes the JSON. | Applied automatically when the editor starts, unless a key conflicts or the parent differs. Those are listed in the JSON Sync tab. The automatic apply saves the asset and cannot be undone with Ctrl+Z. |

| Asset type | Default | Two Way allowed |
|---|---|---|
| Blueprints | Write JSON Only | No |
| Widget Blueprints | Write JSON Only | No |
| Data assets | Two Way | Yes |

For Blueprints the `.uasset` stays the source of truth: the JSON is for reviewing and merging, and changes come back only when
someone chooses to apply them. Data assets are plain data, so a JSON edit can be trusted to apply on its own.

For Two Way the JSON is the source of truth. The automatic apply saves the asset at once (checking it out first if source control
needs it), so the `.uasset` on disk, which is what the game uses, matches the JSON. It is not an undo step: Ctrl+Z cannot quietly
bring back the old values and have the next save write them over a teammate's change. To back out a JSON change, edit the values
and save (which writes the JSON), or revert the JSON file in source control. If the save does not happen (e.g. you decline the
checkout), a warning says so and the asset is left modified: save it yourself.

Two checks keep this safe. They matter only for a Refresh or `JsonBlueprintSync.Sync` later in the session; at editor start
neither applies. An asset is not applied automatically, but listed in the JSON Sync tab with a log line, if:

- it has unsaved changes, because the automatic save would save them too;
- it is in the undo history of this session (you edited it, or undid an apply), because undoing those steps would bring back
  the values from before the apply.

## JSON format

One file per asset under `/Game`, at `<JsonDir>/<package path>.json`. For example, `/Game/Features/Light/BP_Lamp` maps to
`JsonBlueprintSync/Game/Features/Light/BP_Lamp.json`.

```json
{
  "schema": 2,
  "asset": "/Game/Features/Light/BP_Lamp",
  "parent": "/Script/Engine.Actor",
  "overrides": {
    "self": { "bStartOn": "True", "FlickerSpeed": "2.5" },
    "components": {
      "PointLight": { "Intensity": "5000.0", "LightColor": "(R=255,G=200,B=150,A=255)" }
    }
  }
}
```

- **Values are Unreal property text**: the same format as copy/paste in the Details panel, stored as JSON strings. Structs,
  enums, object references and soft paths round-trip exactly. A value that does not parse is reported as an error and nothing in
  that file is applied.
- **`overrides`** holds every value the asset sets, i.e. every value that differs from its archetype:
  - **`self`**: the Blueprint's class default object, or a data asset's own properties.
  - **`components`** (Blueprints): component templates, keyed by component name. This covers components added in this
    Blueprint, inherited from a parent Blueprint, or created in C++.
  - **`widgets`** (Widget Blueprints, see below): the Designer's widgets.
  - A missing `overrides`, `self`, `components` or `widgets` means that nothing in it is overridden.
- **The file is complete: a missing key means the archetype's value.** Deleting a line and applying the file resets that property
  to the parent's (or the class's) value.
- **Arrays and structs are replaced whole.**
- **`parent`** is the parent class of a Blueprint, or the class of a data asset. It is not applied: reparenting is done in the
  editor. If it differs from the asset's (e.g. the Blueprint was reparented), the values in the file are relative to another
  archetype. The editor then lists the asset as **parent differs** and applies nothing automatically, and the commandlet logs a
  warning.
- Output is deterministic: sorted keys, two-space indent, LF line endings, trailing newline, no timestamps. Applying a file and
  writing it again gives the same bytes.
- Files from version 1 of the format (`"schema": 1`, with a `reset` list), and files with the older `defaults` and `scope` fields
  instead of `overrides`, are not read. Re-export them: save the asset, or run the commandlet with `-export`.

Applying a value also updates child Blueprints, data assets of a Blueprint class, and placed instances that inherited the old value,
the same way the Details panel does. Instances that override the value keep their override.

### Widget Blueprints

For a Widget Blueprint, the values set in the Designer are stored in `overrides.widgets`, keyed by widget name. Each widget is
diffed against the defaults of its widget class. The layout of a widget in its parent panel (anchors, offsets, padding, alignment,
Z order and so on) is its slot, stored under `"<Widget>.Slot"`:

```json
    "widgets": {
      "Title": { "RenderOpacity": "0.500000", "Text": "NSLOCTEXT(\"...\", \"...\", \"Hello\")" },
      "Title.Slot": { "ZOrder": "3" }
    }
```

- Keys in the JSON Sync tab are `Title.RenderOpacity` for a widget and `Title.Slot.ZOrder` for its slot.
- Text values keep their localization key, in Unreal's text format.
- The section is written only when a Blueprint has widgets with values set, so other Blueprints' files do not have it.
- Values set on the Widget Blueprint's own class defaults (Class Defaults in the graph editor) still go in `self`.
- Only existing widgets are updated. Adding, removing, renaming or re-parenting widgets is not synced: a file that names a widget
  the Blueprint does not have is an error, and nothing in it is applied.

### Data assets

Any asset whose class derives from `UDataAsset` (including `UPrimaryDataAsset` and Blueprint subclasses of either) is synced. Its
properties are diffed against its class's defaults. Only properties editable on the asset are stored.

## Syncing in the editor

`UJsonBlueprintSyncSubsystem` handles syncing while the editor runs:

| Event | What happens |
|---|---|
| Editor start (asset registry loaded) | Assets whose JSON file or `.uasset` changed since the last sync are compared with their JSON. Two Way assets whose JSON changed keys without conflicts are applied. The rest are listed, and a notification offers **Review** (opens the JSON Sync tab) or **Later**. |
| Asset saved | Its JSON file is written. **Exception:** if the JSON has changes that are not reviewed yet, the file is left alone and a notification says so. The asset stays in the JSON Sync tab until you apply it there. A JSON that is only out of date (it has no changes of its own) is written as usual. |
| Asset renamed | The JSON file moves with it. |
| Asset deleted | The JSON file is deleted. |

The console command `JsonBlueprintSync.Sync` reruns the start-up check, e.g. after a `git pull` with the editor open. Nothing is
checked during Play In Editor; a notification says so.

Problems are shown as notifications, not only in the Output Log (category `LogJsonBlueprintSync`):

- JSON files that could not be checked: unreadable, no asset for them, or the asset cannot be loaded. One notification per check.
- `Saved/JsonBlueprintSync/cache.json` cannot be read (at start-up) or written (once, until a write works again).
- A JSON file cannot be written, or cannot be deleted when its asset is renamed or deleted.
- An asset is renamed but its JSON cannot be read, so the file is not moved.
- An apply is undone (the asset is listed again).
- An asset is saved but its JSON is not written, because it has unreviewed changes or changed again after you reviewed it.

### The JSON Sync tab

Open it from **Window > JSON Sync**, with **Review** on a notification, or by clicking the JSON Sync entry in the status bar at the
bottom of the editor, next to the unsaved assets. That entry reads **JSON Synced**, or **N JSON to Review** (in yellow) while
assets are listed; its tooltip names them. The top list shows the assets whose JSON differs, with a
status:

- **N keys**: the JSON changed keys. Two Way assets in this state are applied at start-up and rarely stay listed.
- **N keys, conflicts**: some keys are conflicts (see below).
- **N keys, JSON out of date**: the asset changed and the JSON did not, e.g. a teammate committed the `.uasset` without its JSON.
  Save the asset, or press **Apply**: every key keeps the asset's value by default.
- **N keys, parent differs**: the JSON names another parent (or data asset class) than the asset has, e.g. after a reparent. The
  JSON's values were relative to the old parent. Usually **All from Asset**, then **Apply**, is right.
- **Cannot read the JSON**: the only action is **Overwrite JSON**, which writes the asset's values to the file. Or fix the file and
  press **Refresh**.

Select an asset to see the keys that differ, as in a merge tool: the JSON value, the asset value, which side changed each key,
and which value the key will use (**Use**). The used value is highlighted; the other one is dimmed.

| Changed in | Meaning | Uses by default |
|---|---|---|
| **JSON** | Only the file changed. | JSON |
| **Asset** | Only the asset changed (e.g. you edited it but have not saved yet, or its `.uasset` was committed without the JSON). | Asset |
| **Conflict** (orange) | Both sides changed, or there is no record of what the asset was before, so the plugin cannot tell. | nothing: **Choose** |

`(default)` means the key is not set on that side, so it has the archetype's value.

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
- Only the `.uasset` changed: the base is the JSON file, so every differing key is an asset change (**JSON out of date**).

There is no base, so every differing key shows as **Conflict**, when:

- both the JSON and the `.uasset` changed since the last sync, or
- the asset had unsaved changes when the file changed, or
- the JSON changed and names another parent than the asset has, or
- the plugin has never synced the asset before, or
- the asset was saved while its JSON had unreviewed changes (unless you had just applied it: see Apply above).

A Blueprint is also changed by edits to its parent Blueprint or its C++ class, without its own `.uasset` changing. Such a change is
not detected until the Blueprint is saved; `-verify` catches it.

### Local state

Sync state is kept in `Saved/JsonBlueprintSync/`, which is local and should not be committed. `cache.json` stores, per asset, the
hash of the JSON and the package's saved hash as of the last sync, and whether the asset is waiting for review. An asset whose two
hashes both match is skipped without loading it; the package hash comes from the asset registry. Deleting the folder is safe: on the
next start every file is compared again, without a base.

Applying a file in the editor does not compile the Blueprint, because compiling would break undo. The values survive the next
compile, just like Details-panel edits. For Widget Blueprints, the Designer shows the new values straight away. A running game
uses the compiled widget tree, which Play In Editor brings up to date by compiling modified Blueprints first.

## Settings

**Project Settings > Plugins > JSON Blueprint Sync**, stored in the project's `Config/DefaultEditor.ini` so the whole team shares
them:

```ini
[/Script/JsonBlueprintSync.JsonBlueprintSyncSettings]
; Folder for the JSON files, relative to the project folder.
JsonDir=JsonBlueprintSync
; None, WriteJsonOnly or TwoWay. TwoWay is treated as WriteJsonOnly for Blueprints and Widget Blueprints.
Blueprints=WriteJsonOnly
WidgetBlueprints=WriteJsonOnly
DataAssets=TwoWay
```

Set every mode to None to turn the plugin off without disabling it.

## Commandlet

For command-line use and CI:

```
UnrealEditor-Cmd.exe <Project>.uproject -run=JsonBlueprintSync -export|-import|-verify [-dir=/Game/...] [-jsondir=<path>] [-nosave]
```

- `-export` writes JSON for every synced asset under `-dir` (default `/Game`).
- `-import` applies every JSON file under `-dir`, compiles the changed Blueprints, and saves them plus any child Blueprints and data
  assets the values reached. It applies the whole file, whatever the mode, except for assets whose mode is None. `-nosave` skips
  saving.
- `-verify` exits with an error if any JSON file differs from its asset, is missing, or has no asset. Use it in CI to catch assets
  that were changed without re-exporting.
- `-jsondir` overrides the `JsonDir` setting.
- Assets whose mode is None are skipped.

In Git Bash, set `MSYS_NO_PATHCONV=1` so `/Game/...` arguments are not turned into Windows paths.

## Limitations

- Only default values are stored. Graph logic, functions and variables themselves are not. For Widget Blueprints, the widget
  hierarchy, animations and property bindings are not stored either.
- Only assets under `/Game` are synced, not plugin content. Only a package's main asset is synced.
- Only properties visible in the Details panel (`CPF_Edit`) are stored. Transient, deprecated, read-only (`EditConst`) and
  instance-only properties are skipped, as are fixed-size C-style arrays. For data assets, properties that can only be edited on
  the class defaults are skipped.
- Instanced sub-objects (properties marked `Instanced` and their contents) are skipped.
- Components created in C++ with `CreateDefaultSubobject` are captured but have not been tested yet.

## Tests

Automation tests are under `JsonBlueprintSync.*`:

```
UnrealEditor-Cmd.exe <Project>.uproject -ExecCmds="Automation RunTests JsonBlueprintSync;Quit" -unattended -nullrhi -nosplash
```

They cover the library and the subsystem's decision rules (which base to use, when Two Way applies on its own), not the editor
subsystem itself, the JSON Sync tab or the commandlet. [Docs/ManualTests.md](Docs/ManualTests.md)
is a step-by-step checklist for those.

## How it works

[Docs/Internals.md](Docs/Internals.md) describes each part of the plugin, how the parts work together, and the caveats and corner
cases.

## License

MIT. See [LICENSE](LICENSE).
