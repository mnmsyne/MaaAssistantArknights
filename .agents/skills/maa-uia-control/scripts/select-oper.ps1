. "$PSScriptRoot\lib.ps1"

$target = $args[0]
if (-not $target) { throw 'usage: select-oper.ps1 <operator-name>' }
$L = Get-Labels

$out = New-Object System.Collections.ArrayList
$win = Get-MaaWindow
Assert-NoDialog $win

# The operator ComboBox is the parent of PART_EditableTextBox
$edit = (Find-ByAid $win 'PART_EditableTextBox')[0]
if (-not $edit) { throw 'operator edit box not found' }
$walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
$combo = $walker.GetParent($edit)
[void]$out.Add("combo type=$($combo.Current.ControlType.ProgrammaticName) name='$($combo.Current.Name)'")

$listCond = New-Object System.Windows.Automation.PropertyCondition(
    $global:AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem)
$exp = $combo.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)

# The search filters on an operator's base name, not its full display name -- an alter-ego's display
# name is "<alter-title><base-name>" and the game's search does not match the alter-title prefix.
# Typing the full display name of an alter-ego therefore matches nothing. Retry with progressively
# shorter suffixes of the query (drop one leading character at a time) until either a dropdown item's
# full Name matches the ORIGINAL target exactly, or the remaining query is too short to be a
# meaningful search.
$match = $null
$query = $target
$minQueryLen = 2
while (-not $match -and $query.Length -ge $minQueryLen) {
    Set-EditValue $edit $query
    Start-Sleep -Milliseconds 800
    $cur = $edit.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value
    [void]$out.Add("after SetValue('$query'): edit='$cur'")

    $exp.Expand()
    Start-Sleep -Milliseconds 800

    $items = $combo.FindAll($global:TS::Descendants, $listCond)
    [void]$out.Add("  dropdown items=$($items.Count)")
    for ($i = 0; $i -lt $items.Count; $i++) {
        $n = $items[$i].Current.Name
        if ($i -lt 12) { [void]$out.Add("    item[$i]='$n'") }
        if ($n -eq $target -or $n -match "Name\s*=\s*$([regex]::Escape($target))\s*[,}]") { $match = $items[$i] }
    }

    if (-not $match) {
        try { $exp.Collapse() } catch { }
        Start-Sleep -Milliseconds 300
        $query = $query.Substring(1)
    }
}

if ($match) {
    Select-Element $match
    [void]$out.Add("selected exact match '$target' (query used: '$query')")
} else {
    [void]$out.Add("NO exact match for '$target' (tried full name and all suffixes down to $minQueryLen chars)")
}
Start-Sleep -Milliseconds 600
try { $exp.Collapse() } catch { }
Start-Sleep -Milliseconds 600

# 3. report resulting enablement (proves SelectedOperator bound)
foreach ($n in $L.skillCheckboxes) {
    $cb = Find-ByName $win $n ([System.Windows.Automation.ControlType]::CheckBox)
    if ($cb) {
        $st = $cb.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState
        [void]$out.Add("checkbox '$n' state=$st enabled=$($cb.Current.IsEnabled)")
    }
}

Report 'select-oper' $out
