// TortoiseGit diff tool for .uasset and .umap files: shows the UnrealAssetJson merge text of both
// versions in TortoiseGitMerge. TortoiseGit's diff doesn't use git's textconv, so this runs
// uasset-textconv.sh on each file and opens TortoiseGitMerge on the two texts.
//
// Setup: TortoiseGit > Settings > Diff Viewer > Advanced, add .uasset and .umap with
//   wscript.exe "<path to>\Tools\uasset-diff.js" %base %mine %bname %yname //E:javascript
// and the git config from the plugin README (unrealassetjson.path).
//
// Runs under wscript, so no console window opens. The texts go to a temporary folder, which is
// deleted when TortoiseGitMerge closes.

var Shell = new ActiveXObject("WScript.Shell");
var Fso = new ActiveXObject("Scripting.FileSystemObject");

function Fail(Message)
{
	Shell.Popup(Message, 0, "uasset-diff", 16);
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

if (WScript.Arguments.length < 2)
{
	Fail("Usage: uasset-diff.js <base> <mine> [<base name> <mine name>]");
}
var Base = WScript.Arguments(0);
var Mine = WScript.Arguments(1);
var BaseName = WScript.Arguments.length > 2 ? WScript.Arguments(2) : Fso.GetFileName(Base);
var MineName = WScript.Arguments.length > 3 ? WScript.Arguments(3) : Fso.GetFileName(Mine);

var Tools = Fso.GetParentFolderName(WScript.ScriptFullName);
var GitDir = RegRead("HKLM\\SOFTWARE\\GitForWindows\\InstallPath", "C:\\Program Files\\Git");
var Sh = GitDir + "\\bin\\sh.exe";
var Merge = RegRead("HKLM\\SOFTWARE\\TortoiseGit\\TMergePath", "C:\\Program Files\\TortoiseGit\\bin\\TortoiseGitMerge.exe");
if (!Fso.FileExists(Sh))
{
	Fail("Git for Windows not found: " + Sh);
}
if (!Fso.FileExists(Merge))
{
	Fail("TortoiseGitMerge not found: " + Merge);
}

// Run inside the repository that holds the plugin, so git config and Git LFS find their settings.
// The outermost folder with a .git folder: a submodule has a .git file, its superproject a folder.
var Repository = null;
for (var Folder = Tools; Folder; Folder = Fso.GetParentFolderName(Folder))
{
	if (Fso.FolderExists(Folder + "\\.git"))
	{
		Repository = Folder;
	}
}
if (!Repository)
{
	Fail("No git repository above " + Tools);
}
Shell.CurrentDirectory = Repository;

var Temp = Fso.GetSpecialFolder(2) + "\\uasset-diff-" + Fso.GetTempName().replace(/\W/g, "");
Fso.CreateFolder(Temp);
var BaseText = Temp + "\\base.txt";
var MineText = Temp + "\\mine.txt";
var TextConv = (Tools + "\\uasset-textconv.sh").replace(/\\/g, "/");
var ErrorLog = Temp + "\\error.txt";

function Convert(File, Text)
{
	// An added or deleted file comes as an empty file: its text is empty.
	if (Fso.FileExists(File) && Fso.GetFile(File).Size == 0)
	{
		Fso.CreateTextFile(Text).Close();
		return;
	}
	// sh -c "sh '<textconv>' '<file>' '<text>' 2>'<error log>'", with forward slashes.
	var Command = "sh " + SingleQuote(TextConv) + " " + SingleQuote(File) + " " + SingleQuote(Text) + " 2>" + SingleQuote(ErrorLog);
	var Exit = Shell.Run(Quote(Sh) + " -c " + Quote(Command), 0, true);
	if (Exit != 0)
	{
		var Message = Fso.FileExists(ErrorLog) && Fso.GetFile(ErrorLog).Size > 0 ? Fso.OpenTextFile(ErrorLog).ReadAll() : "exit code " + Exit;
		try { Fso.DeleteFolder(Temp, true); } catch (e) {}
		Fail("uasset-textconv.sh failed on " + File + ":\n" + Message);
	}
}
Convert(Base, BaseText);
Convert(Mine, MineText);

Shell.Run(Quote(Merge) + " /base:" + Quote(BaseText) + " /mine:" + Quote(MineText)
	+ " /basename:" + Quote(BaseName) + " /minename:" + Quote(MineName) + " /readonly", 1, true);
try { Fso.DeleteFolder(Temp, true); } catch (e) {}
