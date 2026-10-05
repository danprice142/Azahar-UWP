# Copyright Azahar Emulator Project
# Licensed under GPLv2 or any later version
#
# Rebuilds the checked-in x64 AppContainer FFmpeg 6 runtime used for UWP video dumping.

[CmdletBinding()]
param(
    [string] $SourceDir = (Join-Path $env:TEMP 'azahar-ffmpeg-n6.0'),
    [string] $Msys2Root = 'C:\msys64',
    [string] $VsInstallPath = '',
    [ValidateRange(1, 256)]
    [int] $Jobs = [Environment]::ProcessorCount
)

$ErrorActionPreference = 'Stop'
$ffmpegCommit = 'ea3d24bbe3c58b171e55fe2151fc7ffaca3ab3d2'
$requiredDlls = @(
    'avcodec-60.dll',
    'avfilter-9.dll',
    'avformat-60.dll',
    'avutil-58.dll',
    'swresample-4.dll',
    'swscale-7.dll'
)

$distUwp = $PSScriptRoot
$repoRoot = Split-Path (Split-Path $distUwp -Parent) -Parent
$destinationRoot = Join-Path $repoRoot 'externals\uwp\ffmpeg\6.0'
$destinationBin = Join-Path $destinationRoot 'bin'
$bash = Join-Path $Msys2Root 'usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash)) {
    throw "MSYS2 bash was not found at $bash."
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not $VsInstallPath) {
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'Visual Studio Installer (vswhere.exe) was not found.'
    }
    $VsInstallPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $VsInstallPath) {
    throw 'Visual Studio 2022 with the MSVC x64 tools was not found.'
}

$devShellModule = Join-Path $VsInstallPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
if (-not (Test-Path -LiteralPath $devShellModule)) {
    throw "Visual Studio developer shell module was not found at $devShellModule."
}
Import-Module $devShellModule
Enter-VsDevShell -VsInstallPath $VsInstallPath -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

$nasm = Get-Command nasm.exe -ErrorAction SilentlyContinue
if (-not $nasm) {
    throw 'NASM was not found. Install NASM and add it to PATH.'
}
$env:PATH = "$(Split-Path $nasm.Source -Parent);$env:PATH"

$sourceFull = [IO.Path]::GetFullPath($SourceDir)
if (-not (Test-Path -LiteralPath $sourceFull)) {
    New-Item -ItemType Directory -Force -Path (Split-Path $sourceFull -Parent) | Out-Null
    & git clone --no-checkout https://git.ffmpeg.org/ffmpeg.git $sourceFull
    if ($LASTEXITCODE -ne 0) { throw "git clone failed ($LASTEXITCODE)" }
    & git -C $sourceFull checkout --detach $ffmpegCommit
    if ($LASTEXITCODE -ne 0) { throw "git checkout failed ($LASTEXITCODE)" }
} else {
    if (-not (Test-Path -LiteralPath (Join-Path $sourceFull '.git'))) {
        throw "$sourceFull exists but is not an FFmpeg git checkout."
    }
    $actualCommit = (& git -C $sourceFull rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $actualCommit -ne $ffmpegCommit) {
        throw "$sourceFull is at $actualCommit; expected $ffmpegCommit. Refusing to rewrite it."
    }
    # The build/install directories are untracked; refuse tracked source edits.
    $dirty = & git -C $sourceFull status --porcelain --untracked-files=no
    if ($LASTEXITCODE -ne 0 -or $dirty) {
        throw "$sourceFull has local changes. Refusing to build from a dirty FFmpeg source tree."
    }
}

$buildDir = Join-Path $sourceFull 'azahar-build-uwp-x64'
$installDir = Join-Path $sourceFull 'azahar-install-uwp-x64'
$sourcePrefix = $sourceFull.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
foreach ($directory in @($buildDir, $installDir)) {
    $full = [IO.Path]::GetFullPath($directory)
    if (-not $full.StartsWith($sourcePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove build directory outside the FFmpeg checkout: $full"
    }
    if (Test-Path -LiteralPath $full) {
        Remove-Item -LiteralPath $full -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $full | Out-Null
}

$env:AZAHAR_FFMPEG_SOURCE = $sourceFull
$env:AZAHAR_FFMPEG_BUILD = $buildDir
$env:AZAHAR_FFMPEG_INSTALL = $installDir
$env:AZAHAR_FFMPEG_JOBS = [string]$Jobs
function Convert-ToMsysPath([string] $Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($full -notmatch '^([A-Za-z]):\\(.*)$') {
        throw "Cannot convert non-drive path to MSYS2 form: $full"
    }
    return '/' + $Matches[1].ToLowerInvariant() + '/' + $Matches[2].Replace('\', '/')
}
$env:AZAHAR_FFMPEG_SOURCE_MSYS = Convert-ToMsysPath $sourceFull
$env:AZAHAR_FFMPEG_BUILD_MSYS = Convert-ToMsysPath $buildDir
$env:AZAHAR_FFMPEG_INSTALL_MSYS = Convert-ToMsysPath $installDir
$cl = Get-Command cl.exe -ErrorAction Stop
$env:AZAHAR_MSVC_BIN_MSYS = Convert-ToMsysPath (Split-Path $cl.Source -Parent)
$env:AZAHAR_NASM_BIN_MSYS = Convert-ToMsysPath (Split-Path $nasm.Source -Parent)
$msvcRoot = Split-Path (Split-Path (Split-Path (Split-Path $cl.Source -Parent) -Parent) -Parent) -Parent
$msvcStoreLib = Join-Path $msvcRoot 'lib\x64\store'
if (-not (Test-Path -LiteralPath (Join-Path $msvcStoreLib 'vcruntime.lib'))) {
    throw "MSVC Store runtime import libraries were not found at $msvcStoreLib."
}

$buildScript = @'
set -euo pipefail
export PATH="$AZAHAR_MSVC_BIN_MSYS:$AZAHAR_NASM_BIN_MSYS:/usr/bin:/bin:$PATH"
source_dir="$AZAHAR_FFMPEG_SOURCE_MSYS"
build_dir="$AZAHAR_FFMPEG_BUILD_MSYS"
install_dir="$AZAHAR_FFMPEG_INSTALL_MSYS"
cd "$build_dir"
"$source_dir/configure" \
  --prefix="$install_dir" \
  --toolchain=msvc \
  --arch=x86_64 \
  --target-os=win32 \
  --enable-cross-compile \
  --enable-shared \
  --disable-static \
  --disable-programs \
  --disable-doc \
  --disable-debug \
  --disable-avdevice \
  --enable-network \
  --disable-decoders \
  --disable-demuxers \
  --disable-parsers \
  --disable-bsfs \
  --disable-hwaccels \
  --disable-protocols \
  --enable-protocol=file,udp \
  --disable-muxers \
  --enable-muxer=mp4,matroska,avi,mpegts \
  --disable-encoders \
  --enable-encoder=h264_mf,mpeg4,rawvideo,ffv1,aac,flac,pcm_s16le \
  --disable-filters \
  --enable-filter=buffer,buffersink,fps,format,scale \
  --disable-dxva2 \
  --disable-d3d11va \
  --disable-gpl \
  --disable-version3 \
  --extra-cflags="-MD -DWINAPI_FAMILY=WINAPI_FAMILY_APP -D_WIN32_WINNT=0x0A00 -wd4267 -wd4133 -wd4334 -wd4101" \
  --extra-ldflags="-APPCONTAINER WindowsApp.lib"
make -j"$AZAHAR_FFMPEG_JOBS"
make install
'@

# Windows' argument quoting truncates a multiline script passed through `bash -lc`, so the
# build driver is written to the build directory and executed by path.
$buildDriver = Join-Path $buildDir 'azahar-build-driver.sh'
[IO.File]::WriteAllText($buildDriver, $buildScript.Replace("`r`n", "`n"),
    [Text.UTF8Encoding]::new($false))
$buildDriverMsys = Convert-ToMsysPath $buildDriver
$winrtPatch = Join-Path $distUwp 'ffmpeg-winrt.patch'
& git -C $sourceFull apply --check $winrtPatch
if ($LASTEXITCODE -ne 0) {
    throw 'The FFmpeg WinRT compatibility patch no longer applies cleanly.'
}
& git -C $sourceFull apply $winrtPatch
if ($LASTEXITCODE -ne 0) {
    throw 'Could not apply the FFmpeg WinRT compatibility patch.'
}
# Lets h264_mf drive the Xbox hardware transform: D3D11 device manager, non-blocking async
# events and the input sample layout the transform expects.
$encoderPatch = Join-Path $distUwp 'ffmpeg-uwp-mf-encoder.patch'
& git -C $sourceFull apply --check $encoderPatch
if ($LASTEXITCODE -ne 0) {
    throw 'The FFmpeg UWP Media Foundation encoder patch no longer applies cleanly.'
}
& git -C $sourceFull apply $encoderPatch
if ($LASTEXITCODE -ne 0) {
    throw 'Could not apply the FFmpeg UWP Media Foundation encoder patch.'
}
$savedLibPath = $env:LIB
try {
    # cl.exe embeds default-library directives for vcruntime/msvcrt. Put the Store variants first
    # so the DLLs import VCRUNTIME140_APP.dll, which is what the Xbox VCLibs framework provides.
    $env:LIB = "$msvcStoreLib;$savedLibPath"
    & $bash $buildDriverMsys
    if ($LASTEXITCODE -ne 0) {
        throw "FFmpeg build failed ($LASTEXITCODE)"
    }
} finally {
    $env:LIB = $savedLibPath
    # Keep the pinned upstream checkout pristine so rebuilds remain reproducible.
    & git -C $sourceFull checkout -- libavutil/cpu.c libavcodec/mfenc.c
}

New-Item -ItemType Directory -Force -Path $destinationBin | Out-Null
foreach ($dll in $requiredDlls) {
    $built = Join-Path (Join-Path $installDir 'bin') $dll
    if (-not (Test-Path -LiteralPath $built)) {
        throw "The FFmpeg build did not produce $dll."
    }

    $headers = (& dumpbin.exe /headers $built 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $headers -notmatch 'machine \(x64\)' -or
        $headers -notmatch 'App Container') {
        throw "$dll is not an x64 AppContainer binary."
    }

    $dependents = (& dumpbin.exe /dependents $built 2>&1) -join "`n"
    foreach ($forbidden in @(
        'USER32.dll',
        'GDI32.dll',
        'SHELL32.dll',
        'WINMM.dll',
        'VCRUNTIME140.dll',
        'VCRUNTIME140_1.dll'
    )) {
        if ($dependents -match [regex]::Escape($forbidden)) {
            throw "$dll imports forbidden desktop library $forbidden."
        }
    }
    Copy-Item -LiteralPath $built -Destination (Join-Path $destinationBin $dll) -Force
}

Copy-Item -LiteralPath (Join-Path $sourceFull 'COPYING.LGPLv2.1') `
    -Destination (Join-Path $destinationRoot 'LICENSE.txt') -Force

Write-Host "FFmpeg UWP runtime staged at $destinationBin"
Get-ChildItem -LiteralPath $destinationBin -Filter '*.dll' |
    Sort-Object Name |
    Get-FileHash -Algorithm SHA256 |
    Format-Table @{ Name = 'Name'; Expression = { Split-Path $_.Path -Leaf } }, Hash
