param([Parameter(Mandatory=$true)][string]$ProfilePath)
$ErrorActionPreference = 'Stop'
$panel = Join-Path $ProfilePath 'foobox\script\js_panels\jsspm.js'
$text = [IO.File]::ReadAllText($panel)
$folder = Join-Path $ProfilePath 'walkman-sync'
[IO.Directory]::CreateDirectory($folder) | Out-Null
$include = "include(fb.ProfilePath + 'walkman-sync\\foobox-menu.js');"
$updated = $text
if (!$text.Contains($include)) {
    $row = 'var pl_idx = this.rows[id].idx;'
    $popup = 'idx = _menu.TrackPopupMenu(x, y);'
    foreach ($anchor in @($row,$popup)) {
        if ([regex]::Matches($text,[regex]::Escape($anchor)).Count -ne 1) { throw "Unsupported foobox panel version ($anchor)" }
    }
    $firstInclude = $text.IndexOf('include(fb.ProfilePath')
    if ($firstInclude -lt 0) { throw 'Cannot locate the foobox panel includes' }
    $updated = $text.Insert($firstInclude, $include + [Environment]::NewLine).Replace($row,$row + ' var walkmanActions = walkmanMenu(_menu, pl_idx);').Replace($popup,$popup + ' if (walkmanActions && walkmanActions[idx]) { walkmanActions[idx](); return; }')
    $backup = "$panel.pre-walkman-$([DateTime]::Now.ToString('yyyyMMdd-HHmmssfff')).bak"
}
$scriptText = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'foobox-menu.js'))
$encoding = [Text.UTF8Encoding]::new($true)
[IO.File]::WriteAllText((Join-Path $folder 'foobox-menu.js'),$scriptText,$encoding)
if ($updated -ne $text) {
    $stage = "$panel.$([guid]::NewGuid()).tmp"
    [IO.File]::WriteAllText($stage,$updated,$encoding)
    [IO.File]::Replace($stage,$panel,$backup)
}
Write-Output $panel
