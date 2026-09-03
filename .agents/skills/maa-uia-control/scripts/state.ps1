. "$PSScriptRoot/lib.ps1"

$L = Get-Labels
$win = Get-MaaWindow
$out = New-Object System.Collections.ArrayList
[void]$out.Add("window: $($win.Current.Name)")

# Dialog state comes first: everything below is meaningless if a modal is swallowing input.
$dlgs = Get-MaaDialogs $win
[void]$out.Add("open dialogs: $($dlgs.Count)")
foreach ($d in $dlgs) { [void]$out.Add("  $(Format-Dialog $d)") }

# operdevelop tab selection state
$tab = Find-ByName $win $L.operDevelopTab ([System.Windows.Automation.ControlType]::TabItem)
if ($tab) {
    $sel = $tab.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected
    [void]$out.Add("operdevelop tab selected=$sel")
} else {
    [void]$out.Add('operdevelop tab NOT FOUND (run navigate.ps1)')
}

# operator autocomplete textbox
$edits = Find-ByAid $win 'PART_EditableTextBox'
[void]$out.Add("PART_EditableTextBox count=$($edits.Count)")
for ($i = 0; $i -lt $edits.Count; $i++) {
    $e = $edits[$i]
    $v = ''
    try { $v = $e.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value } catch { $v = '<no ValuePattern>' }
    $r = $e.Current.BoundingRectangle
    [void]$out.Add("  [$i] value='$v' offscreen=$($e.Current.IsOffscreen) rect=$($r.X),$($r.Y),$($r.Width),$($r.Height)")
}

foreach ($n in $L.skillCheckboxes) {
    $cb = Find-ByName $win $n ([System.Windows.Automation.ControlType]::CheckBox)
    if ($cb) {
        $st = $cb.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState
        [void]$out.Add("checkbox '$n' state=$st enabled=$($cb.Current.IsEnabled) offscreen=$($cb.Current.IsOffscreen)")
    } else {
        [void]$out.Add("checkbox '$n' NOT FOUND")
    }
}

foreach ($n in @($L.buttons.preflight, $L.buttons.start, $L.buttons.stop)) {
    $b = Find-ByName $win $n ([System.Windows.Automation.ControlType]::Button)
    if ($b) {
        [void]$out.Add("button '$n' enabled=$($b.Current.IsEnabled) offscreen=$($b.Current.IsOffscreen)")
    } else {
        [void]$out.Add("button '$n' NOT FOUND")
    }
}

Report 'state' $out
