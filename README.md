# JsonBlueprintSync

An Unreal Engine editor plugin that keeps each Blueprint's **class-default overrides** in a JSON file and syncs them both ways:

- When you save a Blueprint in the editor, its JSON file is written.
- When the editor starts, JSON files that changed on disk (e.g. after a `git pull`) are applied back to the Blueprints.

`.uasset` files are binary, so this gives you diffs, code review and merges for Blueprint default values. Only values that
differ from the parent class are stored, so a file holds just what the Blueprint itself sets.

Graph logic (nodes and pins) is out of scope. Every file says `"scope": "defaults"` to make that clear.

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

## JSON format

One file per Blueprint under `/Game`, at `<JsonDir>/<package path>.json`. For example, `/Game/Features/Light/BP_Lamp` maps to
`JsonBlueprintSync/Game/Features/Light/BP_Lamp.json`.

```json
{
  "schema": 1,
  "scope": "defaults",
  "blueprint": "/Game/Features/Light/BP_Lamp",
  "parent": "/Script/Engine.Actor",
  "defaults": { "bStartOn": "True", "FlickerSpeed": "2.5" },
  "components": {
    "PointLight": { "Intensity": "5000.0", "LightColor": "(R=255,G=200,B=150,A=255)" }
  },
  "reset": []
}
```

- **Values are Unreal property text**: the same format as copy/paste in the Details panel, stored as JSON strings. Structs,
  enums, object references and soft paths round-trip exactly. A value that does not parse is reported as an error and nothing in
  that file is applied.
- **`defaults`** holds the class default object's properties. **`components`** holds component templates, keyed by component
  name. This covers components added in this Blueprint, inherited from a parent Blueprint, or created in C++.
- **A missing key means "leave it alone"**, so deleting a line from the file does not reset the value. To reset a property to the
  parent's value, list it in `reset` (`"FlickerSpeed"`, or `"PointLight.Intensity"` for a component). The list is kept on export.
- **Arrays and structs are replaced whole.**
- **`parent`** is informational: a warning is logged if the Blueprint's actual parent class differs.
- Output is deterministic: sorted keys, two-space indent, LF line endings, trailing newline, no timestamps. Importing a file and
  exporting again gives the same bytes.

Applying a value also updates child Blueprints and placed instances that inherited the old value, the same way the Details panel
does. Instances that override the value keep their override.

## Syncing in the editor

`UJsonBlueprintSyncSubsystem` handles syncing while the editor runs:

| Event | What happens |
|---|---|
| Blueprint saved | Its JSON file is written: the Blueprint wins. If the file had been changed outside the editor since the last sync, a notification lists the keys that were overwritten. |
| Editor start (asset registry loaded) | JSON files that changed since the last sync are applied: the JSON wins. Applied Blueprints are left modified and **never saved automatically**. All changes are one undo step. |
| Both the file and the Blueprint changed | A notification lists the differing keys, with **Use JSON** and **Keep Blueprint** buttons. Saving the Blueprint also resolves it. |
| Blueprint renamed | The JSON file moves with it. |
| Blueprint deleted | The JSON file is deleted. |

The console command `JsonBlueprintSync.Sync` reruns the start-up check, e.g. after a `git pull` with the editor open. Nothing
happens during Play In Editor.

Sync state is kept in `Saved/JsonBlueprintSync/cache.json`, which is local and should not be committed. It stores two hashes per
Blueprint as of its last save. Files whose hash matches are skipped without loading the Blueprint.

Applying a file in the editor does not compile the Blueprint, because compiling would break undo. The values survive the next
compile, just like Details-panel edits.

## Settings

In the project's `Config/DefaultEditor.ini`:

```ini
[JsonBlueprintSync]
; Folder for the JSON files, relative to the project folder. Default: JsonBlueprintSync
JsonDir=JsonBlueprintSync
; Set to False to turn off the automatic sync in the editor (the commandlet still works).
bAutoSync=True
```

## Commandlet

For command-line use and CI:

```
UnrealEditor-Cmd.exe <Project>.uproject -run=JsonBlueprintSync -export|-import|-verify [-dir=/Game/...] [-jsondir=<path>] [-nosave]
```

- `-export` writes JSON for every Blueprint under `-dir` (default `/Game`).
- `-import` applies every JSON file under `-dir`, compiles the changed Blueprints, and saves them plus any child Blueprints the
  values reached. `-nosave` skips saving.
- `-verify` exits with an error if any JSON file differs from its Blueprint. Use it in CI to catch Blueprints that were changed
  without re-exporting.
- `-jsondir` overrides the `JsonDir` setting.

In Git Bash, set `MSYS_NO_PATHCONV=1` so `/Game/...` arguments are not turned into Windows paths.

## Limitations

- Only class defaults are stored. Graph logic, functions and variables themselves are not.
- Only Blueprints under `/Game` are synced, not plugin content.
- Only properties visible in the Details panel (`CPF_Edit`) are stored. Transient, deprecated, read-only (`EditConst`) and
  instance-only properties are skipped, as are fixed-size C-style arrays.
- Instanced sub-objects (properties marked `Instanced` and their contents) are skipped.
- Components created in C++ with `CreateDefaultSubobject` are captured but have not been tested yet.

## Tests

Automation tests are under `JsonBlueprintSync.*`:

```
UnrealEditor-Cmd.exe <Project>.uproject -ExecCmds="Automation RunTests JsonBlueprintSync;Quit" -unattended -nullrhi -nosplash
```

## License

MIT. See [LICENSE](LICENSE).
