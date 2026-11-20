$ErrorActionPreference = 'Stop'
$locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $locator -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\Current\Bin\MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { throw 'MSBuild or the C++ desktop workload was not found.' }
$out = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build\x64'))
& $msbuild "$PSScriptRoot\foo_walkman_sync.vcxproj" /m:4 /nologo /v:minimal /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 "/p:OutDir=$out/" /p:CL_MPCount=4
if ($LASTEXITCODE) { throw "Build failed ($LASTEXITCODE)" }
$package = Join-Path $out 'foo_walkman_sync.fb2k-component'
Add-Type -AssemblyName System.IO.Compression
$zip = [IO.Compression.ZipArchive]::new([IO.File]::Create($package), [IO.Compression.ZipArchiveMode]::Create)
try {
    $payload = [ordered]@{
        'x64/foo_walkman_sync.dll' = (Join-Path $out 'foo_walkman_sync.dll')
        'x64/foobox/install-foobox.ps1' = (Join-Path $PSScriptRoot 'install-foobox.ps1')
        'x64/foobox/foobox-menu.js' = (Join-Path $PSScriptRoot 'foobox-menu.js')
    }
    foreach ($name in $payload.Keys) {
        $entry = $zip.CreateEntry($name)
        $stream = $entry.Open()
        $inputFile = [IO.File]::OpenRead($payload[$name])
        try { $inputFile.CopyTo($stream) } finally { $inputFile.Dispose(); $stream.Dispose() }
    }
} finally { $zip.Dispose() }
Write-Output $package
