. "$PSScriptRoot/lib.ps1"

$win = Get-MaaWindow
$walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
$out = New-Object System.Collections.ArrayList

function Walk($el, $depth) {
    if ($depth -gt 20) { return }
    $c = $el.Current
    $type = $c.ControlType.ProgrammaticName -replace 'ControlType\.', ''
    if ($type -eq 'ComboBox' -or $type -eq 'ComboBoxItem' -or $type -eq 'ListItem') {
        $name = $c.Name
        $aid = $c.AutomationId
        $off = if ($c.IsOffscreen) { ' [offscreen]' } else { '' }
        $pad = ' ' * ($depth * 2)
        [void]$script:out.Add("$pad$type | name='$name' | aid='$aid'$off")
    }
    $child = $walker.GetFirstChild($el)
    while ($child) {
        Walk $child ($depth + 1)
        $child = $walker.GetNextSibling($child)
    }
}

Walk $win 0

Report 'dump-combos' $out
