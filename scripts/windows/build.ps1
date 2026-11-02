# Run in a VS v143 x64 Native Tools environment. No replacement upstream checkout.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$root = Split-Path $repo
$logs = Join-Path $root 'build-logs'
New-Item -ItemType Directory -Force $logs | Out-Null
function Run-Step([string]$Name, [scriptblock]$Command) {
    Write-Host "=== $Name ==="
    # Fail on native exit codes too; PowerShell's ErrorActionPreference is not enough.
    & $Command 2>&1 | Tee-Object (Join-Path $logs "$Name.log") | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$Name failed: exit $LASTEXITCODE" }
}
if ($env:Platform -ne 'x64') { throw 'Run vcvars64.bat -vcvars_ver=14.44 first' }
if (!$env:AYU_PYTHON_SDK) { throw 'AYU_PYTHON_SDK must point to the full CPython 3.12.10 x64 SDK' }
$sdk = $env:AYU_PYTHON_SDK
foreach ($file in @('python.exe', 'include/Python.h', 'libs/python312.lib')) {
    if (!(Test-Path (Join-Path $sdk $file))) { throw "Missing CPython SDK file: $file" }
}
if (!$env:TDESKTOP_API_ID -or !$env:TDESKTOP_API_HASH) { throw 'Set TDESKTOP_API_ID and TDESKTOP_API_HASH' }
$runtime = Join-Path $root 'PythonRuntime/3.12.10-amd64'
$zip = Join-Path $root 'PythonRuntime/python-3.12.10-embed-amd64.zip'
New-Item -ItemType Directory -Force (Split-Path $zip) | Out-Null
$expected = '4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3'
if (!(Test-Path $zip)) {
    Invoke-WebRequest 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip' -OutFile $zip
}
if ((Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'CPython runtime SHA256 mismatch; remove the cached archive and retry'
}
New-Item -ItemType Directory -Force $runtime | Out-Null
Expand-Archive -LiteralPath $zip -DestinationPath $runtime -Force
# PyConfig supplies explicit paths. Do not let the embeddable package's _pth override them.
Remove-Item (Join-Path $runtime 'python312._pth') -ErrorAction SilentlyContinue
$inno = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
if (!(Test-Path $inno)) {
    Run-Step 'install-inno' { choco install innosetup '--version=6.4.3' -y '--no-progress' }
}
if (!(Test-Path $inno)) { throw 'Inno Setup 6 ISCC.exe not found' }
Push-Location $root
try {
    Run-Step 'prepare' { & (Join-Path $repo 'Telegram/build/prepare/win.bat') qt6 silent }
    # Configure with an explicit generator; upstream's wrapper does not accept an
    # additional positional x64 alongside -G. All source hooks are in this checkout.
    Push-Location (Join-Path $repo 'Telegram')
    try {
        Run-Step 'configure' {
            & ./configure.bat qt6 '-GVisual Studio 17 2022' '-Ax64' '-Tv143,version=14.44' `
                "-DTDESKTOP_API_ID=$env:TDESKTOP_API_ID" "-DTDESKTOP_API_HASH=$env:TDESKTOP_API_HASH" `
                "-DPython3_ROOT_DIR=$sdk" "-DPython3_EXECUTABLE=$sdk/python.exe" `
                "-DAYU_PYTHON_RUNTIME_DIR=$runtime" '-DCMAKE_CXX_FLAGS=/FS' `
                '-DCMAKE_SYSTEM_VERSION=10.0.26100.0' '-DDESKTOP_APP_DISABLE_AUTOUPDATE=ON'
        }
    } finally { Pop-Location }
    $out = Join-Path $repo 'out'
    Run-Step 'compile' { cmake --build $out --config Release --target Telegram --parallel 4 }
    $release = Join-Path $out 'Release'
    $stage = Join-Path $root 'package/AyuGram-Plugins'
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force $stage | Out-Null
    foreach ($name in @('AyuGram.exe','python312.dll','vcruntime140.dll','vcruntime140_1.dll','python','plugin-sdk','plugin-examples')) {
        $source = Join-Path $release $name
        if (!(Test-Path $source)) { throw "Required package payload missing: $source" }
        Copy-Item $source $stage -Recurse -Force
    }
    foreach ($name in @('modules','Updater.exe')) {
        $source = Join-Path $release $name
        if (Test-Path $source) { Copy-Item $source $stage -Recurse -Force }
    }
    Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $stage 'LICENSE-AyuGram.txt')
    Copy-Item (Join-Path $repo 'LEGAL') (Join-Path $stage 'LEGAL-AyuGram.txt')
    Copy-Item (Join-Path $repo 'docs/PLUGINS.md') (Join-Path $stage 'PLUGINS.md')
    $artifacts = Join-Path $root 'artifacts'
    if (Test-Path $artifacts) { Remove-Item -Recurse -Force $artifacts }
    New-Item -ItemType Directory -Force $artifacts | Out-Null
    $line = Get-Content (Join-Path $repo 'Telegram/build/version') | Where-Object { $_ -match '^AppVersionStr\s+' }
    $version = ($line -split '\s+')[1]
    Run-Step 'installer' {
        & $inno "/DAppVersion=$version" "/DPayload=$stage" "/DArtifacts=$artifacts" `
            (Join-Path $repo 'scripts/windows/installer.iss')
    }
    Compress-Archive -Path "$stage/*" -DestinationPath (Join-Path $artifacts 'AyuGram-Plugins-windows-x64-portable.zip') -Force
    Get-ChildItem $artifacts -File | ForEach-Object {
        $hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $($_.Name)"
    } | Set-Content (Join-Path $artifacts 'SHA256SUMS.txt') -Encoding ascii
} finally { Pop-Location }
