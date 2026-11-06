# Native Windows build: DDK LKMs -> ksuinit -> YukiZygisk -> ksud -> Manager App
# Signing env: YUKISU_KEYSTORE, YUKISU_KEYSTORE_PASSWORD, YUKISU_KEY_ALIAS, YUKISU_KEY_PASSWORD
# Usage: .\scripts\build.bat [-k KMI] [--clean] [--yukizygisk|--yukizygisk-off] [--skip-lkm] [-i] [-h]
# Without --kmi, all supported LKM targets are built and embedded in ksud.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Show-Usage {
    @'
YukiSU native Windows build

Usage:
  .\scripts\build.bat [options]

Options:
  -k, --kmi KMI                 Build only one KMI (default: all supported KMIs)
  --clean                       Delete Native CMake build directories first
  --skip-lkm                    Reuse existing out\<KMI>_kernelsu.ko files
  --yukizygisk                  Enable YukiZygisk kernel support (default)
  --yukizygisk-off              Disable YukiZygisk kernel hooks
  -i, --install                 Install the resulting APK with adb
  -h, --help                    Show this help

Environment overrides:
  ANDROID_SDK_ROOT / ANDROID_HOME
  ANDROID_NDK_HOME
  JAVA_HOME
  PYTHON
  YUKISU_DDK_IMAGE             DDK image override; use {KMI} for multi-KMI builds
'@ | Write-Host
}

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [string]$WorkingDirectory
    )

    $displayArguments = @($ArgumentList | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    })
    Write-Host ('  > ' + $FilePath + $(if ($displayArguments.Count) { ' ' + ($displayArguments -join ' ') } else { '' })) -ForegroundColor DarkGray

    $oldLocation = Get-Location
    $oldErrorActionPreference = $ErrorActionPreference
    try {
        if ($WorkingDirectory) {
            Set-Location -LiteralPath $WorkingDirectory
        }
        # Windows PowerShell surfaces native stderr as ErrorRecord objects. Keep
        # compiler diagnostics visible, but use the process exit code as truth.
        $ErrorActionPreference = 'Continue'
        & $FilePath @ArgumentList
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldErrorActionPreference
        Set-Location -LiteralPath $oldLocation
    }

    if ($exitCode -ne 0) {
        throw "Command failed with exit code ${exitCode}: $FilePath"
    }
}

function Assert-SafeBuildDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)

    $fullPath = [IO.Path]::GetFullPath($Path)
    $repoPrefix = [IO.Path]::GetFullPath($script:RepoRoot).TrimEnd('\') + '\'
    if (-not $fullPath.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a build directory outside the repository: $fullPath"
    }
    if ((Split-Path -Leaf $fullPath) -notmatch '^build(?:-[A-Za-z0-9._-]+)?$') {
        throw "Refusing to clean a directory not named like 'build': $fullPath"
    }
}

function Reset-BuildDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)

    Assert-SafeBuildDirectory $Path
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

function Resolve-AndroidSdk {
    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($candidate in @($env:ANDROID_SDK_ROOT, $env:ANDROID_HOME)) {
        if ($candidate) { $candidates.Add($candidate) }
    }

    $localProperties = Join-Path $script:RepoRoot 'manager\local.properties'
    if (Test-Path -LiteralPath $localProperties) {
        $sdkLine = Get-Content -LiteralPath $localProperties | Where-Object { $_ -match '^sdk\.dir=(.+)$' } | Select-Object -First 1
        if ($sdkLine -and $sdkLine -match '^sdk\.dir=(.+)$') {
            $localSdk = $Matches[1] -replace '\\:', ':' -replace '\\\\', '\'
            $candidates.Add($localSdk)
        }
    }

    if ($env:LOCALAPPDATA) { $candidates.Add((Join-Path $env:LOCALAPPDATA 'Android\Sdk')) }
    $candidates.Add('D:\Softwares\AndroidSDK')

    foreach ($candidate in $candidates | Select-Object -Unique) {
        if ($candidate -and
            (Test-Path -LiteralPath (Join-Path $candidate 'platforms')) -and
            (Test-Path -LiteralPath (Join-Path $candidate 'ndk'))) {
            return [IO.Path]::GetFullPath($candidate)
        }
    }
    throw 'Android SDK not found. Set ANDROID_SDK_ROOT or ANDROID_HOME.'
}

function Resolve-Python {
    $candidates = New-Object System.Collections.Generic.List[string]
    if ($env:PYTHON) { $candidates.Add($env:PYTHON) }

    foreach ($name in @('python3.exe', 'python.exe')) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($command -and $command.Source) { $candidates.Add($command.Source) }
    }

    if ($env:LOCALAPPDATA) {
        $pythonRoot = Join-Path $env:LOCALAPPDATA 'Programs\Python'
        if (Test-Path -LiteralPath $pythonRoot) {
            Get-ChildItem -LiteralPath $pythonRoot -Directory -Filter 'Python*' -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending |
                ForEach-Object {
                    $candidate = Join-Path $_.FullName 'python.exe'
                    if (Test-Path -LiteralPath $candidate) { $candidates.Add($candidate) }
                }
        }
    }

    foreach ($candidate in $candidates | Select-Object -Unique) {
        if (-not (Test-Path -LiteralPath $candidate)) { continue }
        try {
            $oldPreference = $ErrorActionPreference
            $ErrorActionPreference = 'SilentlyContinue'
            $version = & $candidate --version 2>&1
            $exitCode = $LASTEXITCODE
            $ErrorActionPreference = $oldPreference
            if ($exitCode -eq 0 -and $version -match '^Python 3\.') {
                return [IO.Path]::GetFullPath($candidate)
            }
        }
        catch {
            $ErrorActionPreference = $oldPreference
        }
    }
    throw 'A real Python 3 interpreter was not found. Set PYTHON to python.exe.'
}

function Resolve-JavaHome {
    $candidates = New-Object System.Collections.Generic.List[string]
    if ($env:JAVA_HOME) { $candidates.Add($env:JAVA_HOME) }
    $candidates.Add('D:\Softwares\AndroidStudio\jbr')

    foreach ($candidate in $candidates | Select-Object -Unique) {
        if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'bin\java.exe'))) {
            return [IO.Path]::GetFullPath($candidate)
        }
    }
    throw 'JDK not found. Set JAVA_HOME (JDK 17 or newer; JDK 21 is recommended).'
}

function Build-CMakeProject {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$SourceDirectory,
        [int]$AndroidApi = 31,
        [string]$AndroidAbi = $script:AndroidAbi,
        [string]$BuildDirectoryName = 'build',
        [switch]$NeedsPython
    )

    $buildDirectory = Join-Path $SourceDirectory $BuildDirectoryName
    Write-Host ">>> Build $Name ..." -ForegroundColor Cyan
    if ($script:CleanBuild) {
        Reset-BuildDirectory $buildDirectory
    }
    else {
        New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
    }

    $configureArguments = @(
        '-S', $SourceDirectory,
        '-B', $buildDirectory,
        '-G', 'Ninja',
        "-DCMAKE_TOOLCHAIN_FILE=$script:NdkToolchainFile",
        "-DCMAKE_MAKE_PROGRAM=$script:NinjaExe",
        "-DANDROID_ABI=$AndroidAbi",
        "-DANDROID_PLATFORM=android-$AndroidApi",
        '-DCMAKE_BUILD_TYPE=Release',
        '-DYUKISU_ENABLE_CLANG_TIDY=OFF'
    )
    if ($NeedsPython) {
        $configureArguments += "-DPython3_EXECUTABLE=$script:PythonExe"
    }
    Invoke-Native -FilePath $script:CMakeExe -ArgumentList $configureArguments
    Invoke-Native -FilePath $script:CMakeExe -ArgumentList @('--build', $buildDirectory, '--parallel', [string]$script:BuildJobs)
}

function Copy-RequiredFile {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Expected build output was not produced: $Source"
    }
    $destinationDirectory = Split-Path -Parent $Destination
    if ($destinationDirectory) {
        New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

try {
    $script:RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    # Keep this list in sync with build-lkm.yml and ksud.yml.
    $supportedKmis = @(
        'android12-5.10',
        'android13-5.10',
        'android13-5.15',
        'android14-5.15',
        'android14-6.1',
        'android15-6.6',
        'android16-6.12',
        'android17-6.18'
    )
    $kmiTargets = @($supportedKmis)
    $ddkRelease = '20260828'
    $script:AndroidAbi = 'arm64-v8a'
    $script:CleanBuild = $false
    $skipLkm = $false
    $installApk = $false
    $enableYukiZygisk = $true

    for ($index = 0; $index -lt $args.Count; $index++) {
        $argument = [string]$args[$index]
        switch ($argument.ToLowerInvariant()) {
            { $_ -in @('-k', '--kmi') } {
                if ($index + 1 -ge $args.Count) { throw "$argument requires a value" }
                $index++
                $kmiTargets = @([string]$args[$index])
                break
            }
            '--clean' { $script:CleanBuild = $true; break }
            '--skip-lkm' { $skipLkm = $true; break }
            '--yukizygisk' { $enableYukiZygisk = $true; break }
            '--yukizygisk-off' { $enableYukiZygisk = $false; break }
            { $_ -in @('-i', '--install') } { $installApk = $true; break }
            { $_ -in @('-h', '--help') } { Show-Usage; exit 0 }
            default { throw "Unknown option: $argument" }
        }
    }

    foreach ($kmi in $kmiTargets) {
        if ($kmi -notmatch '^[A-Za-z0-9._-]+$') {
            throw "Invalid KMI/DDK target: $kmi"
        }
    }

    $sdkRoot = Resolve-AndroidSdk
    $versionCatalog = Get-Content -LiteralPath (Join-Path $script:RepoRoot 'manager\gradle\libs.versions.toml') -Raw
    if ($versionCatalog -notmatch '(?m)^ndk\s*=\s*"([^"]+)"') {
        throw 'Cannot read the required NDK version from manager/gradle/libs.versions.toml.'
    }
    $requiredNdkVersion = $Matches[1]

    $ndkRoot = $env:ANDROID_NDK_HOME
    if (-not $ndkRoot) { $ndkRoot = Join-Path $sdkRoot "ndk\$requiredNdkVersion" }
    $ndkRoot = [IO.Path]::GetFullPath($ndkRoot)
    $script:NdkToolchainFile = Join-Path $ndkRoot 'build\cmake\android.toolchain.cmake'
    $ndkBin = Join-Path $ndkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin'
    if (-not (Test-Path -LiteralPath $script:NdkToolchainFile) -or -not (Test-Path -LiteralPath $ndkBin)) {
        throw "Windows NDK $requiredNdkVersion not found at $ndkRoot"
    }

    $cmakeInstallations = @(Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'cmake') -Directory -ErrorAction SilentlyContinue |
        Sort-Object @{ Expression = {
            try { [version]$_.Name } catch { [version]'0.0' }
        } } -Descending)
    $cmakeInstallation = $cmakeInstallations | Where-Object {
        (Test-Path -LiteralPath (Join-Path $_.FullName 'bin\cmake.exe')) -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'bin\ninja.exe'))
    } | Select-Object -First 1
    if (-not $cmakeInstallation) { throw "Android SDK CMake/Ninja not found under $sdkRoot\cmake" }
    $script:CMakeExe = Join-Path $cmakeInstallation.FullName 'bin\cmake.exe'
    $script:NinjaExe = Join-Path $cmakeInstallation.FullName 'bin\ninja.exe'

    $platform37 = Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'platforms') -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^android-37(?:\.|$)' } |
        Select-Object -First 1
    if (-not $platform37) { throw 'Android SDK Platform 37 is required by the Manager build.' }
    if (-not (Test-Path -LiteralPath (Join-Path $sdkRoot 'build-tools\36.1.0'))) {
        throw 'Android SDK Build Tools 36.1.0 is required by the Manager build.'
    }

    $script:PythonExe = Resolve-Python
    $javaHome = Resolve-JavaHome
    $script:BuildJobs = if ($env:NUMBER_OF_PROCESSORS -match '^\d+$') { [int]$env:NUMBER_OF_PROCESSORS } else { 8 }

    $env:ANDROID_HOME = $sdkRoot
    $env:ANDROID_SDK_ROOT = $sdkRoot
    $env:ANDROID_NDK_HOME = $ndkRoot
    $env:JAVA_HOME = $javaHome
    $pythonDirectory = Split-Path -Parent $script:PythonExe
    $cmakeBin = Split-Path -Parent $script:CMakeExe
    $platformTools = Join-Path $sdkRoot 'platform-tools'
    $env:PATH = "$pythonDirectory;$cmakeBin;$ndkBin;$platformTools;$env:PATH"

    Write-Host '=== YukiSU native Windows build ===' -ForegroundColor Green
    Write-Host "KMIs:      $($kmiTargets -join ', ')"
    Write-Host "ABI:       $script:AndroidAbi"
    Write-Host "SDK:       $sdkRoot"
    Write-Host "NDK:       $ndkRoot"
    Write-Host "CMake:     $script:CMakeExe"
    Write-Host "Ninja:     $script:NinjaExe"
    Write-Host "Python:    $script:PythonExe"
    Write-Host "JAVA_HOME: $javaHome"
    Write-Host "Jobs:      $script:BuildJobs"
    Write-Host "Native cache: $(if ($script:CleanBuild) { 'clean rebuild' } else { 'reuse build directories' })"
    if ($enableYukiZygisk) {
        Write-Host 'YukiZygisk kernel hooks: enabled'
    }
    else {
        Write-Host 'YukiZygisk kernel hooks: disabled'
    }
    Write-Host ''

    $outDirectory = Join-Path $script:RepoRoot 'out'
    New-Item -ItemType Directory -Path $outDirectory -Force | Out-Null
    $lkmOutputs = @($kmiTargets | ForEach-Object {
        Join-Path $outDirectory "$($_)_kernelsu.ko"
    })

    if (-not $skipLkm) {
        & $script:PythonExe (Join-Path $script:RepoRoot 'userspace/zygisk/inject/generate.py') --ndk $ndkRoot
        if ($LASTEXITCODE -ne 0) { throw 'Native injection images are stale or invalid.' }
        Write-Host '>>> [1/5] Build KernelSU LKMs (DDK) ...' -ForegroundColor Cyan
        $dockerCommand = Get-Command docker.exe -ErrorAction SilentlyContinue
        if ($dockerCommand) {
            $dockerExe = $dockerCommand.Source
        }
        else {
            $defaultDocker = 'C:\Program Files\Docker\Docker\resources\bin\docker.exe'
            if (Test-Path -LiteralPath $defaultDocker) {
                $dockerExe = $defaultDocker
            }
            else {
                throw 'Docker Desktop docker.exe is not available in PATH.'
            }
        }

        $kernelDirectory = Join-Path $script:RepoRoot 'kernel'
        $llvmStrip = Join-Path $ndkBin 'llvm-strip.exe'

        # Windows worktree gitdir paths cannot be resolved inside Linux containers.
        $commitCount = (& git -C $script:RepoRoot rev-list --count HEAD).Trim()
        if ($LASTEXITCODE -ne 0 -or $commitCount -notmatch '^\d+$') { throw 'Cannot determine the LKM revision count.' }
        $revision = (& git -C $script:RepoRoot rev-parse --short=8 HEAD).Trim()
        if ($LASTEXITCODE -ne 0 -or $revision -notmatch '^[a-f0-9]{8,40}$') { throw 'Cannot determine the LKM revision.' }
        $versionTag = (& git -C $script:RepoRoot describe --tags --always).Trim()
        if ($LASTEXITCODE -ne 0 -or $versionTag -notmatch '^v?(\d+\.\d+\.\d+)') { throw 'Cannot determine the LKM version tag.' }
        $versionApi = $Matches[1]
        $kernelChanges = @(& git -C $script:RepoRoot -c core.autocrlf=true status --porcelain --untracked-files=normal -- kernel uapi)
        if ($LASTEXITCODE -ne 0) { throw 'Cannot determine the LKM source state.' }
        $dirtySuffix = if ($kernelChanges.Count) { '-nc' } else { '' }
        $lkmVersion = 10000 + [int]$commitCount - 3135
        $lkmFullVersion = "v${versionApi}-${revision}${dirtySuffix}@YukiSU"

        foreach ($kmi in $kmiTargets) {
            Write-Host "    Building LKM: $kmi"
            if ($env:YUKISU_DDK_IMAGE) {
                if ($kmiTargets.Count -gt 1 -and -not $env:YUKISU_DDK_IMAGE.Contains('{KMI}')) {
                    throw 'YUKISU_DDK_IMAGE must contain {KMI} for a multi-KMI build.'
                }
                $ddkImage = $env:YUKISU_DDK_IMAGE.Replace('{KMI}', $kmi).Replace('{DDK_RELEASE}', $ddkRelease)
            }
            else {
                $ddkImage = "ghcr.io/ylarod/ddk-min:${kmi}-${ddkRelease}"
            }

            $makeArguments = @(
                'KCFLAGS=-I/build',
                'CONFIG_KSU=m',
                'CONFIG_KSU_SUPERKEY=y'
                "KSU_VERSION=$lkmVersion"
                "KSU_VERSION_API=$versionApi"
                "KSU_VERSION_FULL=$lkmFullVersion"
                "KSU_LOCAL_VERSION=$commitCount"
                'KSU_GITHUB_VERSION='
                "KSU_GIT_DIRTY_SUFFIX=$dirtySuffix"
            )
            if ($enableYukiZygisk) {
                $makeArguments += 'CONFIG_KSU_YUKIZYGISK=y'
            }
            $makeArguments += @('CC=clang', "-j$script:BuildJobs")
            $makeCommand = 'make -f scripts/ddk.mk clean && make -f scripts/ddk.mk ' + ($makeArguments -join ' ')
            $dockerArguments = @(
                'run',
                '--platform', 'linux/amd64',
                '--rm',
                '-v', "${script:RepoRoot}:/build",
                '-w', '/build',
                $ddkImage,
                'bash', '-lc', $makeCommand
            )
            Invoke-Native -FilePath $dockerExe -ArgumentList $dockerArguments -WorkingDirectory $script:RepoRoot

            $lkmOutput = Join-Path $outDirectory "${kmi}_kernelsu.ko"
            Copy-RequiredFile -Source (Join-Path $kernelDirectory 'kernelsu.ko') -Destination $lkmOutput
            if (Test-Path -LiteralPath $llvmStrip) {
                try { Invoke-Native -FilePath $llvmStrip -ArgumentList @('-d', $lkmOutput) }
                catch { Write-Warning "Could not strip the LKM: $($_.Exception.Message)" }
            }
            Write-Host "    LKM: $lkmOutput"
        }
    }
    else {
        Write-Host '>>> [1/5] Skip LKM builds; reuse outputs' -ForegroundColor Cyan
    }

    $ksuinitDirectory = Join-Path $script:RepoRoot 'userspace\ksuinit'
    $zygiskDirectory = Join-Path $script:RepoRoot 'userspace\zygisk\core'
    $zygiskDaemonDirectory = Join-Path $script:RepoRoot 'userspace\zygisk\daemon'
    $ksudDirectory = Join-Path $script:RepoRoot 'userspace\ksud'
    $assetsDirectory = Join-Path $ksudDirectory 'assets'
    New-Item -ItemType Directory -Path $assetsDirectory -Force | Out-Null

    $generatedAssets = @(
        'ksuinit', 'su', 'zygiskd64', 'zygiskd32',
        'libzygisk64.so', 'libzygisk32.so',
        'libyloader64.so', 'libyloader32.so',
        'libyukizncore64.so', 'libyukizncore32.so',
        'libzygisk.so', 'libyukilinker.so', 'libyukilinker32.so',
        'libyukilinker64.so', 'libyukizncore.so'
    )
    foreach ($generatedAsset in $generatedAssets) {
        $assetPath = Join-Path $assetsDirectory $generatedAsset
        if (Test-Path -LiteralPath $assetPath) { Remove-Item -LiteralPath $assetPath -Force }
    }
    Get-ChildItem -LiteralPath $assetsDirectory -File -Filter '*.ko' -ErrorAction SilentlyContinue | Remove-Item -Force

    Build-CMakeProject -Name 'ksuinit' -SourceDirectory $ksuinitDirectory
    Copy-RequiredFile -Source (Join-Path $ksuinitDirectory 'build\ksuinit') -Destination (Join-Path $assetsDirectory 'ksuinit')

    Build-CMakeProject -Name 'YukiZygisk payload' -SourceDirectory $zygiskDirectory
    Copy-RequiredFile -Source (Join-Path $zygiskDirectory 'build\libzygisk64.so') -Destination (Join-Path $assetsDirectory 'libzygisk64.so')
    Copy-RequiredFile -Source (Join-Path $zygiskDirectory 'build\libyloader64.so') -Destination (Join-Path $assetsDirectory 'libyloader64.so')
    Copy-RequiredFile -Source (Join-Path $zygiskDirectory 'build\libyukizncore64.so') -Destination (Join-Path $assetsDirectory 'libyukizncore64.so')
    Build-CMakeProject -Name 'zygiskd64' -SourceDirectory $zygiskDaemonDirectory -AndroidApi 31
    Copy-RequiredFile -Source (Join-Path $zygiskDaemonDirectory 'build\zygiskd64') -Destination (Join-Path $assetsDirectory 'zygiskd64')
    Build-CMakeProject -Name 'YukiZygisk armv7 core' -SourceDirectory $zygiskDirectory `
        -AndroidAbi 'armeabi-v7a' -BuildDirectoryName 'build-armv7'
    Copy-RequiredFile -Source (Join-Path $zygiskDirectory 'build-armv7\libzygisk32.so') -Destination (Join-Path $assetsDirectory 'libzygisk32.so')
    Copy-RequiredFile -Source (Join-Path $zygiskDirectory 'build-armv7\libyloader32.so') -Destination (Join-Path $assetsDirectory 'libyloader32.so')
    Copy-RequiredFile -Source (Join-Path $zygiskDirectory 'build-armv7\libyukizncore32.so') -Destination (Join-Path $assetsDirectory 'libyukizncore32.so')
    Build-CMakeProject -Name 'zygiskd32' -SourceDirectory $zygiskDaemonDirectory `
        -AndroidApi 31 -AndroidAbi 'armeabi-v7a' -BuildDirectoryName 'build-armv7'
    Copy-RequiredFile -Source (Join-Path $zygiskDaemonDirectory 'build-armv7\zygiskd32') -Destination (Join-Path $assetsDirectory 'zygiskd32')
    Write-Host '    staged arm64/armv7 payloads + zygiskd64/zygiskd32'

    Write-Host '>>> [3/5] Build ksud ...' -ForegroundColor Cyan
    foreach ($lkmOutput in $lkmOutputs) {
        if (-not (Test-Path -LiteralPath $lkmOutput -PathType Leaf)) {
            throw "Required LKM was not produced: $lkmOutput"
        }
        Copy-Item -LiteralPath $lkmOutput -Destination (Join-Path $assetsDirectory (Split-Path -Leaf $lkmOutput)) -Force
    }
    Write-Host "    staged LKMs: $($kmiTargets -join ', ')"
    Build-CMakeProject -Name 'ksud' -SourceDirectory $ksudDirectory -AndroidApi 31 -NeedsPython
    $ksudOutput = Join-Path $ksudDirectory 'build\ksud'
    if (-not (Test-Path -LiteralPath $ksudOutput)) { throw "Expected build output was not produced: $ksudOutput" }
    $ksudSize = (Get-Item -LiteralPath $ksudOutput).Length
    Write-Host "    ksud: $ksudOutput ($ksudSize bytes)"

    Write-Host '>>> [4/5] Build Manager App ...' -ForegroundColor Cyan
    $busyboxDirectory = Join-Path $script:RepoRoot 'userspace\busybox'
    Build-CMakeProject -Name 'BusyBox' -SourceDirectory $busyboxDirectory -AndroidApi 31
    $managerDirectory = Join-Path $script:RepoRoot 'manager'
    $jniLibDirectory = Join-Path $managerDirectory "app\src\main\jniLibs\$script:AndroidAbi"
    Copy-RequiredFile -Source $ksudOutput -Destination (Join-Path $jniLibDirectory 'libksud.so')
    Copy-RequiredFile -Source (Join-Path $busyboxDirectory 'build\busybox') -Destination (Join-Path $jniLibDirectory 'libbusybox.so')

    $signingMap = @{
        'KEYSTORE_FILE' = 'YUKISU_KEYSTORE'
        'KEYSTORE_PASSWORD' = 'YUKISU_KEYSTORE_PASSWORD'
        'KEY_ALIAS' = 'YUKISU_KEY_ALIAS'
        'KEY_PASSWORD' = 'YUKISU_KEY_PASSWORD'
    }
    $presentSigningValues = 0
    foreach ($entry in $signingMap.GetEnumerator()) {
        $value = [Environment]::GetEnvironmentVariable($entry.Value)
        if ($value) {
            [Environment]::SetEnvironmentVariable($entry.Key, $value, 'Process')
            [Environment]::SetEnvironmentVariable("ORG_GRADLE_PROJECT_$($entry.Key)", $value, 'Process')
            $presentSigningValues++
        }
    }
    if ($presentSigningValues -ne 0 -and $presentSigningValues -ne $signingMap.Count) {
        throw 'Release signing requires all four YUKISU_KEYSTORE/YUKISU_KEYSTORE_PASSWORD/YUKISU_KEY_ALIAS/YUKISU_KEY_PASSWORD values.'
    }
    $releaseSigned = $presentSigningValues -eq $signingMap.Count

    $gradleWrapper = Join-Path $managerDirectory 'gradlew.bat'
    Invoke-Native -FilePath $gradleWrapper -ArgumentList @('assembleRelease', '--build-cache', '--no-daemon', "-PABI=$script:AndroidAbi") -WorkingDirectory $managerDirectory

    $apkDirectory = Join-Path $managerDirectory 'app\build\outputs\renamed_apk\release'
    $apk = Get-ChildItem -LiteralPath $apkDirectory -File -Filter '*.apk' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $apk) { throw "Gradle completed but no APK was found in $apkDirectory" }

    Write-Host ''
    Write-Host '=== Build complete ===' -ForegroundColor Green
    Write-Host "APK: $($apk.FullName)"
    if (-not $releaseSigned) {
        Write-Warning 'The Release APK is unsigned. Set the four YUKISU_* signing variables to produce an installable APK.'
    }
    Write-Host ''

    if ($installApk) {
        if (-not $releaseSigned) {
            throw 'Cannot install an unsigned Release APK. Configure the YUKISU_* signing variables first.'
        }
        Write-Host '>>> Install to device ...' -ForegroundColor Cyan
        $adbExe = Join-Path $platformTools 'adb.exe'
        Invoke-Native -FilePath $adbExe -ArgumentList @('install', '-r', $apk.FullName)
        Write-Host 'APK installed. Sync ksud from the app before reboot.'
    }
    else {
        Write-Host "Install: adb install -r `"$($apk.FullName)`""
        Write-Host 'After installing, sync ksud from the app before reboot.'
    }
}
catch {
    Write-Host ''
    Write-Host "BUILD FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
