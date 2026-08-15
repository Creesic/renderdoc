#
# Vendor standalone CPython + renderdoc-mcp + dependencies next to qrenderdoc.exe (MSBuild post-build).
# Uses only PowerShell plus tools shipped with Win10+: tar, HTTPS.
#

param(
  [Parameter(Mandatory = $true)]
  [string]$RepoRoot,
  [Parameter(Mandatory = $true)]
  [string]$RuntimeDir,
  [Parameter(Mandatory = $true)]
  [ValidateSet('Win32','x64','ARM64')]
  [string]$VSPlatform
)

$ErrorActionPreference = 'Stop'

if ($env:RENDERDOC_EMBED_MCP_PYTHON -eq '0' -or $env:RENDERDOC_SKIP_MCP_BUNDLE -eq '1') {
  Write-Host '[MCP bundle] skipped (RENDERDOC_EMBED_MCP_PYTHON=0 or RENDERDOC_SKIP_MCP_BUNDLE=1)'
  exit 0
}

$RepoRoot = $RepoRoot.TrimEnd('\').TrimEnd('/').Trim()
$RuntimeDir = $RuntimeDir.TrimEnd('\').TrimEnd('/').Trim()

$manifestPath = Join-Path $RepoRoot 'util\renderdoc_mcp_bundle.json'
if (!(Test-Path -LiteralPath $manifestPath)) {
  Write-Error "Missing $($manifestPath)"
  exit 1
}

$cfg = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json

$tkey =
  if ($VSPlatform -eq 'Win32') {
    'windows_x86'
  } elseif ($VSPlatform -eq 'x64') {
    'windows_amd64'
  } else {
    Write-Error "MCP bundle: unsupported VS platform $($VSPlatform) (needs manifest entry)."
    exit 2
  }

$tpl = $cfg.triplets.($tkey)
if ($null -eq $tpl -or !$tpl.artifact) {
  Write-Error "Triple $($tkey) missing in $($manifestPath)"
  exit 2
}

$cacheDir =
  if ($env:RENDERDOC_MCP_CACHE) {
    [string]$env:RENDERDOC_MCP_CACHE
  } else {
    Join-Path $RepoRoot '.renderdoc_mcp_cache'
  }
New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null

$archivePath = Join-Path $cacheDir $tpl.artifact
$baseTag = ([string]$cfg.release_tag).Trim()
$url = ($cfg.base_url_template -replace '\{release_tag\}', $baseTag).Trim('/') + '/' + [string]$tpl.artifact

if (!(Test-Path -LiteralPath $archivePath)) {
  Write-Host "[MCP bundle] downloading $url ..."
  Invoke-WebRequest -Uri $url -OutFile ([string]$archivePath + '.part') -UseBasicParsing -TimeoutSec 600
  Move-Item -Force ([string]$archivePath + '.part') $archivePath
}

if ($env:RENDERDOC_MCP_SKIP_HASH -ne '1' -and $tpl.sha256) {
  $h = Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath
  if (($h.Hash).ToLower() -ne ($tpl.sha256).ToLower()) {
    Write-Error ("SHA256 mismatch for {0}. Expected {1}, got {2}. Delete file or bump manifest." -f $archivePath,
      $tpl.sha256, $h.Hash)
    exit 3
  }
}

$mcpPkg = Join-Path $RepoRoot 'renderdoc-mcp\renderdoc_mcp'
if (!(Test-Path -LiteralPath $mcpPkg)) {
  Write-Error "missing $mcpPkg"
  exit 4
}

$pyDst = Join-Path $RuntimeDir 'python'
$mcpDst = Join-Path $RuntimeDir 'mcp'
$siteDst = Join-Path $RuntimeDir 'mcp_site'

foreach ($d in @($pyDst, $mcpDst, $siteDst)) {
  if (Test-Path -LiteralPath $d) {
    Remove-Item -LiteralPath $d -Recurse -Force
  }
}

$stage = Join-Path $env:TEMP ("rd_mcp_tar_" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $stage | Out-Null
try {
  & tar xf $archivePath -C $stage
  if ($LASTEXITCODE -ne 0) { throw "tar failed with exit $($LASTEXITCODE)" }

  $root = Join-Path $stage 'python'
  if (!(Test-Path -LiteralPath (Join-Path $root 'python.exe'))) {
    foreach ($candidate in @(Get-ChildItem -LiteralPath $stage -Directory)) {
      if (Test-Path -LiteralPath (Join-Path $candidate.FullName 'python.exe')) {
        $root = $candidate.FullName
        break
      }
    }
  }
  if (!(Test-Path -LiteralPath (Join-Path $root 'python.exe'))) {
    throw "could not locate python.exe under extracted $($stage)"
  }

  Copy-Item -LiteralPath $root -Destination $pyDst -Recurse -Force
} finally {
  Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}

# python-build-standalone "install_only" trees ship Lib/ but no pythonXY.zip.
# qrenderdoc/Code/pyrenderdoc/python.props requires python$(MM).zip to select this prefix for native pymodules.
$pyMM = ([string]$cfg.python_version_major_minor).Replace('.', '')
$libDir = Join-Path $pyDst 'Lib'
$zipPath = Join-Path $pyDst ('python' + $pyMM + '.zip')
if ((Test-Path -LiteralPath $libDir) -and !(Test-Path -LiteralPath $zipPath)) {
  Write-Host "[MCP bundle] creating python$pyMM.zip from Lib (enables MSBuild pymodules link against this runtime)..."
  Push-Location $libDir
  try {
    Compress-Archive -Path * -DestinationPath $zipPath -CompressionLevel Fastest -Force
  } finally {
    Pop-Location
  }
}

$pyExe = Join-Path $pyDst 'python.exe'

$hookBody = @'
"""Inserted by RenderDoc util/bundle_renderdoc_mcp (processes mcp_site .pth hooks, e.g. pywin32)."""
import os
import site

_appdir = os.environ.get("RENDERDOC_MCP_APPDIR")
if _appdir:
    try:
        site.addsitedir(os.path.join(_appdir, "mcp_site"))
    except Exception:
        pass
'@

$pure = (& $pyExe -c 'import sysconfig; print(sysconfig.get_paths()[''purelib''])').Trim()
if ($LASTEXITCODE -ne 0 -or !$pure) {
  Write-Error "MCP bundle: bundled python failed to report purelib (exit $($LASTEXITCODE))"
  exit 5
}
$hook = Join-Path $pure 'sitecustomize.py'
New-Item -ItemType Directory -Force -Path $pure | Out-Null
Set-Content -LiteralPath $hook -Encoding utf8 -Value $hookBody

& $pyExe -m ensurepip --default-pip | Out-Host
Write-Host ''

& $pyExe -m pip install --disable-pip-version-check --no-input --upgrade pip | Out-Host
Write-Host ''

New-Item -ItemType Directory -Force -Path $mcpDst | Out-Null

Copy-Item -LiteralPath (Join-Path $RepoRoot 'renderdoc-mcp\renderdoc_mcp') -Destination (Join-Path $mcpDst 'renderdoc_mcp') -Recurse -Force
$copiedMcp = Join-Path $mcpDst 'renderdoc_mcp'
Get-ChildItem -LiteralPath $copiedMcp -Directory -Recurse -Filter '__pycache__' -ErrorAction SilentlyContinue |
  Remove-Item -Recurse -Force
Get-ChildItem -LiteralPath $copiedMcp -File -Recurse -Include '*.pyc', '*.pyo' -ErrorAction SilentlyContinue |
  Remove-Item -Force

$mcpProj = Join-Path $RepoRoot 'renderdoc-mcp'
& $pyExe -m pip install --disable-pip-version-check --no-input --target $siteDst $mcpProj | Out-Host
# Native exe exit codes are not covered by $ErrorActionPreference; without this check a failed
# install (e.g. no PyPI access) still stamped and reported success, leaving mcp_site empty and
# the MCP server dying at runtime with ModuleNotFoundError. Mirrors check=True in the .py variant.
if ($LASTEXITCODE -ne 0) {
  Write-Error "MCP bundle: pip install into mcp_site failed (exit $($LASTEXITCODE))"
  exit 6
}
Write-Host ''

$stamp = Join-Path $RuntimeDir '.renderdoc_mcp_bundle.stamp.txt'
Set-Content -LiteralPath $stamp -Encoding utf8 -Value ($tkey + ' ' + $tpl.artifact)
Write-Host "[MCP bundle] done → $RuntimeDir"

exit 0
