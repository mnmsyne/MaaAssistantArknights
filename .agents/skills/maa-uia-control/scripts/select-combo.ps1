param(
    [Parameter(Mandatory = $true)][int]$Index,
    # Substring matched against the item's Name. For an ItemsSource-bound ComboBox the Name is the
    # bound object's ToString(), NOT the display text -- e.g. the training-mode items expose
    # "TrainingModeOption { Display = ..., Value = efficiency }". Match on a stable fragment of that
    # (e.g. "= efficiency }") rather than on the visible label.
    [Parameter(Mandatory = $true)][string]$Contains
)
. "$PSScriptRoot/lib.ps1"

$win = Get-MaaWindow
Assert-NoDialog $win

$combos = Find-AllByType $win $global:CT::ComboBox
if ($Index -ge $combos.Count) { throw "combo index $Index out of range (found $($combos.Count)); run dump-combos.ps1" }
$combo = $combos.Item($Index)
$out = @("combo[$Index] name='$($combo.Current.Name)' aid='$($combo.Current.AutomationId)'")

$expand = $combo.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
$expand.Expand()
Start-Sleep -Milliseconds 500

# Items are ListItem for ItemsSource-bound combos and ComboBoxItem for statically-declared ones.
# Searching only for ComboBoxItem is why the training-mode combo previously threw "NotSupported
# pattern" -- the search matched the wrong element and the Select() went to something else.
$items = New-Object System.Collections.ArrayList
foreach ($t in @($global:CT::ListItem, $global:CT::ComboBoxItem)) {
    foreach ($it in Find-AllByType $combo $t) { [void]$items.Add($it) }
}
$out += "items: $($items.Count)"

$match = $null
foreach ($it in $items) {
    if ($it.Current.Name -and $it.Current.Name.Contains($Contains)) { $match = $it; break }
}
if (-not $match) {
    try { $expand.Collapse() } catch { }
    $names = ($items | ForEach-Object { "'$($_.Current.Name)'" }) -join ' | '
    throw "no item containing '$Contains' among: $names"
}

Select-Element $match
Start-Sleep -Milliseconds 400
try { $expand.Collapse() } catch { }
Start-Sleep -Milliseconds 400

# Verify rather than assume. Select() can silently no-op when the item was virtualized away by the
# collapse, and the old script reported success unconditionally.
$combo = (Find-AllByType $win $global:CT::ComboBox).Item($Index)
$sel = ''
try {
    $selPat = $combo.GetCurrentPattern([System.Windows.Automation.SelectionPattern]::Pattern)
    $sel = ($selPat.Current.GetSelection() | ForEach-Object { $_.Current.Name }) -join ' | '
} catch {
    try { $sel = $combo.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value } catch { $sel = '<unreadable>' }
}
$out += "selection after: '$sel'"
if ($sel -and -not $sel.Contains($Contains)) {
    $out += "WARNING: selection does not contain '$Contains' -- the change did NOT take"
}
Report 'select-combo' $out
