[CmdletBinding()]
param([switch]$SkipInstall)

$ErrorActionPreference = 'Stop'
$dmProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$dmTools = Join-Path $dmProjectRoot '.tools'
$dmBoundary = $dmTools.TrimEnd('\') + '\'
$dmConfiguration = Join-Path $dmTools 'toolchain.json'
if (!(Test-Path -LiteralPath $dmConfiguration -PathType Leaf)) {
    throw 'Missing project-local toolchain. Run tools/Prepare-LocalToolchain.ps1 first.'
}
$dmToolchain = Get-Content -LiteralPath $dmConfiguration -Raw -Encoding UTF8 | ConvertFrom-Json
if ($dmToolchain.schema_version -ne 1 -or $dmToolchain.architecture -ne 'x64' -or $dmToolchain.sdk_version -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw 'Unsupported project-local toolchain configuration.' }
function Resolve-ToolPath([string]$RelativePath) {
    if ([string]::IsNullOrWhiteSpace($RelativePath)) { throw 'Empty tool path.' }
    $absolute = [IO.Path]::GetFullPath((Join-Path $dmTools $RelativePath))
    if (!$absolute.StartsWith($dmBoundary, [StringComparison]::OrdinalIgnoreCase)) { throw 'Tool paths must stay within the project .tools directory.' }
    $absolute
}
$dmCmake = Resolve-ToolPath $dmToolchain.cmake
$dmNinja = Resolve-ToolPath $dmToolchain.ninja
$dmVc = Resolve-ToolPath $dmToolchain.msvc_root
$dmSdk = Resolve-ToolPath $dmToolchain.sdk_root
$dmSdkLib = Resolve-ToolPath $dmToolchain.sdk_lib_root
$dmBin = Join-Path $dmVc 'bin/Hostx64/x64'
$dmSdkBin = Join-Path $dmSdk ('bin/' + $dmToolchain.sdk_version + '/x64')
foreach ($dmTool in @($dmCmake, $dmNinja, (Join-Path $dmBin 'cl.exe'), (Join-Path $dmBin 'link.exe'), (Join-Path $dmBin 'lib.exe'), (Join-Path $dmSdkBin 'rc.exe'), (Join-Path $dmSdkBin 'mt.exe'))) {
    $dmAbsolute = [IO.Path]::GetFullPath($dmTool)
    if (!$dmAbsolute.StartsWith($dmBoundary, [StringComparison]::OrdinalIgnoreCase) -or !(Test-Path -LiteralPath $dmAbsolute -PathType Leaf)) { throw "Missing or nonlocal project tool: $dmTool" }
}
$dmVariables = @('PATH', 'INCLUDE', 'LIB', 'LIBPATH', 'TEMP', 'TMP', 'CL', '_CL_', 'LINK', '_LINK_', 'VSLANG')
$dmPrevious = @{}
foreach ($dmVariable in $dmVariables) { $dmPrevious[$dmVariable] = [Environment]::GetEnvironmentVariable($dmVariable, 'Process') }
$dmPreviousLocation = Get-Location
$null = New-Item -ItemType Directory -Path (Join-Path $dmTools 'tmp') -Force
$null = New-Item -ItemType Directory -Path (Join-Path $dmProjectRoot 'out/logs') -Force
$dmLog = Join-Path $dmProjectRoot 'out/logs/agent-x64-build.log'
[IO.File]::WriteAllText($dmLog, '', (New-Object Text.UTF8Encoding($false)))

function Invoke-ProjectCMake([string[]]$ToolArguments) {
    $dmSavedPreference = $ErrorActionPreference
    try {
        # Native stderr is retained; the process exit code decides success.
        $ErrorActionPreference = 'Continue'
        & $dmCmake @ToolArguments 2>&1 | ForEach-Object { $_.ToString() } | Tee-Object -FilePath $dmLog -Append
        $dmExitCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $dmSavedPreference }
    if ($dmExitCode -ne 0) { throw "CMake failed with exit code $dmExitCode; see $dmLog" }
}
try {
    $env:PATH = @($dmBin, $dmSdkBin, [IO.Path]::GetDirectoryName($dmCmake), [IO.Path]::GetDirectoryName($dmNinja), (Join-Path $env:SystemRoot 'System32'), $env:SystemRoot) -join ';'
    $env:INCLUDE = @((Join-Path $dmVc 'include'), (Join-Path $dmSdk ('Include/' + $dmToolchain.sdk_version + '/ucrt')), (Join-Path $dmSdk ('Include/' + $dmToolchain.sdk_version + '/shared')), (Join-Path $dmSdk ('Include/' + $dmToolchain.sdk_version + '/um')), (Join-Path $dmSdk ('Include/' + $dmToolchain.sdk_version + '/winrt'))) -join ';'
    $env:LIB = @((Join-Path $dmVc 'lib/x64'), (Join-Path $dmSdkLib 'ucrt/x64'), (Join-Path $dmSdkLib 'um/x64')) -join ';'
    $env:LIBPATH = $env:LIB
    $env:TEMP = Join-Path $dmTools 'tmp'
    $env:TMP = $env:TEMP
    $env:CL = $null
    $env:_CL_ = $null
    $env:LINK = $null
    $env:_LINK_ = $null
    $env:VSLANG = '1033'
    Set-Location -LiteralPath $dmProjectRoot
    $dmArguments = @('--preset', 'local-agent-x64',
        ('-DCMAKE_CXX_COMPILER:FILEPATH=' + (Join-Path $dmBin 'cl.exe').Replace('\', '/')),
        ('-DCMAKE_LINKER:FILEPATH=' + (Join-Path $dmBin 'link.exe').Replace('\', '/')),
        ('-DCMAKE_AR:FILEPATH=' + (Join-Path $dmBin 'lib.exe').Replace('\', '/')),
        ('-DCMAKE_RC_COMPILER:FILEPATH=' + (Join-Path $dmSdkBin 'rc.exe').Replace('\', '/')),
        ('-DCMAKE_MT:FILEPATH=' + (Join-Path $dmSdkBin 'mt.exe').Replace('\', '/')),
        ('-DCMAKE_MAKE_PROGRAM:FILEPATH=' + $dmNinja.Replace('\', '/')))
    Invoke-ProjectCMake -ToolArguments $dmArguments
    Invoke-ProjectCMake -ToolArguments @('--build', '--preset', 'release-agent-x64', '--parallel', '2')
    if (!$SkipInstall) { Invoke-ProjectCMake -ToolArguments @('--install', (Join-Path $dmProjectRoot 'out/agent-x64')) }
} finally {
    Set-Location -LiteralPath $dmPreviousLocation
    foreach ($dmVariable in $dmVariables) { [Environment]::SetEnvironmentVariable($dmVariable, $dmPrevious[$dmVariable], 'Process') }
}
Write-Output 'Agent build completed. No application, browser, test or device was launched.'
