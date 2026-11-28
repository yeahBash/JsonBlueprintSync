# JsonDataAssetSync manual tests

A checklist for checking the plugin by hand in the editor and on the command line. The automated tests cover the library (capture,
apply, format, merge, cache, missing pairs) and the rules for choosing a base and for applying automatically, but not the editor
subsystem, the JSON Sync tab or the commandlet. Those are covered here. For how things work and why, see
[Internals.md](Internals.md).

Each test has steps and the expected result. Tests marked **(known issue)** describe current behaviour that is a known weak spot.
They check that it behaves as documented, not that it is ideal.

- [0. Automated tests](#0-automated-tests)
- [1. Setup](#1-setup)
- [2. Writing JSON on save](#2-writing-json-on-save)
- [3. Reviewing JSON changes (Manual mode)](#3-reviewing-json-changes-manual-mode)
- [4. Bad files](#4-bad-files)
- [5. Automatic mode](#5-automatic-mode)
- [6. No base](#6-no-base)
- [7. Missing pairs](#7-missing-pairs)
- [8. Rename, move, delete](#8-rename-move-delete)
- [9. Settings](#9-settings)
- [10. PIE and line endings](#10-pie-and-line-endings)
- [11. Commandlet](#11-commandlet)
- [12. `.uasset` changes and class changes](#12-uasset-changes-and-class-changes)
- [13. Notifications for problems](#13-notifications-for-problems)
- [14. Cleanup](#14-cleanup)

## 0. Automated tests

Close the editor, or run them from a second shell while it is open (it is slower). From the project folder, in PowerShell:

```powershell
& "<Engine>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$PWD\<Project>.uproject" `
  -ExecCmds="Automation RunTests JsonDataAssetSync" -TestExit="Automation Test Queue Empty" -unattended -nullrhi -nosplash
```

`-TestExit` quits once the tests have run. Do not add `Quit` to `-ExecCmds`: it runs before the tests, and the process hangs.

Expected: 12 tests, each logging `Result={Success}`, and no `Result={Fail}`. The full log is in `Saved/Logs/<Project>.log`.
To filter it: `Select-String -Path Saved\Logs\<Project>.log -Pattern "Result=\{"`.

In the editor you can also run them from **Tools > Test Automation**: filter by `JsonDataAssetSync`, tick the group and press
Start Tests.

Some `Condition failed` lines may appear at startup. They come from the engine's own tests, not the plugin.

## 1. Setup

### Useful places

| What | Where |
|---|---|
| JSON files | `<Project>/JsonDataAssetSync/Game/...` (setting `JsonDir`) |
| Local cache | `<Project>/Saved/JsonDataAssetSync/cache.json` |
| Base snapshots | `<Project>/Saved/JsonDataAssetSync/Base/Game/...` |
| Log | Output Log, filtered by the category `LogJsonDataAssetSync` |
| Tab | **Window > JSON Sync**, or the JSON Sync entry in the status bar |
| Rerun the check | **Refresh** in the tab, or the console command `JsonDataAssetSync.Sync` |
| Settings | **Project Settings > Plugins > JSON Data Asset Sync** |

Keep a text editor open on the JSON folder. Unless a test says otherwise, use the default settings (Mode Automatic, no Excluded
Folders, `JsonDir` = `JsonDataAssetSync`). Section 3 switches to Manual.

### Test assets

Create these in a sandbox folder, e.g. `/Game/Developers/<you>/JsonSyncTest/` (written `.../` below). Compile and save each one.

| Asset | How to make it |
|---|---|
| `PDA_SyncItem` | Blueprint Class, parent **PrimaryDataAsset**. Variables, all **Instance Editable** (eye icon open): `Damage` (Float), `Label` (String), `Tint` (Linear Color). Add a fourth, `Secret` (Float), and leave it **not** Instance Editable. |
| `PDA_SyncItemAlt` | Right-click `PDA_SyncItem` > Create Child Blueprint Class. Change nothing. Used only for its class path (12.6). |
| `DA_SyncItem` | Miscellaneous > Data Asset, class `PDA_SyncItem`. |
| `IA_SyncTest` | Input > Input Action. A native C++ data asset (needs the Enhanced Input plugin; any other native `UDataAsset` type also works). |

After saving them, `cache.json` should have an entry for `DA_SyncItem` and `IA_SyncTest`, each with a `json` and an `asset` hash.
`PDA_SyncItem` and `PDA_SyncItemAlt` are Blueprints, so they have no JSON and no entry.

## 2. Writing JSON on save

**2.1 Data asset values**

1. In `DA_SyncItem`, set `Damage` = 10 and `Label` = `Sword`. Save.

Expected `.../DA_SyncItem.json`:

```json
{
  "schema": 2,
  "asset": "/Game/Developers/<you>/JsonSyncTest/DA_SyncItem",
  "parent": "/Game/Developers/<you>/JsonSyncTest/PDA_SyncItem.PDA_SyncItem_C",
  "overrides": {
    "self": {
      "Damage": "10.000000",
      "Label": "Sword"
    }
  }
}
```

- Only changed values are present: no `Tint`.
- Keys are sorted case-sensitively.
- The file uses LF line endings, two-space indent, a trailing newline and no BOM.
- `cache.json` has the asset's hashes and no `pending`.

**2.2 Not instance-editable variables and Blueprints**

1. In `PDA_SyncItem` Class Defaults, set `Secret` = 5. Compile and save.

Expected: no `PDA_SyncItem.json` is written (a Blueprint is not synced), and `Secret` never appears in `DA_SyncItem.json`, because
it is not editable on the asset.

**2.3 Saving without changes does not rewrite the file**

1. Note the file's modified time. Make a change in `DA_SyncItem` and undo it (the asset becomes dirty), then save.

Expected: the file's modified time does not change, and `git status` shows nothing for it.

**2.4 Resetting a value removes its key**

1. In `DA_SyncItem`, click the reset arrow next to `Label`. Save.

Expected: `Label` is gone from `self`. Set it back to `Sword` and save.

**2.5 Native data asset**

1. In `IA_SyncTest`, set Value Type = Axis1D (float), or change any other property. Save.

Expected: `IA_SyncTest.json` with `parent` `/Script/EnhancedInput.InputAction` and the changed values in `self`.

## 3. Reviewing JSON changes (Manual mode)

Set **Mode = Manual** in Project Settings first; section 5 sets it back. Start each test from a clean state: nothing in the JSON
Sync tab and `DA_SyncItem` saved.

**3.1 JSON edited while the editor is closed**

1. Close the editor. In `DA_SyncItem.json`, change `Damage` to `"4.0"`. Start the editor.

Expected:

- The toast "JSON Sync: 1 asset(s) differ from their JSON" appears, with **Review** and **Later**.
- **Review** opens the tab. `DA_SyncItem` is listed with Status "1 keys". The list has two columns, Asset and Status.
- The key `Damage` has JSON value `4.0`, Asset value `10.000000`, Changed in **JSON**, and Use **JSON** in blue (Asset would be
  green, Choose orange). The JSON value has a highlighted background; the asset value is dimmed. No conflict counter is shown, and
  **Apply** and **Apply and Save** are enabled. **Write JSON** and **Apply JSON** are not shown.
- `cache.json` has `"pending": true` for the asset, and `Saved/JsonDataAssetSync/Base/.../DA_SyncItem.json` exists.

2. Press **Apply**.

Expected:

- Toast "Applied JSON to DA_SyncItem. Not saved; Ctrl+Z reverts."
- The asset leaves the tab and the summary reads "Every synced asset matches its JSON file."
- The asset is dirty (asterisk) and `Damage` is 4.0. The JSON file is not touched yet.
- The base file is deleted, and `pending` is gone from the cache.

3. Save the asset.

Expected: the file now reads `"Damage": "4.000000"` (canonical form), and the cache hash is updated.

**3.2 Closing without saving after Apply**

1. Repeat 3.1 up to **Apply** (change the value to `"5.0"`), then close the editor and choose **Don't Save**. Start it again.

Expected: the asset is listed again with the same key. The change is not lost.

**3.3 Undo after Apply**

1. Repeat 3.1 up to **Apply** (value `"6.0"`). Press Ctrl+Z.

Expected:

- `Damage` goes back to its old value.
- A warning toast "JSON Sync: undo reverted the JSON applied to DA_SyncItem. It is back in the JSON Sync tab…".
- The asset is listed again with `Damage` Changed in **JSON**, Use **JSON**, as before the apply. The base file is back and the
  cache has `"pending": true`.

2. Press Ctrl+Y.

Expected: `Damage` is 6.0 again and the asset leaves the tab (no toast).

3. Press Ctrl+Z again, then save the asset.

Expected: the "Saved DA_SyncItem, but did not write its JSON: the JSON has changes you have not reviewed" toast. The JSON still
reads `"6.0"`. Press **Apply and Save** to finish.

**3.4 JSON edited while the editor is open**

1. With the editor open, change `Damage` in the JSON to `"7.0"`. Wait: nothing happens (there is no file watching).
2. Press **Refresh** in the tab, or run `JsonDataAssetSync.Sync`.

Expected: listed as in 3.1. Each further Refresh shows the toast again (known issue).

**3.5 Asset changed after the JSON change was detected**

1. Continue from 3.4 (pending, not applied). In the asset, set `Label` = `Axe` (do not save).
2. Press **Refresh** (or select another asset and back).

Expected:

- Two keys: `Damage` (Changed in JSON, Use JSON) and `Label` (JSON `Sword`, Asset `Axe`, Changed in **Asset**, Use Asset).
- Before the Refresh, the tab still showed one key. The tab does not follow asset edits live.

3. Press **Apply**, then save.

Expected: `Damage` takes the JSON value and `Label` stays `Axe`. The saved JSON has both `7.000000` and `Axe`.

**3.6 Conflict**

1. Change `Damage` in the JSON to `"8.0"` and press Refresh (asset saved, so it gets a base).
2. In the asset, set `Damage` = 9 (do not save). Press Refresh.

Expected:

- `Damage` has JSON `8.0`, Asset `9.000000`, Changed in **Conflict** (orange), and Use **Choose** (orange). Neither value is
  highlighted. Status is "1 keys, conflicts".
- Below the list: "1 conflict to choose". **Apply** and **Apply and Save** are disabled.

3. Click the asset value `9.000000`.

Expected: it is highlighted, the JSON value is dimmed, Use shows **Asset**, the counter disappears and Apply is enabled.

4. Click the JSON value `8.0`, then press **Apply**.

Expected: Use switched to **JSON** before the apply. The value becomes 8.0.

**3.7 Removing a key in the JSON resets the value**

1. Delete the `Damage` line from the JSON (mind the commas). Refresh.

Expected: the JSON value is `(default)` in grey and the key is Changed in JSON. After Apply, `Damage` shows the class default,
with no reset arrow.

**3.8 Keeping every asset value**

1. Change the JSON to `"Damage": "11.0"`, Refresh, select the asset and press **All from Asset**, then **Apply**.

Expected: the file is rewritten with the asset's values at once, the asset leaves the tab, and the asset is **not** dirty.

**3.9 Apply and Save with nothing to apply**

1. As 3.8, but press **All from Asset**, then **Apply and Save**.

Expected: the toast "Wrote the JSON of DA_SyncItem; the asset did not change." The file is rewritten, and the `.uasset` is **not**
saved (its timestamp does not change).

**3.10 Saving a pending asset**

1. Change the JSON, Refresh, leave the asset listed. Change something in the asset and save it.

Expected:

- The asset is saved, but the JSON file is unchanged.
- The toast "Saved DA_SyncItem, but did not write its JSON: the JSON has changes you have not reviewed" appears, with **Review**.
- The asset stays in the tab.

**3.11 Save guard**

1. Change the JSON (e.g. `Damage` `"12.0"`) and do **not** Refresh.
2. Change something else in the asset (e.g. `Label`) and save it.

Expected:

- The save-skipped toast appears and the JSON file still has your edit.
- The asset is listed with **every** differing key as Conflict, all **Choose**, and the counter shows how many.
- The detail line says "every key is a conflict".

**3.12 Same value, other text**

1. Close the editor. In the JSON, rewrite `"Damage": "4.000000"` (whatever the current value is) as `"Damage": "4"`. Start the
   editor.

Expected: nothing is listed (the values are the same), and the cache records the new hash. The next save of the asset rewrites the
file in canonical form.

**3.13 Unknown key, kept from the asset**

1. Add `"Nope": "1"` to `overrides.self` in the JSON. Refresh.

Expected: `Nope` is listed (JSON `1`, Asset `(default)`).

2. Click its asset value `(default)` (Use: Asset) and press **Apply**.

Expected: the asset is applied. Since nothing changed, the JSON is rewritten at once without `Nope`.

**3.14 JSON changes between Apply and Save**

1. Change `Damage` in the JSON to `"10.0"` and `Label` to `"Bow"`. Refresh.
2. Click the asset value of `Label` (Use: Asset) and press **Apply**. Do not save.
3. Change `Tint` in the JSON (e.g. `"(R=0.000000,G=1.000000,B=0.000000,A=1.000000)"`). Do **not** Refresh. Save the asset.

Expected:

- The toast "Saved DA_SyncItem, but did not write its JSON: the JSON changed again after you reviewed it", with Review.
- The JSON still has your `Tint` edit.
- The asset is listed with `Tint` Changed in **JSON** (Use JSON) and `Label` Changed in **Asset** (Use Asset: the value you kept).
  `Damage` is not listed.

4. Press **Apply and Save**.

Expected: the JSON has the new `Tint`, `Damage` `10.000000`, and the old `Label`.

**3.15 Refresh after Apply**

1. Change `Damage` in the JSON to `"11.0"` and `Label` to `"Spear"`. Refresh. Click the asset value of `Label`, then **Apply**. Do
   not save.
2. Press **Refresh**.

Expected: the asset is not listed again (no every-key Conflict list).

3. Change `Tint` in the JSON and press **Refresh**.

Expected: listed as in 3.14 step 3, with the normal pending toast. Saving now does not write the JSON.

**3.16 Choosing several keys at once**

1. Change `Damage`, `Label` and `Tint` in the JSON. In the asset, change all three to other values (do not save). Refresh.

Expected: three Conflicts, "3 conflicts to choose".

2. Ctrl+click `Damage` and `Tint` on their key names, right-click, and choose **Use JSON**.

Expected: while clicking, the two rows are highlighted as selected. The menu is headed "2 keys". Both use JSON; the counter reads
"1 conflict to choose". Clicking a single value afterwards selects only that row, and it is not highlighted.

3. Right-click `Label` alone and choose **Use Asset**.

Expected: the counter disappears.

4. Select another asset in the left list (or Refresh), then come back.

Expected: the three choices are still there.

5. In the asset, change `Tint` again (do not save). Refresh.

Expected: `Tint` is back to **Choose** (its asset value changed, so the choice was dropped). The other two keep theirs.

6. Select `Tint`, right-click and choose **Reset**: nothing changes (it is a conflict, so its default is Choose). Choose it, press
   **Apply and Save**.

Expected: the toast "Applied the JSON to DA_SyncItem and saved it." The asset is not dirty, the JSON has the chosen values, and the
asset leaves the tab.

**3.17 Apply and Save, then Ctrl+Z**

1. Change `Damage` in the JSON to `"14.0"`. Refresh, press **Apply and Save**.
2. Press Ctrl+Z.

Expected: `Damage` goes back and the asset is dirty, but there is **no** undo toast and the asset is **not** listed: the JSON
already has `14.000000`, and this is an ordinary edit now.

3. Save.

Expected: the JSON gets the old value back (shown in git as a change).

**3.18 Apply and Save, checkout declined** (only with source control)

1. With source control connected and the `.uasset` not checked out, change the JSON, Refresh, press **Apply and Save**, and decline
   the checkout.

Expected: the warning toast "Applied the JSON to DA_SyncItem, but it was not saved. …". The asset is dirty and not listed, as after
**Apply**. Saving it later writes the JSON.

**3.19 Partial struct text**

1. In the asset, set `Tint` to R 0.5, G 0.5, B 0.5, A 1 and save. In the JSON, change `Tint` to `"(R=1.0)"`. Refresh, **Apply**.

Expected: `Tint` is R 1, and G, B and A take the **class default's** values (Black: 0, 0, 0, 1, unless you changed the default in
`PDA_SyncItem`), not the asset's 0.5. Saving writes the whole colour.

**3.20 Status bar**

1. With nothing listed, look at the status bar at the bottom of the level editor.

Expected: directly left of the unsaved assets entry (right of the Compile button), "All JSON Synced" with the merge icon. The
tooltip says no asset has JSON changes.

2. Change `Damage` in `DA_SyncItem.json` and press **Refresh** (or run `JsonDataAssetSync.Sync`).

Expected: the entry turns yellow and reads "1 JSON to Review". Its tooltip lists `DA_SyncItem`.

3. Close the JSON Sync tab and click the entry.

Expected: the tab opens. After **Apply** the entry reads "All JSON Synced" again.

## 4. Bad files

Each of these leaves the asset listed and changes nothing in it. Run them in Manual mode (in Automatic mode the first five are
attempted at once and fail with the same toast). After each one, restore the file (or use **All from Asset** and **Apply**, or
**Write JSON** for an unreadable file).

| # | Change in `DA_SyncItem.json` | Expected |
|---|---|---|
| 4.1 | `"Damage": "abc"` | Listed as a normal key. **Apply** shows "Cannot apply the JSON of DA_SyncItem; nothing was changed for them". The Output Log names the key. The asset stays listed. |
| 4.2 | Add `"Nope": "1"` and leave it on JSON | Apply fails as in 4.1: "unknown property". |
| 4.3 | Rename `Damage` to `damage` with a different value | Apply fails: "unknown property (did you mean 'Damage'?)". With the **same** value it is not listed at all (key comparison ignores case); the commandlet's `-verify` reports it (11.2). |
| 4.4 | `"Tint": "(X=1)"` | Apply fails: the struct field is unknown. |
| 4.5 | `"Damage": "2.0 extra"` | Apply fails: unexpected text after the value. |
| 4.6 | `"Secret": "1"` | Apply fails: the variable is not instance editable, so it is not synced. |
| 4.7 | Remove a closing `}` | Status "Cannot read the JSON" (orange). The detail line shows the parse error. No keys are listed; only **Write JSON** is shown (with the "Discard the unreadable JSON" tooltip), and it works. |
| 4.8 | `"schema": 1` | "Cannot read the JSON", and the error says the file is from an older version of the plugin. |
| 4.9 | `"Damage": 4` (a number) | "Cannot read the JSON": the value must be a string. |
| 4.10 | Add a top-level field `"reset": []` | "Cannot read the JSON": unknown field. |
| 4.11 | Add `"components": {}` inside `overrides` | "Cannot read the JSON": unknown field `overrides.components`. |
| 4.12 | Git conflict markers (`<<<<<<<`) in the file | "Cannot read the JSON". |

**4.13 Wrong `asset` field (known issue)**

1. Change `"asset"` in `DA_SyncItem.json` to another path, plus a value change. Refresh and apply.

Expected: the editor does not check the field, so the apply works and the next save fixes the field. The commandlet's `-import`
rejects such a file and `-verify` reports it.

## 5. Automatic mode

Set **Mode = Automatic** again.

**5.1 Automatic apply**

1. Close the editor. In `DA_SyncItem.json`, set `"Damage": "20.0"`. Start the editor.

Expected:

- No asset is listed.
- The toast "Applied JSON to DA_SyncItem automatically and saved it." appears.
- `DA_SyncItem` shows Damage 20 and is **not** dirty: the `.uasset` was saved (its modified time changed).
- The JSON reads `"20.000000"` (rewritten by the save).

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

**5.5 Local and JSON changes on different keys**

1. Close the editor. Copy `DA_SyncItem.uasset` outside the project. Start, set `Label` = `Club` in the asset, save, and close.
2. Copy the old `.uasset` back, and change `Damage` in `DA_SyncItem.json`. Start.

Expected: listed with every differing key as Conflict, not applied: both the `.uasset` and the JSON changed since the last sync, so
there is no base.

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

Expected: still not applied. The undo left the asset dirty, so the log now says "not applied automatically, because it has
unsaved changes" (the apply is also a redo step, which would stop it too).

5. Close the editor, choose **Don't Save**, and start it again.

Expected: applied automatically and saved, as in 5.1: the undo history starts empty and the asset is clean.

## 6. No base

**6.1 Fresh machine (no cache)**

1. Close the editor. Delete `Saved/JsonDataAssetSync/`. Change `Damage` in `DA_SyncItem.json`. Start the editor.

Expected:

- Start-up takes a little longer, because every synced asset with a JSON file is loaded once.
- Assets whose JSON matches are not listed, and `cache.json` is rebuilt with their hashes.
- `DA_SyncItem` is listed with every differing key as Conflict and **Choose**, and is **not** applied automatically.
- The detail line says it is unknown which side changed each key, so every key is a conflict.
- **All from JSON** sets every key to JSON and the counter disappears; **Reset** brings back **Choose** for all of them.

**6.2 Base deleted while pending**

1. In Manual mode, with an asset listed with a base (3.4), delete its file under `Saved/JsonDataAssetSync/Base/`. Refresh.

Expected: every key becomes Conflict.

## 7. Missing pairs

Missing pairs are only reported: nothing is listed in the tab and nothing is changed.

**7.1 JSON without its data asset**

1. Copy `DA_SyncItem.json` to `DA_Missing.json` in the same folder. Refresh.

Expected:

- A warning toast "JSON Sync: 1 JSON files have no data asset. See the Output Log (LogJsonDataAssetSync) for how to fix each one."
- The log has a warning for the file: "no data asset for this JSON file. Delete the file if the asset was deleted on purpose, or
  restore the asset."
- `DA_Missing` is not in the tab. The toast comes back on every Refresh and start until you delete the file (known issue).

**7.2 Data asset without its JSON**

1. Close the editor. Delete `DA_SyncItem.json`. Start.

Expected: a warning toast "JSON Sync: 1 data assets have no JSON file…", and a log warning naming the package with the fix (save
the asset, run `-export`, or exclude its folder).

2. Open `DA_SyncItem` and save it (the asset editor's Save button saves even when nothing changed). Refresh.

Expected: the JSON is written and no toast appears.

**7.3 JSON of a Blueprint (from an older version of the plugin)**

1. Copy `DA_SyncItem.json` to `PDA_SyncItem.json` and set its `"asset"` to the Blueprint's package. Refresh.

Expected: reported as a JSON without its data asset (7.1), because `PDA_SyncItem` is not a data asset. It is not listed in the tab
and nothing in the Blueprint changes. Delete the file.

**7.4 Both kinds at once**

1. Do 7.1 and 7.2 step 1 together.

Expected: one toast with both counts.

**7.5 Plugin content** (only if the project has a plugin with content)

1. Create a data asset in a project plugin's content folder and save it.

Expected: its JSON is written under `<JsonDir>/<PluginName>/...`. Deleting that JSON with the editor closed reports it as in 7.2.
Data assets in engine plugins are never synced or reported.

## 8. Rename, move, delete

**8.1 Rename**

1. Rename `DA_SyncItem` to `DA_SyncItem2` in the Content Browser.

Expected:

- `DA_SyncItem.json` is gone, and `DA_SyncItem2.json` exists with `"asset"` updated and the same values.
- `cache.json` has no entry for either name until the renamed asset is next saved (Unreal may save it as part of the rename).
- No missing-pair toast on the next Refresh.

**8.2 Edit right after a rename (known issue)**

1. Rename an asset, then change a value and save it, before any other save.

Expected: the save may be reported as "did not write its JSON", and the asset listed with every key as Conflict, although nothing
came from outside. **All from Asset** and **Apply** resolve it. Report what you see. This case is not confirmed yet.

**8.3 Move**

1. Move `DA_SyncItem` to a subfolder.

Expected: the JSON moves to the matching subfolder under `JsonDataAssetSync/Game/...`.

**8.4 Rename a folder**

1. Rename the `JsonSyncTest` folder.

Expected: every data asset's JSON file moves to the new folder. Then right-click > **Update Redirector References** on the old
folder. The JSON files are not affected.

**8.5 Delete**

1. Duplicate `DA_SyncItem`, save the copy (a JSON is written), then delete the copy.

Expected: its JSON file is deleted. If you cancel the delete dialog, the file stays.

**8.6 Rename a pending asset**

1. Make `DA_SyncItem` pending (5.2), then rename it.

Expected: it leaves the tab. The JSON (with the unreviewed change) moves to the new path. The next save goes through the save guard
(3.11).

**8.7 Move into an excluded folder**

1. Add `.../JsonSyncTest/Excluded` to **Excluded Folders**. Move a copy of `DA_SyncItem` (with its JSON) into that folder.

Expected: the copy's old JSON is deleted and no new one is written, now or on save.

## 9. Settings

**9.1 Mode**

Expected: in Project Settings, **Mode** offers Automatic and Manual. Changing it writes `Config/DefaultEditor.ini`
(`Mode=Manual`), and the next Refresh uses it without a restart.

**9.2 Manual**

1. Set Mode = Manual. Close the editor, change `DA_SyncItem.json` and start.

Expected: listed, not applied automatically.

**9.3 Excluded Folders**

1. Add `.../JsonSyncTest/Excluded` to **Excluded Folders** (the picker offers content folders). Create a data asset there and save
   it.

Expected: no JSON is written, and the asset is not reported as missing its JSON. `DefaultEditor.ini` has
`+ExcludedFolders=(Path="/Game/Developers/<you>/JsonSyncTest/Excluded")`.

2. Put a JSON file for it under the matching JSON folder, with a value the asset does not have. Refresh.

Expected: not listed, not applied, not reported.

3. Remove the folder from Excluded Folders and Refresh.

Expected: the JSON is now compared like any other (listed, or applied in Automatic mode). Delete the asset and its JSON afterwards.

**9.4 Plugin disabled for a while**

1. Disable the plugin (Edit > Plugins) and restart. Change `Damage` in `DA_SyncItem` and save it. Enable the plugin and restart.

Expected: `DA_SyncItem` is listed as "1 keys, asset changed" (12.1), in both modes.

2. Repeat, but also change `Label` in `DA_SyncItem.json` while the plugin is off.

Expected: both the `.uasset` and the JSON changed, so every key is **Conflict** and **Choose**.

**9.5 `JsonDir`**

1. Set `JsonDir` = `JsonDataAssetSyncTest`. Save `DA_SyncItem`.

Expected: `<Project>/JsonDataAssetSyncTest/Game/.../DA_SyncItem.json` is created and the old folder is left as it is. A Refresh
reports every other data asset as missing its JSON. Set it back and delete the test folder.

## 10. PIE and line endings

**10.1 PIE**

1. Start Play In Editor. Run `JsonDataAssetSync.Sync`.

Expected: a warning toast (yellow triangle) that JSON files are not checked during Play In Editor. Nothing else happens. The same
for **Refresh** in the JSON Sync tab.

**10.2 CRLF files**

1. Close the editor. Convert `DA_SyncItem.json` to CRLF line endings (e.g. in VS Code, click "LF" in the status bar and save as
   CRLF). Start.

Expected: not listed. Saving the asset without changes does not rewrite the file. With `*.json text eol=lf` in `.gitattributes`,
git does not show it as changed either.

## 11. Commandlet

Close the editor first. `-import` saves packages, and the editor may have them loaded. From the project folder, in PowerShell:

```powershell
$Editor = "<Engine>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
$Project = "$PWD\<Project>.uproject"
& $Editor $Project -run=JsonDataAssetSync -verify -unattended -nosplash; "exit code: $LASTEXITCODE"
```

**11.1 Export**

1. Run with `-export`.

Expected: exit code 0, and every synced data asset has a JSON file. No file is written for Blueprints or for assets in excluded
folders. A second run changes nothing (`git status` is clean). JSON files without a synced asset are reported as warnings.

**11.2 Verify**

1. Run with `-verify` right after an export.

Expected: exit code 0.

2. Change a value in one file and run it again.

Expected: exit code 1, and the log names the file.

3. Restore the value but write it as `"2.5"` instead of `"2.500000"`.

Expected: still exit code 1. `-verify` compares bytes. Re-export to fix it.

4. Delete one file, or add an orphan file (7.1).

Expected: exit code 1.

**11.3 Import**

1. Change `Damage` in `DA_SyncItem.json` and a value in `IA_SyncTest.json`. Run with `-import`.

Expected:

- Exit code 0.
- Both `.uasset` files are saved, and no other package is.
- Starting the editor lists nothing, and the assets show the new values.

**11.4 Import ignores the mode**

Expected: with Mode = Manual, `-import` still applies and saves the files. A file in an excluded folder is skipped, with a log line.

**11.5 `-nosave`**

1. Change a value and run `-import -nosave`.

Expected: the log reports the change, but no `.uasset` is modified on disk.

**11.6 Import errors**

1. Set an invalid value (4.1) or a wrong `"asset"` (4.13), and run `-import`.

Expected: exit code 1. Nothing is applied for that file. The other files are still applied.

**11.7 `-dir` and `-jsondir`**

1. Run `-verify -dir=/Game/Developers/<you>/JsonSyncTest`.

Expected: only that folder is checked.

2. Run `-export -jsondir=Saved/JsonExportTest`.

Expected: the files are written under `<Project>/Saved/JsonExportTest/Game/...`.

3. Run with `-dir=Nope`.

Expected: an error about the path. With no action, or with two actions, you also get an error.

In Git Bash, prefix the command with `MSYS_NO_PATHCONV=1`. Otherwise `-dir=/Game/...` becomes a Windows path and is rejected.

## 12. `.uasset` changes and class changes

These tests make a `.uasset` change arrive without its JSON, as when a teammate commits only the `.uasset`. The trick: keep a copy
of the old `.uasset` outside the project, change and save the asset (which also writes the JSON), then put the old `.uasset` back
with the editor closed. The JSON then describes the newer asset, and the cache knows both hashes. If the project is in git, `git
stash` / `git checkout -- <file>` on the `.uasset` alone does the same. Run them in both modes where it says so; the result is the
same.

**12.1 `.uasset` changed, JSON not: asset changed**

1. Close the editor. Copy `DA_SyncItem.uasset` to a folder outside the project.
2. Start, set `Damage` to 4 on `DA_SyncItem`, and save. Close the editor.
3. Copy the old `.uasset` back over the new one. Start (Automatic mode).

Expected:

- `DA_SyncItem` is listed as "1 keys, asset changed" (orange), and is **not** applied automatically: no "Applied JSON" toast, and
  the asset is not dirty.
- `Damage` is Changed in **Asset**, Use Asset. The JSON value is 4, the asset value is the old one.
- The detail line says the asset changed but its JSON did not, and that this is never fixed automatically.
- Only **Write JSON** and **Apply JSON** are shown; All from JSON, All from Asset, Reset, Apply and Apply and Save are hidden.

**12.2 Asset changed, kept with Write JSON**

1. Repeat 12.1. Press **Write JSON**.

Expected: the JSON gets the asset's (old) value, the asset leaves the list, and the asset is not dirty.

**12.3 Asset changed, kept by saving**

1. Repeat 12.1. Open `DA_SyncItem` and press **Save** in the asset editor's toolbar (it saves even when nothing changed).

Expected: the JSON is written with the asset's value, the asset leaves the list, and there is **no** "did not write its JSON" toast.

**12.4 Asset changed, reverted with Apply JSON**

1. Repeat 12.1. Press **Apply JSON**.

Expected: the toast "Applied JSON to DA_SyncItem. Not saved; Ctrl+Z reverts." The asset gets 4 from the JSON, is dirty, and leaves
the list. Saving it writes the JSON unchanged.

2. Before saving, press Ctrl+Z.

Expected: the undo toast; the asset is listed as "asset changed" again. Saving it now keeps the asset's value, as Write JSON does.

**12.5 Both changed**

1. Repeat 12.1 steps 1–3, but before starting in step 3, also change `Label` in `DA_SyncItem.json`.

Expected: listed with every differing key (`Damage`, `Label`) as **Conflict**, all **Choose**, and not applied automatically.

**12.6 JSON written for another class**

1. In `DA_SyncItem.json`, change `"parent"` to `PDA_SyncItemAlt`'s class path
   (`/Game/Developers/<you>/JsonSyncTest/PDA_SyncItemAlt.PDA_SyncItemAlt_C`), without changing values. Refresh.

Expected:

- Listed as "0 keys, class differs" (orange), in both modes. The detail line names both classes and suggests All from Asset, then
  Apply. The Output Log has a warning naming both.
- **Apply** is enabled (nothing to choose) and writes the asset's real class back.

2. Change `"parent"` again and also a value. Refresh.

Expected: listed as "class differs"; nothing is applied automatically.

**12.7 Class defaults changed**

1. Set `Damage` in `PDA_SyncItem` Class Defaults to the value `DA_SyncItem` overrides. Compile and save. Close the editor and run
   `-verify`.

Expected: nothing is listed in the editor (the `.uasset` of `DA_SyncItem` did not change), but `-verify` fails for
`DA_SyncItem.json`: its `Damage` is no longer an override. Saving `DA_SyncItem` removes the key. Set the default back.

**12.8 Cache from an older plugin version**

1. Close the editor. In `cache.json`, delete the `"asset"` field of every entry. Start.

Expected: nothing is listed, and `cache.json` has the `"asset"` fields again.

## 13. Notifications for problems

**13.1 Unreadable cache**

1. Close the editor. Replace the contents of `Saved/JsonDataAssetSync/cache.json` with `not json`. Start.

Expected: an error toast "JSON Sync: cannot read …cache.json, so the last sync is unknown…". Assets whose JSON differs from the
asset are listed with every key as Conflict. `cache.json` is valid again afterwards.

**13.2 Read-only cache**

1. Mark `cache.json` read-only (Properties > Read-only). Change `Damage` on `DA_SyncItem` and save.

Expected: the JSON is written, and an error toast "JSON Sync: cannot write …cache.json. Is it read-only?…".

2. Change it again and save.

Expected: no new toast (only a log warning).

3. Clear the read-only flag, change it again and save.

Expected: no toast; `cache.json` is updated. Making it read-only again shows the toast again on the next save.

**13.3 Asset that cannot be read**

1. Close the editor. Copy `DA_SyncItem.json` to `DA_SyncBroken.json`, set its `"asset"` to the matching package
   (`.../JsonSyncTest/DA_SyncBroken`), and create an empty file `DA_SyncBroken.uasset` in the matching content folder. Start.

Expected: the asset registry cannot read the empty package, so the file is reported as a JSON without its data asset (7.1). If the
registry does list it and loading fails, the error toast "JSON Sync: 1 JSON files could not be checked" appears instead. Report
what you see. Delete both files afterwards.

**13.4 Rename with an unreadable JSON**

1. Put `<<<<<<< HEAD` on the first line of `DA_SyncItem.json`. Rename `DA_SyncItem` to `DA_SyncItem2` in the Content Browser.

Expected: a warning toast "renamed DA_SyncItem to DA_SyncItem2, but its JSON cannot be read (…), so it was not moved". The old file
is still there and is reported as a JSON without its data asset at the next Refresh. Saving `DA_SyncItem2` writes
`DA_SyncItem2.json`. Delete the old file and rename the asset back.

**13.5 JSON that cannot be deleted**

1. Duplicate `DA_SyncItem` and save the copy. In PowerShell, hold its JSON open:
   `$f = [IO.File]::Open("<path to the copy's JSON>", 'Open', 'Read', 'None')`
2. Delete the copy in the Content Browser.

Expected: an error toast "JSON Sync: cannot delete … Is it open in another program?". Run `$f.Close()` and delete the file by hand.

## 14. Cleanup

1. Delete the `JsonSyncTest` folder in the Content Browser.

Expected: its JSON files are deleted as well.

2. Delete any leftover test JSON files or folders (`DA_Missing.json`, `JsonDataAssetSyncTest/`, `Saved/JsonExportTest/`). Restore
   Project Settings (Mode Automatic, no Excluded Folders) and `Config/DefaultEditor.ini`. Delete the `.uasset` copies from
   sections 5 and 12.
