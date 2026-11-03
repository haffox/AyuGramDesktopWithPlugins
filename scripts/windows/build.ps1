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
    # Dependencies use Ninja: a VS-only instance selector must not leak into them.
    Remove-Item Env:CMAKE_GENERATOR_INSTANCE -ErrorAction SilentlyContinue
    Run-Step 'prepare' { & (Join-Path $repo 'Telegram/build/prepare/win.bat') qt6 silent }
    # Run CMake directly. The upstream Python wrapper concatenates a shell command;
    # a VSINSTALLDIR ending in backslash can consume subsequent quoted arguments.
    # Always supply separate, explicit source/build paths and an argument array.
    $out = Join-Path $repo 'out'
    $sourceCmake = Join-Path $repo 'CMakeLists.txt'
    if (!(Test-Path $sourceCmake)) { throw "Source CMakeLists.txt missing: $sourceCmake" }
    $qtVersions = @(Select-String -Path (Join-Path $repo 'Telegram/build/qt_version.py') `
        -Pattern "os.environ\['QT'\]\s*=\s*'(6\.[^']+)'" | ForEach-Object {
            $_.Matches[0].Groups[1].Value
        } | Sort-Object -Unique)
    if ($qtVersions.Count -ne 1) { throw 'Cannot resolve a unique Qt 6 version from the pinned upstream source' }
    $env:QT = $qtVersions[0]
    $qtConfig = Join-Path $root "Libraries/win64/Qt-$env:QT/lib/cmake/Qt6/Qt6Config.cmake"
    if (!(Test-Path $qtConfig)) { throw "Prepared Qt 6 configuration missing: $qtConfig" }
    $vs = $env:VSINSTALLDIR
    if (!$vs) { throw 'VSINSTALLDIR is missing; initialize vcvars64.bat first' }
    $vs = $vs.TrimEnd([char[]]"\/")
    $configureArgs = @(
        '-S', $repo, '-B', $out,
        '-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'v143,version=14.44',
        "-DCMAKE_GENERATOR_INSTANCE=$vs",
        "-DTDESKTOP_API_ID=$env:TDESKTOP_API_ID",
        "-DTDESKTOP_API_HASH=$env:TDESKTOP_API_HASH",
        "-DPython3_ROOT_DIR=$sdk",
        "-DPython3_EXECUTABLE=$sdk/python.exe",
        "-DAYU_PYTHON_RUNTIME_DIR=$runtime",
        '-DCMAKE_CXX_FLAGS=/FS',
        '-DCMAKE_SYSTEM_VERSION=10.0.26100.0',
        '-DDESKTOP_APP_DISABLE_AUTOUPDATE=ON'
    )
    Write-Host "CMake source: $repo; build: $out; Qt: $env:QT; VS: $vs"
    Run-Step 'configure' { & cmake @configureArgs }
    if (!(Test-Path (Join-Path $out 'Telegram.sln'))) {
        throw 'Configuration returned success but the Visual Studio solution was not generated'
    }
    Run-Step 'compile' { cmake --build $out --config Release --target Telegram --parallel 4 }
    $release = Join-Path $out 'Release'
    $stage = Join-Path $root 'package/Veyra'
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force $stage | Out-Null
    foreach ($name in @('Veyra.exe','python312.dll','vcruntime140.dll','vcruntime140_1.dll','python','plugin-sdk','plugin-examples')) {
        $source = Join-Path $release $name
        if (!(Test-Path $source)) { throw "Required package payload missing: $source" }
        Copy-Item $source $stage -Recurse -Force
    }
    foreach ($name in @('modules','Updater.exe')) {
        $source = Join-Path $release $name
        if (Test-Path $source) { Copy-Item $source $stage -Recurse -Force }
    }
    $exe = Join-Path $stage 'Veyra.exe'
    if ((Get-Item $exe).Length -lt 1048576) { throw 'Veyra.exe is unexpectedly small; refusing to package' }
    Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $stage 'LICENSE.txt')
    Copy-Item (Join-Path $repo 'LEGAL') (Join-Path $stage 'LEGAL.txt')
    Copy-Item (Join-Path $repo 'NOTICE-VEYRA.md') (Join-Path $stage 'NOTICE-VEYRA.md')
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
    Compress-Archive -Path "$stage/*" -DestinationPath (Join-Path $artifacts 'Veyra-windows-x64-portable.zip') -Force
    $installers = @(Get-ChildItem $artifacts -Filter '*-setup.exe' -File)
    if ($installers.Count -ne 1) { throw 'Exactly one Windows installer must be produced' }
    Get-ChildItem $artifacts -File | ForEach-Object {
        $hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $($_.Name)"
    } | Set-Content (Join-Path $artifacts 'SHA256SUMS.txt') -Encoding ascii
} finally { Pop-Location }
