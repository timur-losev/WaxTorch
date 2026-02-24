[CmdletBinding()]
param(
  [Parameter(Mandatory = $false)]
  [string]$Url = "",

  [Parameter(Mandatory = $false)]
  [string]$ZipPath = "",

  [Parameter(Mandatory = $false)]
  [string]$OutputFileName = "",

  [Parameter(Mandatory = $false)]
  [string]$ManifestPath = "cpp/manifest/libtorch-manifest.json",

  [Parameter(Mandatory = $false)]
  [string]$DistRoot = "cpp/third_party/libtorch-dist",

  [Parameter(Mandatory = $false)]
  [string]$ArtifactSubdir = "artifacts/windows-cuda",

  [switch]$NoManifestUpdate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Ensure-Directory([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path)) {
    New-Item -ItemType Directory -Path $Path | Out-Null
  }
}

function Normalize-RelativeArtifactPath([string]$DistRootAbsolute, [string]$ArtifactAbsolute) {
  $relative = [System.IO.Path]::GetRelativePath($DistRootAbsolute, $ArtifactAbsolute).Replace('\', '/')
  if ($relative.StartsWith("../", [System.StringComparison]::Ordinal) -or $relative -eq "..") {
    throw "artifact path escapes dist root: '$relative'"
  }
  return $relative
}

function Load-Manifest([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path)) {
    return @{ artifacts = @() }
  }

  $raw = Get-Content -LiteralPath $Path -Raw
  if ([string]::IsNullOrWhiteSpace($raw)) {
    return @{ artifacts = @() }
  }

  $parsed = $raw | ConvertFrom-Json -Depth 64 -AsHashtable
  if (-not $parsed.ContainsKey("artifacts") -or $null -eq $parsed.artifacts) {
    $parsed.artifacts = @()
  }
  return $parsed
}

function Save-Manifest([string]$Path, [hashtable]$Manifest) {
  $manifestDir = Split-Path -Parent -Path $Path
  if (-not [string]::IsNullOrWhiteSpace($manifestDir)) {
    Ensure-Directory -Path $manifestDir
  }

  $normalizedArtifacts = @()
  foreach ($item in $Manifest.artifacts) {
    if ($null -eq $item) {
      continue
    }
    if (-not $item.ContainsKey("path") -or -not $item.ContainsKey("sha256")) {
      continue
    }
    if ([string]::IsNullOrWhiteSpace([string]$item.path) -or [string]::IsNullOrWhiteSpace([string]$item.sha256)) {
      continue
    }
    $normalizedArtifacts += [ordered]@{
      path = [string]$item.path
      sha256 = ([string]$item.sha256).ToLowerInvariant()
    }
  }

  $normalizedArtifacts = $normalizedArtifacts |
    Sort-Object -Property @{ Expression = { $_.path } }, @{ Expression = { $_.sha256 } }

  $outManifest = [ordered]@{
    artifacts = @($normalizedArtifacts)
  }

  $json = $outManifest | ConvertTo-Json -Depth 16
  [System.IO.File]::WriteAllText($Path, $json + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
}

$repoRoot = Resolve-Path -LiteralPath (Join-Path -Path $PSScriptRoot -ChildPath "..")
$distRootAbs = [System.IO.Path]::GetFullPath((Join-Path -Path $repoRoot -ChildPath $DistRoot))
$manifestAbs = [System.IO.Path]::GetFullPath((Join-Path -Path $repoRoot -ChildPath $ManifestPath))

if ([string]::IsNullOrWhiteSpace($Url) -and [string]::IsNullOrWhiteSpace($ZipPath)) {
  throw "pass -Url or -ZipPath"
}
if (-not [string]::IsNullOrWhiteSpace($Url) -and -not [string]::IsNullOrWhiteSpace($ZipPath)) {
  throw "pass only one source: -Url or -ZipPath"
}

Ensure-Directory -Path $distRootAbs
$artifactDirAbs = [System.IO.Path]::GetFullPath((Join-Path -Path $distRootAbs -ChildPath $ArtifactSubdir))
Ensure-Directory -Path $artifactDirAbs

if ([string]::IsNullOrWhiteSpace($OutputFileName)) {
  if (-not [string]::IsNullOrWhiteSpace($ZipPath)) {
    $OutputFileName = Split-Path -Leaf -Path $ZipPath
  } else {
    $uri = [System.Uri]::new($Url)
    $leaf = Split-Path -Leaf -Path $uri.AbsolutePath
    $OutputFileName = [System.Uri]::UnescapeDataString($leaf)
  }
}

if ([string]::IsNullOrWhiteSpace($OutputFileName)) {
  throw "failed to resolve output filename"
}

$artifactAbs = Join-Path -Path $artifactDirAbs -ChildPath $OutputFileName

if (-not [string]::IsNullOrWhiteSpace($Url)) {
  Write-Host "[waxcpp] Downloading artifact from URL"
  Write-Host "  $Url"
  Invoke-WebRequest -Uri $Url -OutFile $artifactAbs
} else {
  $zipAbs = [System.IO.Path]::GetFullPath((Join-Path -Path $repoRoot -ChildPath $ZipPath))
  if (-not (Test-Path -LiteralPath $zipAbs)) {
    throw "zip source not found: $zipAbs"
  }
  Copy-Item -LiteralPath $zipAbs -Destination $artifactAbs -Force
}

if (-not (Test-Path -LiteralPath $artifactAbs)) {
  throw "artifact file not found after prepare step: $artifactAbs"
}

$artifactHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $artifactAbs).Hash.ToLowerInvariant()
$artifactRelative = Normalize-RelativeArtifactPath -DistRootAbsolute $distRootAbs -ArtifactAbsolute $artifactAbs

Write-Host "[waxcpp] Prepared artifact:"
Write-Host "  path   : $artifactRelative"
Write-Host "  sha256 : $artifactHash"

if (-not $NoManifestUpdate) {
  $manifest = Load-Manifest -Path $manifestAbs

  $updated = $false
  for ($i = 0; $i -lt $manifest.artifacts.Count; $i++) {
    $current = $manifest.artifacts[$i]
    if ($null -eq $current) {
      continue
    }
    if (-not $current.ContainsKey("path")) {
      continue
    }
    if ([string]$current.path -eq $artifactRelative) {
      $manifest.artifacts[$i] = [ordered]@{
        path = $artifactRelative
        sha256 = $artifactHash
      }
      $updated = $true
      break
    }
  }

  if (-not $updated) {
    $manifest.artifacts += [ordered]@{
      path = $artifactRelative
      sha256 = $artifactHash
    }
  }

  Save-Manifest -Path $manifestAbs -Manifest $manifest
  Write-Host "[waxcpp] Manifest updated:"
  Write-Host "  $manifestAbs"
} else {
  Write-Host "[waxcpp] Manifest update skipped (-NoManifestUpdate)."
}

Write-Host ""
Write-Host "[waxcpp] Next:"
Write-Host "  `$env:WAXCPP_LIBTORCH_MANIFEST='$manifestAbs'"
Write-Host "  `$env:WAXCPP_LIBTORCH_DIST_ROOT='$distRootAbs'"
Write-Host "  `$env:WAXCPP_REQUIRE_LIBTORCH_MANIFEST='1'"
Write-Host "  `$env:WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256='1'"
Write-Host "  scripts\\generate-cmake.bat --config Release --enable-libtorch-runtime ON --libtorch-root <unpacked_libtorch_root>"
