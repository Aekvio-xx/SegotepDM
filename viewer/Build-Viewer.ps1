[CmdletBinding()]
param([switch]$CopyToAgent)

$ErrorActionPreference = 'Stop'
$viewerRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$projectRoot = [IO.Path]::GetFullPath((Join-Path $viewerRoot '..'))
$sourceRoot = Join-Path $viewerRoot 'src'
$outputRoot = Join-Path $viewerRoot 'dist'
$utf8 = New-Object Text.UTF8Encoding($false, $true)
$lineBreak = [string][char]10
$null = New-Item -ItemType Directory -Path $outputRoot -Force

function Read-Source([string]$Name) {
    [IO.File]::ReadAllText((Join-Path $sourceRoot $Name), $utf8)
}
function Get-Part([string]$Content, [string]$Name) {
    $pattern = '(?s)<!--DM_PART:' + [regex]::Escape($Name) + '-->(.*?)<!--/DM_PART:' + [regex]::Escape($Name) + '-->'
    $partMatches = [regex]::Matches($Content, $pattern)
    if ($partMatches.Count -ne 1) { throw "HTML partial must appear once: $Name" }
    $partMatches[0].Groups[1].Value.Trim()
}
function Inline-Script([string]$Code) {
    if ($Code -match '(?i)</script') { throw 'JavaScript source contains an HTML closing script tag.' }
    '<script>' + $lineBreak + $Code + $lineBreak + '</script>'
}
$portableRoot = $null
if ($CopyToAgent) {
    $agentRoot = Join-Path $projectRoot 'dist/DM-Agent-x64'
    if (!(Test-Path -LiteralPath $agentRoot -PathType Container)) { throw 'DM-Agent-x64 distribution folder does not exist.' }
    $portableRoot = Join-Path $agentRoot 'viewer'
    $null = New-Item -ItemType Directory -Path $portableRoot -Force
}

$csv = Read-Source 'csv.html'
$logs = Read-Source 'logs.html'
$sections = [ordered]@{
    COMMON_STYLE = '<style>' + $lineBreak + (Read-Source 'styles.css') + $lineBreak + '</style>'
    CSV_STYLE = (Get-Part $csv 'STYLE')
    LOG_STYLE = (Get-Part $logs 'STYLE')
    CSV_SIDEBAR = (Get-Part $csv 'SIDEBAR')
    LOG_SIDEBAR = (Get-Part $logs 'SIDEBAR')
    CSV_MAIN = (Get-Part $csv 'MAIN')
    LOG_MAIN = (Get-Part $logs 'MAIN')
    CSV_OVERLAY = (Get-Part $csv 'OVERLAY')
    LOG_OVERLAY = (Get-Part $logs 'OVERLAY')
    COMMON_SCRIPT = (Inline-Script (Read-Source 'common.js'))
    CSV_SCRIPT = (Inline-Script (Read-Source 'csv.js'))
    LOG_SCRIPT = (Inline-Script (Read-Source 'logs.js'))
    NAV_SCRIPT = (Inline-Script (Read-Source 'navigation.js'))
}
$html = Read-Source 'index.html'
foreach ($name in $sections.Keys) {
    $marker = '<!--DM_' + $name + '-->'
    if ([regex]::Matches($html, [regex]::Escape($marker)).Count -ne 1) { throw "Template marker must appear once: $marker" }
    $html = $html.Replace($marker, $sections[$name])
}
if ($html -match '<!--DM_[A-Z_]+-->') { throw 'Unresolved HTML template marker.' }
[IO.File]::WriteAllText((Join-Path $outputRoot 'DM-Record-Viewer.html'), $html, $utf8)
Copy-Item -LiteralPath (Join-Path $viewerRoot 'USAGE.md') -Destination (Join-Path $outputRoot 'USAGE.md') -Force
$outputNames = @('DM-Record-Viewer.html', 'USAGE.md')
$sourceNames = @('src/index.html', 'src/navigation.js', 'src/common.js', 'src/styles.css', 'src/csv.html', 'src/csv.js', 'src/logs.html', 'src/logs.js', 'Build-Viewer.ps1', 'USAGE.md')
$sourceHashes = [ordered]@{}
foreach ($name in $sourceNames) {
    $sourceHashes[$name] = (Get-FileHash -LiteralPath (Join-Path $viewerRoot $name) -Algorithm SHA256).Hash
}
$outputHashes = [ordered]@{}
foreach ($name in $outputNames) {
    $outputHashes[$name] = (Get-FileHash -LiteralPath (Join-Path $outputRoot $name) -Algorithm SHA256).Hash
}
$manifest = [ordered]@{
    format_version = 2
    product = 'DM Offline Record Viewer'
    layout = 'single_html_tabs'
    assembled_at = [DateTimeOffset]::Now.ToString('o')
    source_sha256 = $sourceHashes
    output_sha256 = $outputHashes
    assembly_scope = 'Embed local source text only. Does not launch or test any page, browser, DM Agent, plugin, or device.'
}
[IO.File]::WriteAllText((Join-Path $outputRoot 'build-manifest.json'), ($manifest | ConvertTo-Json -Depth 6) + $lineBreak, $utf8)
$outputNames += 'build-manifest.json'

if ($CopyToAgent) {
    foreach ($name in $outputNames) {
        Copy-Item -LiteralPath (Join-Path $outputRoot $name) -Destination (Join-Path $portableRoot $name) -Force
    }
    Write-Output "Portable copy: $portableRoot"
}
foreach ($name in $outputNames) {
    Get-Item -LiteralPath (Join-Path $outputRoot $name) | Select-Object Name, Length
}
