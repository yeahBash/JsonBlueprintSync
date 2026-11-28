// TortoiseGit merge tool for .uasset and .umap files. It doesn't merge the asset: it writes the
// UnrealAssetJson merge text of base, ours and theirs to a temporary folder and opens
// TortoiseGitMerge on them. The result goes to merged.txt in the asset's merge folder,
// <Project>/Saved/JsonBlueprintSync/Merge/<package path>/, never over the asset; the .uasset in
// the working tree stays ours. The plugin compares merged.txt with the asset in the editor and
// applies it.
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
	var Answer = Shell.Popup("A merge of " + PackagePath + " is already saved in\n" + MergedText + "\n\n"
		+ "Start over? Yes deletes merged.txt and opens the merge again. No leaves it as it is.", 0, Title, 4 + 32);
	if (Answer != 6)
	{
		WScript.Quit(1);
	}
	Fso.DeleteFile(MergedText);
}
CreateFolders(MergeFolder);

// The three texts are only needed while TortoiseGitMerge is open.
var Temp = Fso.GetSpecialFolder(2) + "\\uasset-merge-" + Fso.GetTempName().replace(/\W/g, "");
Fso.CreateFolder(Temp);
var ErrorLog = Temp + "\\error.txt";

function Cleanup()
{
	try { Fso.DeleteFolder(Temp, true); } catch (e) {}
	// TortoiseGitMerge closed without saving: leave no empty merge folder behind.
	try
	{
		var Folder = Fso.GetFolder(MergeFolder);
		if (Folder.Files.Count == 0 && Folder.SubFolders.Count == 0)
		{
			Fso.DeleteFolder(MergeFolder);
		}
	}
	catch (e) {}
}

// An added or deleted file comes as an empty file: its text is empty.
function Convert(File, Text)
{
	if (!Fso.FileExists(File))
	{
		Cleanup();
		Fail("File not found: " + File);
	}
	if (Fso.GetFile(File).Size == 0)
	{
		Fso.CreateTextFile(Text, true).Close();
		return;
	}
	var Command = "sh " + SingleQuote(Tools + "\\uasset-textconv.sh") + " " + SingleQuote(File) + " " + SingleQuote(Text);
	var Exit = Shell.Run(Quote(Sh) + " -c " + Quote(Command + " 2>" + SingleQuote(ErrorLog)), 0, true);
	if (Exit != 0)
	{
		var Message = Fso.FileExists(ErrorLog) && Fso.GetFile(ErrorLog).Size > 0 ? ReadText(ErrorLog) : "exit code " + Exit;
		Cleanup();
		Fail("uasset-textconv.sh failed on " + File + ":\n" + Message);
	}
}
Convert(Base, Temp + "\\base.txt");
Convert(Mine, Temp + "\\ours.txt");
Convert(Theirs, Temp + "\\theirs.txt");

var Exit = Shell.Run(Quote(Merge)
	+ " /base:" + Quote(Temp + "\\base.txt") + " /basename:" + Quote(BaseName)
	+ " /mine:" + Quote(Temp + "\\ours.txt") + " /minename:" + Quote(MineName)
	+ " /theirs:" + Quote(Temp + "\\theirs.txt") + " /theirsname:" + Quote(TheirsName)
	+ " /merged:" + Quote(MergedText) + " /mergedname:" + Quote(PackagePath + " (merged text)"), 1, true);
Cleanup();
WScript.Quit(Exit);
