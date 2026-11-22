# JsonBlueprintSync manual tests

A checklist for checking the plugin by hand in the editor and on the command line. The automated tests cover the library (capture,
apply, format, merge, cache) and the rules for choosing a base, but not the editor subsystem, the JSON Sync tab or the commandlet. Those are covered here. For how
things work and why, see [Internals.md](Internals.md).

Each test has steps and the expected result. Tests marked **(known issue)** describe current behaviour that is a known weak spot.
They check that it behaves as documented, not that it is ideal.

- [0. Automated tests](#0-automated-tests)
- [1. Setup](#1-setup)
- [2. Writing JSON on save](#2-writing-json-on-save)
- [3. JSON changes on a Blueprint (Write JSON Only)](#3-json-changes-on-a-blueprint-write-json-only)
- [4. Bad files](#4-bad-files)
- [5. Data assets (Two Way)](#5-data-assets-two-way)
- [6. No base](#6-no-base)
- [7. Propagation and inherited components](#7-propagation-and-inherited-components)
- [8. Widget Blueprints](#8-widget-blueprints)
- [9. Rename, move, delete](#9-rename-move-delete)
- [10. Settings](#10-settings)
- [11. PIE and line endings](#11-pie-and-line-endings)
- [12. Commandlet](#12-commandlet)
- [13. `.uasset` changes and reparenting](#13-uasset-changes-and-reparenting)
- [14. Notifications for problems](#14-notifications-for-problems)
- [15. Cleanup](#15-cleanup)

## 0. Automated tests

Close the editor, or run them from a second shell while it is open (it is slower). From the project folder, in PowerShell:

```powershell
& "<Engine>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$PWD\<Project>.uproject" `
  -ExecCmds="Automation RunTests JsonBlueprintSync;Quit" -unattended -nullrhi -nosplash
```

Expected: 14 tests, each logging `Result={Success}`, and no `Result={Fail}`. The full log is in `Saved/Logs/<Project>.log`.
To filter it: `Select-String -Path Saved\Logs\<Project>.log -Pattern "Result=\{"`.

In the editor you can also run them from **Tools > Test Automation**: filter by `JsonBlueprintSync`, tick the group and press
Start Tests.

Some `Condition failed` lines may appear at startup. They come from the engine's own tests, not the plugin.

## 1. Setup

### Useful places

| What | Where |
|---|---|
| JSON files | `<Project>/JsonBlueprintSync/Game/...` (setting `JsonDir`) |
| Local cache | `<Project>/Saved/JsonBlueprintSync/cache.json` |
| Base snapshots | `<Project>/Saved/JsonBlueprintSync/Base/Game/...` |
| Log | Output Log, filtered by the category `LogJsonBlueprintSync` |
| Tab | **Window > JSON Sync** |
| Rerun the check | **Refresh** in the tab, or the console command `JsonBlueprintSync.Sync` |
| Settings | **Project Settings > Plugins > JSON Blueprint Sync** |

Keep a text editor open on the JSON folder. Unless a test says otherwise, use the default settings (Blueprints and Widget Blueprints
Write JSON Only, data assets Two Way, `JsonDir` = `JsonBlueprintSync`).

### Test assets

Create these in a sandbox folder, e.g. `/Game/Developers/<you>/JsonSyncTest/` (written `.../` below). Compile and save each one.

| Asset | How to make it |
|---|---|
| `BP_SyncLamp` | Blueprint Class, parent **Actor**. Variables: `FlickerSpeed` (Float, default 1.0), `bStartOn` (Boolean), `Tint` (Linear Color). Add a **Point Light** component and name it `Light`. |
| `BP_SyncLampChild` | Right-click `BP_SyncLamp` > Create Child Blueprint Class. Change nothing yet. |
| `WBP_SyncTest` | Widget Blueprint (User Widget). Add a **Canvas Panel**, and in it a **Text** widget named `Title`. |
| `PDA_SyncItem` | Blueprint Class, parent **PrimaryDataAsset**. Variables: `Damage` (Float) and `Label` (String), both **Instance Editable** (eye icon open). Add a third, `Secret` (Float), and leave it **not** Instance Editable. |
| `DA_SyncItem` | Miscellaneous > Data Asset, class `PDA_SyncItem`. |
| `IA_SyncTest` | Input > Input Action. A native C++ data asset (needs the Enhanced Input plugin; any other native `UDataAsset` type also works). |
| `L_SyncTest` | An empty level, for the propagation test. |

After saving them, `cache.json` should have an entry for each synced asset (not `L_SyncTest`), each with a `json` and an `asset`
hash.

## 2. Writing JSON on save

**2.1 Blueprint defaults and a component**

1. In `BP_SyncLamp`, set `FlickerSpeed` = 2.5 and `bStartOn` = true. Select `Light` and set Intensity = 8000.
2. Compile and save.

Expected `.../BP_SyncLamp.json`:

```json
{
  "schema": 2,
  "asset": "/Game/Developers/<you>/JsonSyncTest/BP_SyncLamp",
  "parent": "/Script/Engine.Actor",
  "overrides": {
    "self": {
      "FlickerSpeed": "2.500000",
      "bStartOn": "True"
    },
    "components": {
      "Light": {
        "Intensity": "8000.000000"
      }
    }
  }
}
```

- Only changed values are present: no `Tint`, no other Light properties.
- Keys are sorted case-sensitively, so `FlickerSpeed` comes before `bStartOn`.
- The file uses LF line endings, two-space indent, a trailing newline and no BOM.
- `cache.json` has the asset's hash and no `pending`.

**2.2 Saving without changes does not rewrite the file**

1. Note the file's modified time. Make a change and undo it (the asset becomes dirty), then save.

Expected: the file's modified time does not change, and `git status` shows nothing for it.

**2.3 Resetting a value removes its key**

1. In `BP_SyncLamp` Class Defaults, click the reset arrow next to `bStartOn`. Compile and save.

Expected: `bStartOn` is gone from `self`.

**2.4 Child Blueprint stores only its own changes**

1. In `BP_SyncLampChild`, set `FlickerSpeed` = 3. Select the inherited `Light` and set Attenuation Radius = 500. Compile and save.

Expected `.../BP_SyncLampChild.json`: `parent` is `/Game/.../BP_SyncLamp.BP_SyncLamp_C`, `self` has only `FlickerSpeed`, and
`components.Light` has only `AttenuationRadius`, not the parent's Intensity.

**2.5 Data assets**

1. In `DA_SyncItem`, set `Damage` = 10 and `Label` = `Sword`. Save.
2. In `PDA_SyncItem` Class Defaults, set `Secret` = 5. Compile and save.

Expected:

- `DA_SyncItem.json`: `parent` is `/Game/.../PDA_SyncItem.PDA_SyncItem_C`, with `Damage` and `Label` in `self`. There is no
  `components` section.
- `PDA_SyncItem.json` is a normal Blueprint file with `Secret` in `self`.
- `Secret` never appears in `DA_SyncItem.json`, because it is not editable on the asset.

**2.6 Native data asset**

1. In `IA_SyncTest`, set Value Type = Axis1D (float) and tick Consume Input off, or change any other property. Save.

Expected: `IA_SyncTest.json` with `parent` `/Script/EnhancedInput.InputAction` and the changed values in `self`.

## 3. JSON changes on a Blueprint (Write JSON Only)

Start each test from a clean state: nothing in the JSON Sync tab and `BP_SyncLamp` saved.

**3.1 JSON edited while the editor is closed**

1. Close the editor. In `BP_SyncLamp.json`, change `FlickerSpeed` to `"4.0"`. Start the editor.

Expected:

- The toast "JSON Sync: 1 asset(s) differ from their JSON" appears, with **Review** and **Later**.
- **Review** opens the tab. `BP_SyncLamp` is listed with Type Blueprint and Status "1 keys".
- The key `FlickerSpeed` has JSON value `4.0`, Asset value `2.500000`, Changed in **JSON**, and Use **JSON**. The JSON value
  has a highlighted background; the asset value is dimmed. No conflict counter is shown, and **Apply** and **Apply and Save** are
  enabled.
- `cache.json` has `"pending": true` for the asset, and `Saved/JsonBlueprintSync/Base/.../BP_SyncLamp.json` exists.

2. Press **Apply**.

Expected:

- Toast "Applied JSON to BP_SyncLamp. Not saved; Ctrl+Z reverts."
- The asset leaves the tab and the summary reads "Every synced asset matches its JSON file."
- The Blueprint is dirty (asterisk) and `FlickerSpeed` is 4.0 in Class Defaults. The JSON file is not touched yet.
- The base file is deleted, and `pending` is gone from the cache.

3. Save the Blueprint.

Expected: the file now reads `"FlickerSpeed": "4.000000"` (canonical form), and the cache hash is updated.

**3.2 Closing without saving after Apply**

1. Repeat 3.1 up to **Apply** (change the value to `"5.0"`), then close the editor and choose **Don't Save**. Start it again.

Expected: the asset is listed again with the same key. The change is not lost.

**3.3 Undo after Apply**

1. Repeat 3.1 up to **Apply** (value `"6.0"`). Press Ctrl+Z.

Expected:

- `FlickerSpeed` goes back to its old value.
- A warning toast "JSON Sync: undo reverted the JSON applied to BP_SyncLamp. It is back in the JSON Sync tab…".
- The asset is listed again with `FlickerSpeed` Changed in **JSON**, Use **JSON**, as before the apply. The base file is back and the cache
  has `"pending": true`.

2. Press Ctrl+Y.

Expected: `FlickerSpeed` is 6.0 again and the asset leaves the tab (no toast).

3. Press Ctrl+Z again, then save the Blueprint.

Expected: the "Saved BP_SyncLamp, but did not write its JSON: the JSON has changes you have not reviewed" toast. The JSON still
reads `"6.0"`. Press **Apply and Save** to finish.

**3.4 JSON edited while the editor is open**

1. With the editor open, change `FlickerSpeed` in the JSON to `"7.0"`. Wait: nothing happens (there is no file watching).
2. Press **Refresh** in the tab, or run `JsonBlueprintSync.Sync`.

Expected: listed as in 3.1. Each further Refresh shows the toast again (known issue).

**3.5 Asset changed after the JSON change was detected**

1. Continue from 3.4 (pending, not applied). In the Blueprint, set Light Intensity = 9000 (do not save).
2. Press **Refresh** (or select another asset and back).

Expected:

- Two keys: `FlickerSpeed` (Changed in JSON, Use JSON) and `Light.Intensity` (JSON `8000.000000`, Asset `9000.000000`, Changed
  in **Asset**, Use Asset).
- Before the Refresh, the tab still showed one key. The tab does not follow asset edits live.

3. Press **Apply**, then save.

Expected: `FlickerSpeed` takes the JSON value and Intensity stays at 9000. The saved JSON has both `7.000000` and `9000.000000`.

**3.6 Conflict**

1. Change `FlickerSpeed` in the JSON to `"8.0"` and press Refresh (asset saved, so it gets a base).
2. In the Blueprint, set `FlickerSpeed` = 9 (do not save). Press Refresh.

Expected:

- `FlickerSpeed` has JSON `8.0`, Asset `9.000000`, Changed in **Conflict** (orange), and Use **Choose** (orange). Neither value is
  highlighted. Status is "1 keys, conflicts".
- Below the list: "1 conflict to choose". **Apply** and **Apply and Save** are disabled.

3. Click the asset value `9.000000`.

Expected: it is highlighted, the JSON value is dimmed, Use shows **Asset**, the counter disappears and Apply is enabled.

4. Click the JSON value `8.0`, then press **Apply**.

Expected: Use switched to **JSON** before the apply. The value becomes 8.0.

**3.7 Removing a key in the JSON resets the value**

1. Delete the `FlickerSpeed` line from the JSON (mind the commas). Refresh.

Expected: the JSON value is `(default)` in grey and the key is Changed in JSON. After Apply, `FlickerSpeed` shows the variable's
default (1.0), with no reset arrow.

**3.8 Keeping every asset value**

1. Change the JSON to `"FlickerSpeed": "11.0"`, Refresh, select the asset and press **All from Asset**, then **Apply**.

Expected: the file is rewritten with the asset's values at once, the asset leaves the tab, and the asset is **not** dirty.
**Overwrite JSON** is not shown (it is only for an unreadable file, see 4.6).

**3.9 Apply and Save with nothing to apply**

1. As 3.8, but press **All from Asset**, then **Apply and Save**.

Expected: the toast "Wrote the JSON of BP_SyncLamp; the asset did not change." The file is rewritten, and the `.uasset` is **not**
saved (its timestamp does not change).

**3.10 Saving a pending asset**

1. Change the JSON, Refresh, leave the asset listed. Change something in the Blueprint and save it.

Expected:

- The asset is saved, but the JSON file is unchanged.
- The toast "Saved BP_SyncLamp, but did not write its JSON: the JSON has changes you have not reviewed" appears, with **Review**.
- The asset stays in the tab.

**3.11 Save guard**

1. Change the JSON (e.g. `FlickerSpeed` `"12.0"`) and do **not** Refresh.
2. Change something else in the Blueprint and save it.

Expected:

- The save-skipped toast appears and the JSON file still has your edit.
- The asset is listed with **every** differing key as Conflict, all **Choose**, and the counter shows how many.
- The detail line says "every key is a conflict".

**3.12 Save guard when values match**

1. Close the editor. In the JSON, rewrite `"FlickerSpeed": "4.000000"` as `"FlickerSpeed": "4"` (same value). Start the editor.

Expected: nothing is listed (the values are the same), and the cache records the new hash. The next save of the asset rewrites the
file in canonical form.

**3.13 Unknown key, kept from the asset**

1. Add `"Nope": "1"` to `overrides.self` in the JSON. Refresh.

Expected: `Nope` is listed (JSON `1`, Asset `(default)`).

2. Click its asset value `(default)` (Use: Asset) and press **Apply**.

Expected: the asset is applied. Since nothing changed, the JSON is rewritten at once without `Nope`.

**3.14 JSON changes between Apply and Save**

1. Change `FlickerSpeed` in the JSON to `"10.0"` and `Light.Intensity` to `"8100.0"`. Refresh.
2. Click the asset value of `Light.Intensity` (Use: Asset) and press **Apply**. Do not save.
3. Change `Tint` in the JSON (e.g. `"(R=0.0,G=1.0,B=0.0,A=1.0)"`). Do **not** Refresh. Save the Blueprint.

Expected:

- The toast "Saved BP_SyncLamp, but did not write its JSON: the JSON changed again after you reviewed it", with Review.
- The JSON still has your `Tint` edit.
- The asset is listed with `Tint` Changed in **JSON** (Use JSON) and `Light.Intensity` Changed in **Asset** (Use Asset: the value
  you kept). `FlickerSpeed` is not listed.

4. Press **Apply and Save**.

Expected: the JSON has the new `Tint`, `FlickerSpeed` `10.000000`, and the old `Light.Intensity`.

**3.15 Refresh after Apply**

1. Change `FlickerSpeed` in the JSON to `"11.0"` and `Light.Intensity` to `"8200.0"`. Refresh. Click the asset value of `Light.Intensity`, then **Apply**.
   Do not save.
2. Press **Refresh**.

Expected: the asset is not listed again (no every-key Conflict list).

3. Change `Tint` in the JSON and press **Refresh**.

Expected: listed as in 3.14 step 3, with the normal pending toast. Saving now does not write the JSON.

**3.16 Choosing several keys at once**

1. Change `FlickerSpeed`, `Light.Intensity` and `Tint` in the JSON. In the Blueprint, change all three to other values (do not
   save). Refresh.

Expected: three Conflicts, "3 conflicts to choose".

2. Ctrl+click `FlickerSpeed` and `Tint` on their key names, right-click, and choose **Use JSON**.

Expected: the menu is headed "2 keys". Both use JSON; the counter reads "1 conflict to choose".

3. Right-click `Light.Intensity` alone and choose **Use Asset**.

Expected: the counter disappears.

4. Select another asset in the left list (or Refresh), then come back.

Expected: the three choices are still there.

5. In the Blueprint, change `Tint` again (do not save). Refresh.

Expected: `Tint` is back to **Choose** (its asset value changed, so the choice was dropped). The other two keep theirs.

6. Select `Tint`, right-click and choose **Reset**: nothing changes (it is a conflict, so its default is Choose). Choose it, press
   **Apply and Save**.

Expected: the toast "Applied the JSON to BP_SyncLamp and saved it." The asset is not dirty, the JSON has the chosen values, and
the asset leaves the tab.

**3.17 Apply and Save, then Ctrl+Z**

1. Change `FlickerSpeed` in the JSON to `"14.0"`. Refresh, press **Apply and Save**.
2. Press Ctrl+Z.

Expected: `FlickerSpeed` goes back and the asset is dirty, but there is **no** undo toast and the asset is **not** listed: the JSON
already has `14.000000`, and this is an ordinary edit now.

3. Save.

Expected: the JSON gets the old value back (shown in git as a change).

**3.18 Apply and Save, checkout declined** (only with source control)

1. With source control connected and the `.uasset` not checked out, change the JSON, Refresh, press **Apply and Save**, and decline
   the checkout.

Expected: the warning toast "Applied the JSON to BP_SyncLamp, but it was not saved. …". The asset is dirty and not listed, as after
**Apply**. Saving it later writes the JSON.

## 4. Bad files

Each of these leaves the asset listed and changes nothing in it. After each one, restore the file (or use **All from Asset** and
**Apply**, or **Overwrite JSON** for an unreadable file).

| # | Change in `BP_SyncLamp.json` | Expected |
|---|---|---|
| 4.1 | `"FlickerSpeed": "abc"` | Listed as a normal key. **Apply** shows "Cannot apply the JSON of BP_SyncLamp; nothing was changed for them". The Output Log names the key. The asset stays listed. |
| 4.2 | Add `"Nope": "1"` and leave it on JSON | Apply fails as in 4.1: "unknown property". |
| 4.3 | Rename `FlickerSpeed` to `flickerSpeed` with a different value | Apply fails: "unknown property (did you mean 'FlickerSpeed'?)". With the **same** value it is not listed at all (key comparison ignores case); the commandlet's `-verify` reports it (12.2). |
| 4.4 | `"Tint": "(X=1)"` | Apply fails: the struct field is unknown. |
| 4.5 | `"FlickerSpeed": "2.0 extra"` | Apply fails: unexpected text after the value. |
| 4.6 | Remove a closing `}` | Status "Cannot read the JSON" (orange). The detail line shows the parse error. No keys are listed; Apply, Apply and Save and the All/Reset buttons are disabled. **Overwrite JSON** is shown and works. |
| 4.7 | `"schema": 1` | "Cannot read the JSON", and the error says the file is from an older version of the plugin. |
| 4.8 | `"FlickerSpeed": 4` (a number) | "Cannot read the JSON": the value must be a string. |
| 4.9 | Add a top-level field `"reset": []` | "Cannot read the JSON": unknown field. |
| 4.10 | Add `"components": { "Nope": { "Intensity": "1" } }` inside `overrides`, on JSON | Apply fails: unknown component. |
| 4.11 | Git conflict markers (`<<<<<<<`) in the file | "Cannot read the JSON". |

**4.12 JSON with no asset**

1. Copy `BP_SyncLamp.json` to `BP_Missing.json` in the same folder. Refresh.

Expected: the error toast "JSON Sync: 1 JSON files could not be checked. See the Output Log", and the log says there is no asset.
It appears on every Refresh and start until you delete the file.

**4.13 Wrong `asset` field (known issue)**

1. Change `"asset"` in `BP_SyncLamp.json` to another path, plus a value change. Refresh and apply.

Expected: the editor does not check the field, so the apply works and the next save fixes the field. The commandlet's `-import`
rejects such a file and `-verify` reports it.

## 5. Data assets (Two Way)

**5.1 Automatic apply**

1. Close the editor. In `DA_SyncItem.json`, set `"Damage": "20.0"`. Start the editor.

Expected:

- No asset is listed.
- The toast "Applied JSON to DA_SyncItem automatically (Two Way). Not saved; Ctrl+Z does not revert it." appears.
- `DA_SyncItem` is dirty and shows Damage 20.

2. Save it.

Expected: the JSON reads `"20.000000"`.

**5.2 Not applied when the asset has unsaved changes**

1. Open `DA_SyncItem` and change `Label` (do not save). Change `Damage` in the JSON. Refresh.

Expected: it is **not** applied. It is listed with every differing key as Conflict (no base, because the asset was dirty), and both
`Damage` and `Label` are shown.

**5.3 Bad value**

1. Close the editor, set `"Damage": "abc"`, and start.

Expected: the "Cannot apply the JSON of DA_SyncItem" toast appears and the asset stays listed.

**5.4 Native data asset**

1. Close the editor, change a value in `IA_SyncTest.json` (e.g. `"ValueType": "Axis2D"`), and start.

Expected: applied automatically, as in 5.1.

**5.5 Data asset of a parent Blueprint class**

1. Change `Damage` in the **class** `PDA_SyncItem.json` (not the asset), to a value `DA_SyncItem` does not override. Refresh.

Expected: `PDA_SyncItem` is listed (Blueprints are not Two Way). After **Apply**, `DA_SyncItem` follows if it does not override
`Damage`, and keeps its own value if it does.

**5.6 Ctrl+Z does not revert an automatic apply**

1. Close the editor. In `DA_SyncItem.json`, set `"Damage": "21.0"`. Start the editor; it is applied as in 5.1.
2. Press Ctrl+Z.

Expected: nothing is undone (Edit > Undo History is empty). Damage stays 21, no undo toast, and `DA_SyncItem` is not listed.

3. Set Damage back to its old value in the asset and save.

Expected: the JSON has the old value again. This is the way to back out a JSON change (or revert the JSON in git).

**5.7 Not applied automatically when the asset is in the undo history**

1. Open `DA_SyncItem`, change `Label`, and save. Change `Damage` in the JSON and press **Refresh**.

Expected: it is **not** applied. The log says "not applied automatically, because the undo history has changes to it". The asset
is listed with `Damage` Changed in JSON.

2. Click **Apply**.

Expected: the toast "Applied JSON to DA_SyncItem. Not saved; Ctrl+Z reverts." Damage has the JSON value.

3. Press Ctrl+Z.

Expected: Damage goes back, the undo warning toast appears, and the asset is listed again.

4. Press **Refresh**.

Expected: still not applied (the apply is a redo step), with the same log line.

5. Close the editor, choose **Don't Save**, and start it again.

Expected: applied automatically, as in 5.1: the undo history starts empty.

## 6. No base

**6.1 Fresh machine (no cache)**

1. Close the editor. Delete `Saved/JsonBlueprintSync/`. Change `FlickerSpeed` in `BP_SyncLamp.json` and `Damage` in
   `DA_SyncItem.json`. Start the editor.

Expected:

- Start-up takes a little longer, because every synced asset with a JSON file is loaded once.
- Assets whose JSON matches are not listed, and `cache.json` is rebuilt with their hashes.
- Both changed assets are listed with every differing key as Conflict and **Choose**. `DA_SyncItem` is **not** applied
  automatically.
- The detail line says it is unknown which side changed each key, so every key is a conflict.
- **All from JSON** sets every key to JSON and the counter disappears; **Reset** brings back **Choose** for all of them.

**6.2 Base deleted while pending**

1. With an asset listed with a base (3.4), delete its file under `Saved/JsonBlueprintSync/Base/`. Refresh.

Expected: every key becomes Conflict.

## 7. Propagation and inherited components

**7.1 Parent changes reach children and placed actors**

1. Open `L_SyncTest`. Place one `BP_SyncLamp` (A), one more `BP_SyncLamp` (B) with `FlickerSpeed` set to 99 in its Details, and
   one `BP_SyncLampChild` (C). Save the level.
2. Reset `FlickerSpeed` on `BP_SyncLampChild` (from test 2.4) and save it.
3. In `BP_SyncLamp.json`, change `FlickerSpeed` to `"13.0"`. Refresh, then **Apply**.

Expected:

- `BP_SyncLamp`, `BP_SyncLampChild` and A and C in the level have 13.
- B keeps 99.
- `BP_SyncLampChild` and the level are dirty as well.
- One Ctrl+Z reverts all of them.

**7.2 Inherited component override**

1. In `BP_SyncLampChild.json`, set `components.Light.Intensity` to `"123.0"`. Refresh, then **Apply**.

Expected: in the child's Components panel, `Light` has Intensity 123. The parent is unchanged.

2. Save the child, then remove `Intensity` from the child's JSON, Refresh and Apply.

Expected: the child's Light goes back to the parent's Intensity.

## 8. Widget Blueprints

**8.1 Write**

1. In `WBP_SyncTest`, set `Title` Text = "Hello", Render Opacity = 0.5, and in its Slot set ZOrder = 3. Compile and save.

Expected: the JSON has `widgets.Title` with `RenderOpacity` and `Text` (as `NSLOCTEXT(...)`), and `widgets."Title.Slot"` with
`ZOrder` and the slot's layout values that differ from the defaults (e.g. `LayoutData`).

**8.2 Apply**

1. Change `ZOrder` to `"5"` and the opacity to `"0.25"` in the JSON. Refresh.

Expected: the keys are `Title.RenderOpacity` and `Title.Slot.ZOrder`.

2. Press **Apply**.

Expected: the Designer shows the new values straight away. Play In Editor shows them too, since PIE compiles the dirty Widget
Blueprint first. Add the widget to the viewport from a level Blueprint to see it.

**8.3 Unknown widget**

1. Add `"Nope": { "RenderOpacity": "0.5" }` under `overrides.widgets`. Refresh, then Apply.

Expected: apply fails with "unknown widget". Widgets are not created from JSON.

## 9. Rename, move, delete

**9.1 Rename**

1. Rename `BP_SyncLamp` to `BP_SyncLamp2` in the Content Browser.

Expected:

- `BP_SyncLamp.json` is gone, and `BP_SyncLamp2.json` exists with `"asset"` updated and the same values.
- `cache.json` has no entry for either name until the renamed asset is next saved (Unreal may save it as part of the rename).
- `BP_SyncLampChild.json`'s `parent` is updated the next time the child is saved.

**9.2 Edit right after a rename (known issue)**

1. Rename an asset, then change a value and save it, before any other save.

Expected: the save may be reported as "did not write its JSON", and the asset listed with every key as Conflict, although nothing
came from outside. **All from Asset** and **Apply** resolve it. Report what you see. This case is not confirmed yet.

**9.3 Move**

1. Move `DA_SyncItem` to a subfolder.

Expected: the JSON moves to the matching subfolder under `JsonBlueprintSync/Game/...`.

**9.4 Rename a folder**

1. Rename the `JsonSyncTest` folder.

Expected: every JSON file moves to the new folder. Then right-click > **Update Redirector References** on the old folder. The JSON
files are not affected.

**9.5 Delete**

1. Duplicate `DA_SyncItem`, save the copy (a JSON is written), then delete the copy.

Expected: its JSON file is deleted. If you cancel the delete dialog, the file stays.

**9.6 Rename a pending asset**

1. Make `DA_SyncItem` pending (5.2), then rename it.

Expected: it leaves the tab. The JSON (with the unreviewed change) moves to the new path. The next save goes through the save guard
(3.11).

## 10. Settings

**10.1 Two Way is not offered for Blueprints**

Expected: in Project Settings, the Blueprints and Widget Blueprints drop-downs offer None and Write JSON Only only, while Data
Assets also offers Two Way. Changing a setting writes `Config/DefaultEditor.ini`.

**10.2 Two Way forced in the ini**

1. Close the editor and add `Blueprints=TwoWay` under `[/Script/JsonBlueprintSync.JsonBlueprintSyncSettings]` in
   `Config/DefaultEditor.ini`. Change a Blueprint's JSON and start.

Expected: the Blueprint is listed, not applied automatically.

**10.3 Mode None**

1. Set Blueprints = None. Change `BP_SyncLamp` and save it. Change its JSON and Refresh.

Expected: the JSON is not written on save, and the JSON change is not listed.

2. Set the mode back to Write JSON Only and Refresh.

Expected: the asset is listed, because the JSON and the asset now differ. Both the JSON and the `.uasset` changed since the last
sync, so every key shows as **Conflict** and **Choose**. Choose each key, or use **All from Asset** and **Apply**.

3. Repeat step 1, but this time do not change the JSON. Set the mode back and Refresh.

Expected: listed as "JSON out of date" (13.1).

**10.4 Data assets Write JSON Only**

1. Set Data Assets = Write JSON Only and change `DA_SyncItem.json`. Refresh.

Expected: listed, not applied automatically.

**10.5 `JsonDir`**

1. Set `JsonDir` = `JsonBlueprintSyncTest`. Save `BP_SyncLamp`.

Expected: `<Project>/JsonBlueprintSyncTest/Game/.../BP_SyncLamp.json` is created and the old folder is left as it is. Set it back
and delete the test folder.

## 11. PIE and line endings

**11.1 PIE**

1. Start Play In Editor. Run `JsonBlueprintSync.Sync`.

Expected: a warning toast (yellow triangle) that JSON files are not checked during Play In Editor. Nothing else happens. The same
for **Refresh** in the JSON Sync tab.

**11.2 CRLF files**

1. Close the editor. Convert `BP_SyncLamp.json` to CRLF line endings (e.g. in VS Code, click "LF" in the status bar and save as
   CRLF). Start.

Expected: not listed. Saving the asset without changes does not rewrite the file. With `*.json text eol=lf` in `.gitattributes`,
git does not show it as changed either.

## 12. Commandlet

Close the editor first. `-import` saves packages, and the editor may have them loaded. From the project folder, in PowerShell:

```powershell
$Editor = "<Engine>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
$Project = "$PWD\<Project>.uproject"
& $Editor $Project -run=JsonBlueprintSync -verify -unattended -nosplash; "exit code: $LASTEXITCODE"
```

**12.1 Export**

1. Run with `-export`.

Expected: exit code 0, and every synced asset has a JSON file. A second run changes nothing (`git status` is clean). JSON files
without an asset are reported as warnings.

**12.2 Verify**

1. Run with `-verify` right after an export.

Expected: exit code 0.

2. Change a value in one file and run it again.

Expected: exit code 1, and the log names the file.

3. Restore the value but write it as `"2.5"` instead of `"2.500000"`.

Expected: still exit code 1. `-verify` compares bytes. Re-export to fix it.

4. Delete one file, or add an orphan file (4.12).

Expected: exit code 1.

**12.3 Import**

1. Change `FlickerSpeed` in `BP_SyncLamp.json` and `Damage` in `DA_SyncItem.json`. Run with `-import`.

Expected:

- Exit code 0.
- Both `.uasset` files are saved. `BP_SyncLampChild` is saved too, if the value reached it.
- `L_SyncTest` is **not** saved.
- Starting the editor lists nothing, and the assets show the new values.

**12.4 Import ignores Write JSON Only**

Expected: a Blueprint's JSON is applied by `-import` even though its mode is Write JSON Only. Only None is skipped.

**12.5 `-nosave`**

1. Change a value and run `-import -nosave`.

Expected: the log reports the change, but no `.uasset` is modified on disk.

**12.6 Import errors**

1. Set an invalid value (4.1) or a wrong `"asset"` (4.13), and run `-import`.

Expected: exit code 1. Nothing is applied for that file. The other files are still applied.

**12.7 `-dir` and `-jsondir`**

1. Run `-verify -dir=/Game/Developers/<you>/JsonSyncTest`.

Expected: only that folder is checked.

2. Run `-export -jsondir=Saved/JsonExportTest`.

Expected: the files are written under `<Project>/Saved/JsonExportTest/Game/...`.

3. Run with `-dir=Nope`.

Expected: an error about the path. With no action, or with two actions, you also get an error.

In Git Bash, prefix the command with `MSYS_NO_PATHCONV=1`. Otherwise `-dir=/Game/...` becomes a Windows path and is rejected.

## 13. `.uasset` changes and reparenting

These tests make a `.uasset` change arrive without its JSON, as when a teammate commits only the `.uasset`. The trick: keep a copy
of the old `.uasset` outside the project, change and save the asset (which also writes the JSON), then put the old `.uasset` back
with the editor closed. The JSON then describes the newer asset, and the cache knows both hashes. If the project is in git, `git
stash` / `git checkout -- <file>` on the `.uasset` alone does the same.

**13.1 `.uasset` changed, JSON not: JSON out of date**

1. Close the editor. Copy `BP_SyncLamp.uasset` to a folder outside the project.
2. Start, set `FlickerSpeed` to 4 on `BP_SyncLamp`, and save. Close the editor.
3. Copy the old `.uasset` back over the new one. Start.

Expected:

- `BP_SyncLamp` is listed as "1 keys, JSON out of date" (not orange).
- `FlickerSpeed` is Changed in **Asset**, Use Asset. The JSON value is 4, the asset value is the old one.
- The detail line says the JSON has no changes of its own.

4. Open `BP_SyncLamp` and press **Save** in the Blueprint editor toolbar (it saves even when nothing changed).

Expected: the JSON is written with the asset's value, the asset leaves the list, and there is **no** "did not write its JSON" toast.

**13.2 Out of date, resolved with Apply**

1. Repeat 13.1 steps 1–3. Press **Apply** (every key already uses the asset).

Expected: the JSON gets the asset's value and the asset leaves the list.

**13.3 Out of date, reverting the asset's change**

1. Repeat 13.1 steps 1–3. Click the JSON value of `FlickerSpeed` and press **Apply and Save**.

Expected: the asset gets 4 from the JSON (the old `.uasset` change is reverted), and saving writes the JSON unchanged.

**13.4 Only the graph changed**

1. Repeat 13.1, but in step 2 add a node to the Event Graph (e.g. a Print String) instead of changing a value. Compile and save.

Expected: nothing is listed. Restoring the old `.uasset` changed its saved hash but not its values, so the new hash is just recorded
in `cache.json` (the `asset` field changes). A second start loads nothing.

**13.5 Both changed**

1. Repeat 13.1 steps 1–3, but before starting in step 3, also change `bStartOn` in `BP_SyncLamp.json`.

Expected: listed with every differing key (`FlickerSpeed`, `bStartOn`) as **Conflict**, all **Choose**.

**13.6 Data asset out of date (Two Way)**

1. Repeat 13.1 with `DA_SyncItem` and `Damage`.

Expected: `DA_SyncItem` is listed as "JSON out of date" and **not** applied automatically: no "Applied JSON" toast, and the asset is
not dirty. Saving it writes the JSON.

**13.7 Reparent, saved normally**

1. Create `BP_SyncLampAlt`, a child Blueprint of `BP_SyncLamp`, set its `FlickerSpeed` to 7, and save it.
2. Open `BP_SyncLampChild`, set its `Tint`, then **Class Settings > Parent Class** = `BP_SyncLampAlt`. Compile and save.

Expected: nothing is listed. `BP_SyncLampChild.json` has `"parent"` set to `BP_SyncLampAlt`'s class, still has `Tint`, and does not
list `FlickerSpeed` (it now comes from the new parent).

**13.8 Reparented `.uasset` arrives without its JSON**

1. Close the editor. Copy `BP_SyncLampChild.uasset` outside the project.
2. Start, reparent `BP_SyncLampChild` back to `BP_SyncLamp`, compile and save. Close the editor.
3. Copy the old `.uasset` back (its parent is `BP_SyncLampAlt`). Start.

Expected:

- `BP_SyncLampChild` is listed as "parent differs" (orange). The detail line names both parents and suggests All from Asset, then Apply.
- Few or no keys differ, because a reparent keeps the child's own overrides. Any that do are Changed in **Asset**, Use Asset.
- The Output Log has a warning naming both parents.

4. Press **Apply**.

Expected: the JSON's `"parent"` is `BP_SyncLampAlt`'s class again and the asset leaves the list.

**13.9 JSON written for another parent**

1. Keep a copy of `BP_SyncLampChild.json`. Reparent `BP_SyncLampChild` to `BP_SyncLamp` and save. Close the editor.
2. Copy the old JSON back (it names `BP_SyncLampAlt`), and change a value in it. Start.

Expected: listed as "parent differs", every differing key a **Conflict**, all **Choose**. With a data asset in Two Way mode (change
the `"parent"` of `DA_SyncItem.json` to another class path), nothing is applied automatically.

3. Edit only `"parent"` in a JSON so it names another class, without changing values. Refresh.

Expected: listed as "0 keys, parent differs". **Apply** is enabled (nothing to choose) and writes the asset's real parent back.

**13.10 Cache from an older plugin version**

1. Close the editor. In `cache.json`, delete the `"asset"` field of every entry. Start.

Expected: nothing is listed, and `cache.json` has the `"asset"` fields again.

## 14. Notifications for problems

Each of these used to be reported only in the Output Log.

**14.1 Unreadable cache**

1. Close the editor. Replace the contents of `Saved/JsonBlueprintSync/cache.json` with `not json`. Start.

Expected: an error toast "JSON Sync: cannot read …cache.json, so the last sync is unknown…". Assets whose JSON differs from the
asset are listed with every key as Conflict. `cache.json` is valid again afterwards.

**14.2 Read-only cache**

1. Mark `cache.json` read-only (Properties > Read-only). Change `FlickerSpeed` on `BP_SyncLamp` and save.

Expected: the JSON is written, and an error toast "JSON Sync: cannot write …cache.json. Is it read-only?…".

2. Change it again and save.

Expected: no new toast (only a log warning).

3. Clear the read-only flag, change it again and save.

Expected: no toast; `cache.json` is updated. Making it read-only again shows the toast again on the next save.

**14.3 Asset that cannot be loaded**

1. Close the editor. Copy `BP_SyncLamp.json` to `BP_SyncNotAsset.json`, set its `"asset"` to `/Game/JsonSyncTest/BP_SyncNotAsset`,
   and create an empty file `Content/JsonSyncTest/BP_SyncNotAsset.uasset`. Start.

Expected: the error toast "JSON Sync: 1 JSON files could not be checked", and the log names the package (as "no asset" or "cannot
load the asset", depending on whether the registry lists the broken file). Delete both files afterwards.

**14.4 Rename with an unreadable JSON**

1. Put `<<<<<<< HEAD` on the first line of `BP_SyncLamp.json`. Rename `BP_SyncLamp` to `BP_SyncLamp2` in the Content Browser.

Expected: a warning toast "renamed BP_SyncLamp to BP_SyncLamp2, but its JSON cannot be read (…), so it was not moved". The old file
is still there. Saving `BP_SyncLamp2` writes `BP_SyncLamp2.json`. Delete the old file and rename the asset back.

**14.5 JSON that cannot be deleted**

1. Duplicate `DA_SyncItem` and save the copy. In PowerShell, hold its JSON open:
   `$f = [IO.File]::Open("<path to the copy's JSON>", 'Open', 'Read', 'None')`
2. Delete the copy in the Content Browser.

Expected: an error toast "JSON Sync: cannot delete … Is it open in another program?". Run `$f.Close()` and delete the file by hand.

## 15. Cleanup

1. Delete the `JsonSyncTest` folder in the Content Browser.

Expected: its JSON files are deleted as well.

2. Delete any leftover test JSON files or folders (`BP_Missing.json`, `JsonBlueprintSyncTest/`, `Saved/JsonExportTest/`). Restore
   Project Settings and `Config/DefaultEditor.ini`. Delete the `.uasset` and JSON copies from section 13.
