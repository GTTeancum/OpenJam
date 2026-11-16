# Run in a VS Developer PowerShell with clang, cmake, ninja and git on PATH.
# All sources, build products, installation files and caches remain in this repo.
param([ValidateRange(1,32)] [int]$Jobs = 8)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
$source = Join-Path $repo '.local/deps/rexglue-sdk'
$build = Join-Path $repo '.local/sdk-build'
$prefix = Join-Path $repo '.local/sdk-dual'
New-Item -ItemType Directory -Force -Path "$repo/.local/tmp", "$repo/.local/profile/Local", "$repo/.local/profile/Roaming" | Out-Null
$env:TEMP = "$repo/.local/tmp"
$env:TMP = $env:TEMP
$env:LOCALAPPDATA = "$repo/.local/profile/Local"
$env:APPDATA = "$repo/.local/profile/Roaming"
$env:XDG_CACHE_HOME = "$repo/.local/cache"
$env:PYTHONDONTWRITEBYTECODE = '1'

function Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed ($LASTEXITCODE)" }
}
foreach ($program in 'git','cmake','ninja','clang','clang++') {
    Get-Command $program -ErrorAction Stop | Out-Null
}
if (-not (Test-Path -LiteralPath "$source/.git")) {
    Checked git @('clone','--depth','1','--branch','v0.10.0',
        'https://github.com/rexglue/rexglue-sdk.git',$source)
}
$revision = & git -C $source rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $revision -ne 'f5337cdc947ff6d4c4196737e2c807a48f2a1fc2') {
    throw "Expected ReXGlue v0.10.0; found $revision"
}
Checked git @('-C',$source,'submodule','update','--init','--recursive','--depth','1')

# Git for Windows may check out symlinks as small text files. Materialize only
# libmspack's tracked links, and only when their targets stay in that submodule.
$mspack = Join-Path $source 'thirdparty/libmspack'
$boundary = [IO.Path]::GetFullPath($mspack) + [IO.Path]::DirectorySeparatorChar
$links = & git -C $mspack ls-files -s
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect libmspack links' }
foreach ($line in $links) {
    if ($line -notmatch '^120000 [0-9a-f]+ 0\t(.+)$') { continue }
    $relative = $matches[1]
    $link = Join-Path $mspack $relative
    if ((Get-Item -LiteralPath $link).LinkType) { continue }
    $reference = & git -C $mspack show (':'+$relative)
    if ($LASTEXITCODE -ne 0) { throw "Cannot read link $relative" }
    $target = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $link) $reference))
    if (-not $target.StartsWith($boundary, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Link target escapes libmspack: $relative"
    }
    if ((Get-Content -LiteralPath $link -Raw).Trim() -eq $reference.Trim()) {
        Copy-Item -LiteralPath $target -Destination $link -Force
    }
}
foreach ($patch in Get-ChildItem -LiteralPath "$repo/tools/sdk-patches" -Filter '*.patch' | Sort-Object Name) {
    & git -C $source apply --reverse --check $patch.FullName 2>$null
    if ($LASTEXITCODE -eq 0) { continue }
    Checked git @('-C',$source,'apply','--check',$patch.FullName)
    Checked git @('-C',$source,'apply',$patch.FullName)
}
Checked cmake @('-S',$source,'-B',$build,'-G','Ninja',
    '-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DCMAKE_C_COMPILER=clang',
    '-DCMAKE_CXX_COMPILER=clang++','-DCMAKE_C_FLAGS=-march=x86-64-v2',
    '-DCMAKE_CXX_FLAGS=-march=x86-64-v2','-DREXGLUE_USE_VULKAN=ON',
    '-DREXGLUE_USE_D3D12=ON','-DREXGLUE_REGISTER_USER_PACKAGE=OFF',
    '-DCMAKE_EXPORT_PACKAGE_REGISTRY=OFF',"-DCMAKE_INSTALL_PREFIX=$prefix")
Checked cmake @('--build',$build,'-j',"$Jobs")
Checked cmake @('--install',$build)
Write-Output "SDK installed in $prefix"
