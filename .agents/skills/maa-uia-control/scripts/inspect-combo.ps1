param(
    [Parameter(Mandatory = $true)][int]$Index
)

. "$PSScriptRoot/lib.ps1"

$out = New-Object System.Collections.ArrayList
$win = Get-MaaWindow
$cond = New-Object System.Windows.Automation.PropertyCondition($global:AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::ComboBox)
$combos = $win.FindAll($global:TS::Descendants, $cond)
[void]$out.Add("total combos: $($combos.Count)")
if ($Index -lt 0 -or $Index -ge $combos.Count) {
    throw "Index $Index out of range -- there are $($combos.Count) combo(s) (0..$($combos.Count - 1)); run dump-combos.ps1 first"
}
$combo = $combos.Item($Index)
[void]$out.Add("name='$($combo.Current.Name)' aid='$($combo.Current.AutomationId)'")
foreach ($p in $combo.GetSupportedPatterns()) {
    [void]$out.Add("pattern: $($p.ProgrammaticName)")
}
$walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
$child = $walker.GetFirstChild($combo)
while ($child) {
    $c = $child.Current
    [void]$out.Add("  child: type=$($c.ControlType.ProgrammaticName) name='$($c.Name)'")
    $child = $walker.GetNextSibling($child)
}

Report 'inspect-combo' $out
