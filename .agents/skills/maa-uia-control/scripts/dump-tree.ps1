. "$PSScriptRoot/lib.ps1"

$out = New-Object System.Collections.ArrayList
$win = Get-MaaWindow
[void]$out.Add("window: name='$($win.Current.Name)' class='$($win.Current.ClassName)'")
[void]$out.Add('')

# Walk the tree, printing interactive/named elements
$walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker

function Walk($el, $depth) {
    if ($depth -gt 14) { return }
    $c = $el.Current
    $type = $c.ControlType.ProgrammaticName -replace 'ControlType\.', ''
    $name = $c.Name
    if ($name.Length -gt 60) { $name = $name.Substring(0, 60) + '...' }
    $aid = $c.AutomationId
    # only print rows that carry identifying info
    if ($name -or $aid) {
        $pad = ' ' * ($depth * 2)
        $off = if ($c.IsOffscreen) { ' [offscreen]' } else { '' }
        [void]$script:out.Add("$pad$type | name='$name' | aid='$aid'$off")
    }
    $child = $walker.GetFirstChild($el)
    while ($child) {
        Walk $child ($depth + 1)
        $child = $walker.GetNextSibling($child)
    }
}

Walk $win 0

Report 'dump-tree' $out
