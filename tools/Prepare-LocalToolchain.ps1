param()
$ErrorActionPreference = 'Stop'
$dmProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$dmToolRoot = Join-Path $dmProjectRoot '.tools'
$dmLock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'toolchain-lock.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($dmLock.schema_version -ne 1 -or $dmLock.msvc_directory_version -notmatch '^\d+\.\d+\.\d+$' -or $dmLock.sdk_directory_version -notmatch '^\d+\.\d+\.\d+\.\d+$') {
    throw 'Unsupported toolchain lock configuration.'
}
function Resolve-LocalToolPath([string]$RelativePath) {
    $absolute = [IO.Path]::GetFullPath((Join-Path $dmToolRoot $RelativePath))
    if (!$absolute.StartsWith($dmToolRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Toolchain path escapes project .tools.' }
    $absolute
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Directory]::CreateDirectory((Join-Path $dmToolRoot 'cache')) | Out-Null
[IO.Directory]::CreateDirectory((Join-Path $dmToolRoot 'state')) | Out-Null
[IO.Directory]::CreateDirectory((Join-Path $dmToolRoot 'tmp')) | Out-Null
foreach ($dmArtifact in $dmLock.artifacts) {
    $dmArchive = Resolve-LocalToolPath ('cache/' + $dmArtifact.file)
    if (![IO.File]::Exists($dmArchive)) {
        Write-Output ('Downloading ' + $dmArtifact.id)
        Invoke-WebRequest -UseBasicParsing -Uri $dmArtifact.url -OutFile $dmArchive
    }
    $dmHash = (Get-FileHash -LiteralPath $dmArchive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($dmHash -ne $dmArtifact.sha256) { throw ('Archive hash mismatch: ' + $dmArtifact.file) }
    $dmState = Resolve-LocalToolPath ('state/' + $dmArtifact.file + '.sha256')
    if ([IO.File]::Exists($dmState) -and [IO.File]::ReadAllText($dmState).Trim() -eq $dmHash) {
        Write-Output ('Ready ' + $dmArtifact.id)
        continue
    }
    $dmDestination = [IO.Path]::GetFullPath((Join-Path $dmToolRoot $dmArtifact.destination))
    $dmBoundary = $dmDestination.TrimEnd('\') + '\'
    if (!$dmBoundary.StartsWith($dmToolRoot.TrimEnd('\') + '\',[StringComparison]::OrdinalIgnoreCase)) {
        throw 'Tool destination must stay inside the project .tools directory'
    }
    [IO.Directory]::CreateDirectory($dmDestination) | Out-Null
    $dmZip = [IO.Compression.ZipFile]::OpenRead($dmArchive)
    try {
        $dmCount = 0
        foreach ($dmEntry in $dmZip.Entries) {
            if ($dmEntry.FullName.EndsWith('/')) { continue }
            if ($dmArtifact.prefix -and !$dmEntry.FullName.StartsWith($dmArtifact.prefix,[StringComparison]::OrdinalIgnoreCase)) { continue }
            $dmRelative = $dmEntry.FullName.Substring($dmArtifact.prefix.Length)
            if (!$dmRelative) { continue }
            if ($dmArtifact.id -eq 'Microsoft.Windows.SDK.CPP' -and
                $dmRelative -notmatch '^(Include/10\.0\.26100\.0/(shared|um|ucrt|winrt)/|bin/10\.0\.26100\.0/x64/)') { continue }
            if ($dmArtifact.id -eq 'Microsoft.Windows.SDK.CPP.x64' -and
                $dmRelative -notmatch '^(um|ucrt)/x64/') { continue }
            $dmOutput = [IO.Path]::GetFullPath((Join-Path $dmDestination $dmRelative.Replace('/','\')))
            if (!$dmOutput.StartsWith($dmBoundary,[StringComparison]::OrdinalIgnoreCase)) { throw 'Archive entry escapes its project destination' }
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($dmOutput)) | Out-Null
            $dmInputStream = $dmEntry.Open()
            $dmOutputStream = [IO.File]::Create($dmOutput)
            try { $dmInputStream.CopyTo($dmOutputStream) }
            finally { $dmOutputStream.Dispose(); $dmInputStream.Dispose() }
            $dmCount++
        }
        Write-Output ('Extracted ' + $dmArtifact.id + ': ' + $dmCount + ' files')
    } finally { $dmZip.Dispose() }
    [IO.File]::WriteAllText($dmState,$dmHash,(New-Object Text.UTF8Encoding($false)))
}
$dmClRoot = Join-Path $dmToolRoot ('msvc\VC\Tools\MSVC\' + $dmLock.msvc_directory_version)
$dmClBin = Join-Path $dmClRoot 'bin\Hostx64\x64'
# 编译器依赖的 VC 运行库只进行应用本地复制，不运行 vcredist。
$dmRedist = Get-ChildItem -LiteralPath (Join-Path $dmToolRoot 'msvc\VC\Redist\MSVC') -Recurse -File |
    Where-Object { $_.DirectoryName -match '\\x64\\Microsoft\.VC[0-9]+\.CRT$' -and $_.Extension -eq '.dll' }
foreach ($dmDll in $dmRedist) {
    $dmDestination=Join-Path $dmClBin $dmDll.Name
    if([IO.File]::Exists($dmDestination) -and
       (Get-FileHash -LiteralPath $dmDll.FullName -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath $dmDestination -Algorithm SHA256).Hash){continue}
    [IO.File]::Copy($dmDll.FullName,$dmDestination,$true)
}
$dmConfiguration = [ordered]@{
    schema_version=1
    cmake='cmake/cmake-4.4.4-windows-x86_64/bin/cmake.exe'
    ninja='ninja/ninja.exe'
    msvc_root=('msvc/VC/Tools/MSVC/' + $dmLock.msvc_directory_version)
    sdk_root='sdk'
    sdk_lib_root='sdk-libs-x64'
    sdk_version=$dmLock.sdk_directory_version
    architecture='x64'
}
$dmConfiguration | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $dmToolRoot 'toolchain.json') -Encoding UTF8
Write-Output 'Project-local x64 toolchain prepared; no installer, registry or persistent environment changes.'
