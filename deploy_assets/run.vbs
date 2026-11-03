Set fso = CreateObject("Scripting.FileSystemObject")
scriptDir = fso.GetParentFolderName(WScript.ScriptFullName)
Set sh = CreateObject("WScript.Shell")
sh.Run Chr(34) & scriptDir & "\deploy.bat" & Chr(34), 0, False
Set sh = Nothing
Set fso = Nothing