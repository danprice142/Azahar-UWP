# Copyright Citra Emulator Project / Azahar Emulator Project
# Licensed under GPLv2 or any later version
# Refer to the license.txt file included.
#
# Builds a signed MSIX for the Xbox/UWP frontend.
#
#   .\package.ps1 -BuildDir C:\path\to\uwp-build
#   .\package.ps1 -BuildDir C:\path\to\uwp-build -PackageVersion 2126.0.0.0
#
# The output is AppPackages\Azahar_<version>.msix, named after the Identity version stamped into
# the package (the manifest's version unless -PackageVersion overrides it).
#
# The signing certificate's subject MUST match the Publisher in Package.appxmanifest exactly, or
# makeappx/signtool will produce a package the device refuses to install.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $BuildDir,
    [string] $Config    = 'Release',
    [string] $Publisher = 'CN=DanP142',
    # Four unsigned 16-bit components; defaults to the manifest's Identity version.
    [string] $PackageVersion = '',
    # The console refuses to register an unsigned package (TRUST_E_NOSIGNATURE).
    [switch] $NoSign
)

$ErrorActionPreference = 'Stop'
$distUwp = $PSScriptRoot
$repoRoot = Split-Path (Split-Path $distUwp -Parent) -Parent
$binDir = Join-Path $BuildDir "bin\$Config"
$layout = Join-Path $BuildDir 'AppPackages\layout'
$pfx    = Join-Path $BuildDir 'AppPackages\azahar-uwp.pfx'

function Get-SdkTool([string] $name) {
    $tool = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\$name" -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $tool) { throw "Could not find $name in the Windows SDK." }
    return $tool.FullName
}

# --- Assets ---------------------------------------------------------------------------------
# The manifest's tile images are generated from dist/azahar.png.
function New-TileAssets([string] $sourcePng, [string] $assetsDir) {
    Add-Type -AssemblyName System.Drawing
    $sizes = @{
        'StoreLogo.png'          = @(50, 50)
        'Square44x44Logo.png'    = @(44, 44)
        'Square150x150Logo.png'  = @(150, 150)
        'Wide310x150Logo.png'    = @(310, 150)
        'SplashScreen.png'       = @(620, 300)
    }
    $src = [System.Drawing.Image]::FromFile($sourcePng)
    try {
        foreach ($name in $sizes.Keys) {
            $w, $h = $sizes[$name]
            $bmp = New-Object System.Drawing.Bitmap($w, $h)
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            try {
                $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $g.Clear([System.Drawing.Color]::Transparent)
                # Preserve aspect ratio, centred -- the wide and splash tiles are not square.
                $scale = [Math]::Min($w / $src.Width, $h / $src.Height)
                $dw = [int]($src.Width * $scale)
                $dh = [int]($src.Height * $scale)
                $g.DrawImage($src, [int](($w - $dw) / 2), [int](($h - $dh) / 2), $dw, $dh)
            } finally { $g.Dispose() }
            $bmp.Save((Join-Path $assetsDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
            $bmp.Dispose()
        }
    } finally { $src.Dispose() }
}

# --- Layout ---------------------------------------------------------------------------------
Write-Host "==> Staging package layout at $layout"
if (Test-Path $layout) { Remove-Item -Recurse -Force $layout }
New-Item -ItemType Directory -Force -Path $layout, (Join-Path $layout 'Assets') | Out-Null

$exe = Join-Path $binDir 'citra_uwp.exe'
if (-not (Test-Path $exe)) { throw "citra_uwp.exe not found at $exe -- build it first." }
Copy-Item $exe $layout

foreach ($dll in @(
    'opengl32.dll',
    'libgallium_wgl.dll',
    'z-1.dll',
    'dxil.dll',
    'avcodec-61.dll',
    'avfilter-10.dll',
    'avformat-61.dll',
    'avutil-59.dll',
    'swresample-5.dll',
    'swscale-8.dll'
)) {
    $path = Join-Path $binDir $dll
    if (-not (Test-Path $path)) { throw "Required runtime DLL missing from the build: $dll" }
    Copy-Item $path $layout
}

$thirdPartyNotices = Join-Path $layout 'ThirdPartyNotices'
New-Item -ItemType Directory -Force -Path $thirdPartyNotices | Out-Null
$ffmpegLicense = Join-Path $repoRoot 'externals\uwp\ffmpeg\7.1\LICENSE.txt'
$ffmpegBuildInfo = Join-Path $repoRoot 'externals\uwp\ffmpeg\7.1\BUILD_INFO.txt'
if (-not (Test-Path $ffmpegLicense) -or -not (Test-Path $ffmpegBuildInfo)) {
    throw 'FFmpeg license or build provenance is missing from externals\uwp\ffmpeg\7.1.'
}
Copy-Item $ffmpegLicense (Join-Path $thirdPartyNotices 'FFmpeg-LICENSE.txt')
Copy-Item $ffmpegBuildInfo (Join-Path $thirdPartyNotices 'FFmpeg-BUILD_INFO.txt')

$mesaNoticeRoot = Join-Path $repoRoot 'externals\uwp\mesa'
$mesaLicenseRoot = Join-Path $mesaNoticeRoot 'licenses'
$mesaPackageNotices = Join-Path $thirdPartyNotices 'Mesa'
$mesaNoticeFiles = @('BUILD_INFO.txt', 'LICENSE.rst', 'ZLIB-LICENSE.txt')
foreach ($notice in $mesaNoticeFiles) {
    $path = Join-Path $mesaNoticeRoot $notice
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Mesa notice missing: $path"
    }
}
if (-not (Test-Path -LiteralPath $mesaLicenseRoot)) {
    throw "Mesa licence directory missing: $mesaLicenseRoot"
}
New-Item -ItemType Directory -Force -Path $mesaPackageNotices | Out-Null
foreach ($notice in $mesaNoticeFiles) {
    Copy-Item (Join-Path $mesaNoticeRoot $notice) $mesaPackageNotices
}
Copy-Item $mesaLicenseRoot (Join-Path $mesaPackageNotices 'licenses') -Recurse

$compatibilityList = Join-Path $repoRoot 'dist\compatibility_list\compatibility_list.json'
if (-not (Test-Path $compatibilityList)) {
    throw "Compatibility list missing: $compatibilityList"
}
Copy-Item $compatibilityList $layout

Copy-Item (Join-Path $distUwp 'Package.appxmanifest') (Join-Path $layout 'AppxManifest.xml')

$manifestPath = Join-Path $layout 'AppxManifest.xml'
$manifestText = Get-Content $manifestPath -Raw
$identityVersionPattern = '(?s)(<Identity\b.*?\bVersion=")\d+\.\d+\.\d+\.\d+(")'
$identityVersionValuePattern = '(?s)<Identity\b.*?\bVersion="(?<version>\d+\.\d+\.\d+\.\d+)"'
$templateVersionMatch = [regex]::Match($manifestText, $identityVersionValuePattern)
if (-not $templateVersionMatch.Success) {
    throw 'Package.appxmanifest does not contain a valid four-part Identity version.'
}

if ([string]::IsNullOrWhiteSpace($PackageVersion)) {
    $stampVersion = ([version]$templateVersionMatch.Groups['version'].Value).ToString(4)
} else {
    try {
        $requestedVersion = [version]$PackageVersion
    } catch {
        throw "PackageVersion '$PackageVersion' is not a valid version. Use four numeric parts."
    }
    $versionParts = @($requestedVersion.Major, $requestedVersion.Minor,
                      $requestedVersion.Build, $requestedVersion.Revision)
    if ($versionParts | Where-Object { $_ -lt 0 -or $_ -gt [uint16]::MaxValue }) {
        throw "PackageVersion '$PackageVersion' must contain four parts between 0 and 65535."
    }
    $stampVersion = $requestedVersion.ToString(4)
}
$manifestText = [regex]::Replace(
    $manifestText,
    $identityVersionPattern,
    { param($match) $match.Groups[1].Value + $stampVersion + $match.Groups[2].Value })
$manifestText | Set-Content $manifestPath -Encoding utf8
$msix = Join-Path $BuildDir "AppPackages\Azahar_$stampVersion.msix"
Write-Host "==> Package version $stampVersion"

New-TileAssets (Join-Path $repoRoot 'dist\azahar.png') (Join-Path $layout 'Assets')

# --- Resource index -------------------------------------------------------------------------
Write-Host '==> Building resource index'
$makepri = Get-SdkTool 'makepri.exe'
$priConfig = Join-Path $BuildDir 'AppPackages\priconfig.xml'   # kept outside the layout
& $makepri createconfig /cf $priConfig /dq en-US /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makepri createconfig failed ($LASTEXITCODE)" }
& $makepri new /pr $layout /cf $priConfig /of (Join-Path $layout 'resources.pri') /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makepri new failed ($LASTEXITCODE)" }

# --- Pack -----------------------------------------------------------------------------------
Write-Host '==> Building MSIX'
$makeappx = Get-SdkTool 'makeappx.exe'
if (Test-Path $msix) { Remove-Item -Force $msix }
& $makeappx pack /d $layout /p $msix /o /nv
if ($LASTEXITCODE -ne 0) { throw "makeappx failed ($LASTEXITCODE)" }

# --- Sign -----------------------------------------------------------------------------------
if ($NoSign) {
    Write-Host '==> Leaving package unsigned (NOTE: the devkit rejects this with TRUST_E_NOSIGNATURE)'
    Write-Host "==> Package ready: $msix"
    return
}

Write-Host '==> Signing'
if (-not (Test-Path $pfx)) {
    Write-Host "    Creating a self-signed certificate for $Publisher"
    $cert = New-SelfSignedCertificate -Type Custom -Subject $Publisher `
        -KeyUsage DigitalSignature -FriendlyName 'Azahar UWP (dev)' `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
    $pwd = ConvertTo-SecureString -String 'azahar' -Force -AsPlainText
    Export-PfxCertificate -Cert "Cert:\CurrentUser\My\$($cert.Thumbprint)" -FilePath $pfx -Password $pwd | Out-Null
    Export-Certificate -Cert "Cert:\CurrentUser\My\$($cert.Thumbprint)" `
        -FilePath ([IO.Path]::ChangeExtension($pfx, '.cer')) | Out-Null
}
$signtool = Get-SdkTool 'signtool.exe'
& $signtool sign /fd SHA256 /a /f $pfx /p 'azahar' $msix
if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)" }

Write-Host "==> Package ready: $msix"
