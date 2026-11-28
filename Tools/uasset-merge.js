// TortoiseGit merge tool for .uasset and .umap files. It doesn't merge the asset: it writes the
// UnrealAssetJson merge text of base, ours and theirs to the asset's merge folder in the project's
// Saved folder and opens TortoiseGitMerge on them. The result goes to merged.txt there, never over
// the asset; the .uasset in the working tree stays ours. The plugin applies merged.txt in the editor.
//
// Merge folder: <Project>/Saved/JsonBlueprintSync/Merge/<package path>/ with base.txt, ours.txt,
// theirs.txt, merged.txt (written by TortoiseGitMerge) and info.json.
//
// Setup: TortoiseGit > Settings > Diff Viewer > Merge Tool > Advanced, add .uasset and .umap with
//   wscript.exe "<path to>\Tools\uasset-merge.js" %base %mine %theirs %merged %bname %yname %tname //E:javascript
// and the git config from the plugin README (unrealassetjson.path).
//
// Runs under wscript, so no console window opens. Errors show in a message box.

var Shell = new ActiveXObject("WScript.Shell");
var Fso = new ActiveXObject("Scripting.FileSystemObject");
var Title = "uasset-merge";

function Fail(Message)
{
	Shell.Popup(Message, 0, Title, 16);
	WScript.Quit(1);
}

function RegRead(Key, Default)
{
	try { return Shell.RegRead(Key); } catch (e) { return Default; }
}

function Quote(Text)
{
	return "\"" + Text + "\"";
}

function SingleQuote(Path)
{
	return "'" + Path.replace(/\\/g, "/").replace(/'/g, "'\\''") + "'";
}

// ASCII only, so the file reads the same in any code page.
function JsonString(Text)
{
	return "\"" + Text.replace(/[\\"]/g, "\\$&").replace(/[\x00-\x1f\x7f-\uffff]/g, function (C)
	{
		return "\\u" + ("000" + C.charCodeAt(0).toString(16)).slice(-4);
	}) + "\"";
}

function Pad(Number)
{
	return (Number < 10 ? "0" : "") + Number;
}

function CreateFolders(Folder)
{
	if (!Fso.FolderExists(Folder))
	{
		CreateFolders(Fso.GetParentFolderName(Folder));
		Fso.CreateFolder(Folder);
	}
}

function ReadText(File)
{
	var Stream = Fso.OpenTextFile(File, 1);
	var Text = Stream.AtEndOfStream ? "" : Stream.ReadAll();
	Stream.Close();
	return Text;
}

if (WScript.Arguments.length < 4)
{
	Fail("Usage: uasset-merge.js <base> <mine> <theirs> <merged> [<base name> <mine name> <theirs name>]");
}
var Base = WScript.Arguments(0);
var Mine = WScript.Arguments(1);
var Theirs = WScript.Arguments(2);
var Asset = Fso.GetAbsolutePathName(WScript.Arguments(3));
var BaseName = WScript.Arguments.length > 4 ? WScript.Arguments(4) : "Base";
var MineName = WScript.Arguments.length > 5 ? WScript.Arguments(5) : "Ours";
var TheirsName = WScript.Arguments.length > 6 ? WScript.Arguments(6) : "Theirs";

var Tools = Fso.GetParentFolderName(WScript.ScriptFullName);
var Sh = RegRead("HKLM\\SOFTWARE\\GitForWindows\\InstallPath", "C:\\Program Files\\Git") + "\\bin\\sh.exe";
var Merge = RegRead("HKLM\\SOFTWARE\\TortoiseGit\\TMergePath", "C:\\Program Files\\TortoiseGit\\bin\\TortoiseGitMerge.exe");
if (!Fso.FileExists(Sh))
{
	Fail("Git for Windows not found: " + Sh);
}
if (!Fso.FileExists(Merge))
{
	Fail("TortoiseGitMerge not found: " + Merge);
}

// The project is the nearest folder above the asset with a .uproject file.
var Project = null;
for (var Folder = Fso.GetParentFolderName(Asset); Folder && !Project; Folder = Fso.GetParentFolderName(Folder))
{
	for (var Files = new Enumerator(Fso.GetFolder(Folder).Files); !Files.atEnd(); Files.moveNext())
	{
		if (/\.uproject$/i.test(Files.item().Name))
		{
			Project = Folder;
		}
	}
}
if (!Project)
{
	Fail("No .uproject above " + Asset);
}

// The package path: /Game/ for the project's Content folder, /<Plugin>/ for a plugin's.
var PackagePath = null;
var Below = Fso.GetBaseName(Asset);
for (var Folder = Fso.GetParentFolderName(Asset); Folder && Folder.length > Project.length; Folder = Fso.GetParentFolderName(Folder))
{
	var Name = Fso.GetFileName(Folder);
	var Parent = Fso.GetParentFolderName(Folder);
	if (Name.toLowerCase() == "content")
	{
		if (Parent.toLowerCase() == Project.toLowerCase())
		{
			PackagePath = "/Game/" + Below;
			break;
		}
		for (var Files = new Enumerator(Fso.GetFolder(Parent).Files); !Files.atEnd(); Files.moveNext())
		{
			if (/\.uplugin$/i.test(Files.item().Name))
			{
				PackagePath = "/" + Fso.GetBaseName(Files.item().Name) + "/" + Below;
			}
		}
		if (PackagePath)
		{
			break;
		}
	}
	Below = Name + "/" + Below;
}
if (!PackagePath)
{
	Fail("Not in a Content folder of the project or of a plugin: " + Asset);
}

// Run inside the repository, so git config and Git LFS find their settings. The outermost folder
// with a .git folder: a submodule has a .git file, its superproject a folder.
var Repository = null;
for (var Folder = Fso.GetParentFolderName(Asset); Folder; Folder = Fso.GetParentFolderName(Folder))
{
	if (Fso.FolderExists(Folder + "\\.git"))
	{
		Repository = Folder;
	}
}
if (!Repository)
{
	Fail("No git repository above " + Asset);
}
Shell.CurrentDirectory = Repository;

var MergeFolder = Project + "\\Saved\\JsonBlueprintSync\\Merge" + PackagePath.replace(/\//g, "\\");
var MergedText = MergeFolder + "\\merged.txt";
if (Fso.FileExists(MergedText))
{
	var Answer = Shell.Popup("A merge of " + PackagePath + " is already saved in\n" + MergeFolder + "\n\n"
		+ "Start over? Yes writes the texts again and deletes merged.txt. No leaves the merge folder as it is.", 0, Title, 4 + 32);
	if (Answer != 6)
	{
		WScript.Quit(1);
	}
	Fso.DeleteFile(MergedText);
}
CreateFolders(MergeFolder);

var Temp = Fso.GetSpecialFolder(2) + "\\uasset-merge-" + Fso.GetTempName().replace(/\W/g, "");
Fso.CreateFolder(Temp);
var ErrorLog = Temp + "\\error.txt";

function RunSh(Command, What)
{
	var Exit = Shell.Run(Quote(Sh) + " -c " + Quote(Command + " 2>" + SingleQuote(ErrorLog)), 0, true);
	if (Exit != 0)
	{
		var Message = Fso.FileExists(ErrorLog) && Fso.GetFile(ErrorLog).Size > 0 ? ReadText(ErrorLog) : "exit code " + Exit;
		try { Fso.DeleteFolder(Temp, true); } catch (e) {}
		Fail(What + ":\n" + Message);
	}
}

// The SHA-256 of the real file. A Git LFS pointer names it, so the object isn't needed.
function Sha256(File)
{
	if (Fso.GetFile(File).Size < 1024)
	{
		var Pointer = /^version https:\/\/git-lfs\.github\.com\/spec\/v1[\s\S]*\noid sha256:([0-9a-f]{64})/.exec(ReadText(File));
		if (Pointer)
		{
			return Pointer[1];
		}
	}
	var Out = Temp + "\\sha256.txt";
	RunSh("sha256sum < " + SingleQuote(File) + " | cut -d ' ' -f 1 > " + SingleQuote(Out), "sha256sum failed on " + File);
	return ReadText(Out).replace(/\s+$/, "");
}

// An added or deleted file comes as an empty file: its text is empty.
function Convert(File, Text)
{
	if (Fso.GetFile(File).Size == 0)
	{
		Fso.CreateTextFile(Text, true).Close();
		return "";
	}
	RunSh("sh " + SingleQuote(Tools + "\\uasset-textconv.sh") + " " + SingleQuote(File) + " " + SingleQuote(Text),
		"uasset-textconv.sh failed on " + File);
	return Sha256(File);
}

var Inputs = [["base", Base, BaseName], ["ours", Mine, MineName], ["theirs", Theirs, TheirsName]];
var Hashes = {};
for (var i = 0; i < Inputs.length; i++)
{
	if (!Fso.FileExists(Inputs[i][1]))
	{
		Fail("File not found: " + Inputs[i][1]);
	}
	Hashes[Inputs[i][0]] = Convert(Inputs[i][1], MergeFolder + "\\" + Inputs[i][0] + ".txt");
}
try { Fso.DeleteFolder(Temp, true); } catch (e) {}

// The format line of the merge text, e.g. "UnrealAssetJson merge text 3".
var Format = "";
var FirstLine = /^# (.*)/.exec(ReadText(MergeFolder + "\\ours.txt"));
if (FirstLine)
{
	Format = FirstLine[1];
}

var Now = new Date();
var Stamp = Now.getUTCFullYear() + "-" + Pad(Now.getUTCMonth() + 1) + "-" + Pad(Now.getUTCDate()) + "T"
	+ Pad(Now.getUTCHours()) + ":" + Pad(Now.getUTCMinutes()) + ":" + Pad(Now.getUTCSeconds()) + "Z";
var Info = "{\n"
	+ "\t\"version\": 1,\n"
	+ "\t\"asset\": " + JsonString(PackagePath) + ",\n"
	+ "\t\"file\": " + JsonString(Asset.substr(Project.length + 1).replace(/\\/g, "/")) + ",\n"
	+ "\t\"format\": " + JsonString(Format) + ",\n"
	+ "\t\"date\": " + JsonString(Stamp) + ",\n"
	+ "\t\"sha256\": {\n";
for (var i = 0; i < Inputs.length; i++)
{
	Info += "\t\t" + JsonString(Inputs[i][0]) + ": " + JsonString(Hashes[Inputs[i][0]]) + (i + 1 < Inputs.length ? "," : "") + "\n";
}
Info += "\t},\n"
	+ "\t\"names\": {\n";
for (var i = 0; i < Inputs.length; i++)
{
	Info += "\t\t" + JsonString(Inputs[i][0]) + ": " + JsonString(Inputs[i][2]) + (i + 1 < Inputs.length ? "," : "") + "\n";
}
Info += "\t}\n}\n";
var InfoFile = Fso.CreateTextFile(MergeFolder + "\\info.json", true);
InfoFile.Write(Info);
InfoFile.Close();

var Exit = Shell.Run(Quote(Merge)
	+ " /base:" + Quote(MergeFolder + "\\base.txt") + " /basename:" + Quote(BaseName)
	+ " /mine:" + Quote(MergeFolder + "\\ours.txt") + " /minename:" + Quote(MineName)
	+ " /theirs:" + Quote(MergeFolder + "\\theirs.txt") + " /theirsname:" + Quote(TheirsName)
	+ " /merged:" + Quote(MergedText) + " /mergedname:" + Quote(PackagePath + " (merged text)"), 1, true);
WScript.Quit(Exit);
