[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_OS -ne 'Windows') {
    throw 'Run this script on a GitHub Actions Windows runner with Visual Studio 2022 and CMake.'
}

$dmProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$dmCmakeText = [IO.File]::ReadAllText((Join-Path $dmProjectRoot 'CMakeLists.txt'))
$dmVersionMatch = [regex]::Match($dmCmakeText, 'project\(DMAgent VERSION (\d+\.\d+\.\d+)\b')
if (!$dmVersionMatch.Success) { throw 'Cannot read the source version.' }
$dmVersion = $dmVersionMatch.Groups[1].Value
if ($env:GITHUB_REF_TYPE -eq 'tag' -and $env:GITHUB_REF_NAME -ne ('v' + $dmVersion)) {
    throw 'Release tag must match the version in CMakeLists.txt.'
}
if ($env:GITHUB_REF_TYPE -eq 'tag' -and !(Test-Path -LiteralPath (Join-Path $dmProjectRoot ('docs/releases/v' + $dmVersion + '.md')))) {
    throw 'Release notes are required for this version.'
}

$dmBuildRoot = Join-Path $dmProjectRoot 'out/hosted-x64'
$dmPortableRoot = Join-Path $dmProjectRoot 'dist/DM-Agent-x64'
$dmReleaseRoot = Join-Path $dmProjectRoot 'dist/release'
function Invoke-HostedCMake([string[]]$ToolArguments) {
    & cmake @ToolArguments
    if ($LASTEXITCODE -ne 0) { throw "CMake failed with exit code $LASTEXITCODE." }
}

& (Join-Path $dmProjectRoot 'viewer/Build-Viewer.ps1')
Invoke-HostedCMake -ToolArguments @('-S', $dmProjectRoot, '-B', $dmBuildRoot, '-G', 'Visual Studio 17 2022', '-A', 'x64')
Invoke-HostedCMake -ToolArguments @('--build', $dmBuildRoot, '--config', 'Release', '--parallel', '2')
Invoke-HostedCMake -ToolArguments @('--install', $dmBuildRoot, '--config', 'Release', '--prefix', $dmPortableRoot)

$dmExecutable = Join-Path $dmPortableRoot 'DmTelemetryAgent.exe'
$dmReader = [IO.BinaryReader]::new([IO.File]::OpenRead($dmExecutable))
try {
    if ($dmReader.ReadUInt16() -ne 0x5A4D) { throw 'Missing executable DOS header.' }
    $dmReader.BaseStream.Position = 0x3C
    $dmPeOffset = $dmReader.ReadInt32()
    $dmReader.BaseStream.Position = $dmPeOffset
    if ($dmReader.ReadUInt32() -ne 0x00004550 -or $dmReader.ReadUInt16() -ne 0x8664) {
        throw 'The release executable must be a Windows x64 PE file.'
    }
} finally { $dmReader.Dispose() }
$dmFileVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($dmExecutable)
if ($dmFileVersion.FileVersion -ne $dmVersion) { throw 'Executable file version does not match the source version.' }

$dmBuildInfo = [ordered]@{
    product = 'SegotepDM'
    version = $dmVersion
    source_commit = $env:GITHUB_SHA
    build_url = "$env:GITHUB_SERVER_URL/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID"
    runner_image = $env:ImageOS
    runner_image_version = $env:ImageVersion
    generator = 'Visual Studio 17 2022'
    architecture = 'x64'
    configuration = 'Release'
    verification = 'Compilation, package contents, executable architecture and version.'
}
[IO.File]::WriteAllText((Join-Path $dmPortableRoot 'BUILD-INFO.json'), ($dmBuildInfo | ConvertTo-Json) + "`n", [Text.UTF8Encoding]::new($false))
$dmRequired = @(
    'DmTelemetryAgent.exe', 'README.txt', 'LICENSE', 'BUILD-INFO.json',
    'docs/USAGE.md', 'docs/IPC.md', 'docs/ENERGY.md', 'docs/PROTOCOL.md', 'docs/THIRD_PARTY.md',
    'licenses/nlohmann-json-LICENSE.MIT', 'viewer/DM-Record-Viewer.html', 'viewer/USAGE.md', 'viewer/build-manifest.json'
)
foreach ($dmRelative in $dmRequired) {
    if (!(Test-Path -LiteralPath (Join-Path $dmPortableRoot $dmRelative) -PathType Leaf)) {
        throw "Missing release file: $dmRelative"
    }
}

$null = New-Item -ItemType Directory -Path $dmReleaseRoot -Force
$dmArchiveName = 'SegotepDM-v' + $dmVersion + '-windows-x64.zip'
$dmArchivePath = Join-Path $dmReleaseRoot $dmArchiveName
Compress-Archive -LiteralPath $dmPortableRoot -DestinationPath $dmArchivePath -CompressionLevel Optimal -Force
$dmArchive = [IO.Compression.ZipFile]::OpenRead($dmArchivePath)
try {
    $dmEntryNames = @($dmArchive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
    foreach ($dmRelative in $dmRequired) {
        if ($dmEntryNames -notcontains ('DM-Agent-x64/' + $dmRelative)) { throw "Missing ZIP entry: $dmRelative" }
    }
} finally { $dmArchive.Dispose() }
$dmHash = (Get-FileHash -LiteralPath $dmArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $dmReleaseRoot 'SHA256SUMS.txt'), "$dmHash  $dmArchiveName`n", [Text.UTF8Encoding]::new($false))
"version=$dmVersion" | Out-File -LiteralPath $env:GITHUB_OUTPUT -Encoding utf8 -Append
Write-Output "Release package: $dmArchivePath"
