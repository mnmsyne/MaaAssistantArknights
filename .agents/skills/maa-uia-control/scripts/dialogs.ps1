. "$PSScriptRoot/lib.ps1"

# Lists modal dialogs currently open inside MAA's window. Run this whenever a click "did nothing":
# a HandyControl MessageBox is a nested child Window, so it never shows up in the OS window list.
$win = Get-MaaWindow
$dlgs = Get-MaaDialogs $win

$out = @("open dialogs: $($dlgs.Count)")
for ($i = 0; $i -lt $dlgs.Count; $i++) {
    $out += "  [$i] $(Format-Dialog $dlgs[$i])"
}
if ($dlgs.Count -eq 0) {
    $out += '  (none -- the app is free to accept actions)'
} else {
    $out += 'answer them with: click-dialog.ps1 -Choice Yes|No [-All]'
}
Report 'dialogs' $out
