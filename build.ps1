param(
    [switch]$NoLaunch
)
$ErrorActionPreference = 'Stop'
$source = $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Build Tools with the C++ desktop workload are required.'
}
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$visualStudio) { throw 'MSVC x64 tools were not found.' }
$vcvars = Join-Path $visualStudio 'VC\Auxiliary\Build\vcvars64.bat'
$buildDir = Join-Path $source 'build'
New-Item -Type Directory -Force -Path $buildDir | Out-Null

# Safely stop only the main Vetra process and wait for its --restore-taskbar watchdog to finish restoring the taskbar cleanly
$running = Get-CimInstance Win32_Process -Filter "Name='Vetra.exe'" -ErrorAction SilentlyContinue |
    Where-Object { -not $_.CommandLine -or $_.CommandLine -notlike '*--restore-taskbar*' }
if ($running) {
    foreach ($proc in $running) {
        Stop-Process -Id $proc.ProcessId -ErrorAction SilentlyContinue
    }
    for ($i = 0; $i -lt 30; $i++) {
        if (-not (Get-Process -Name Vetra -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Milliseconds 100
    }
}

$batch = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$source"
rc /nologo /fo build\Vetra.res Vetra.rc
if errorlevel 1 exit /b 1
cl /nologo /MP /std:c++20 /utf-8 /EHsc /O2 /Oi /Ot /GL /Gw /Gy /favor:AMD64 /MT /W4 /wd4100 /wd4244 /wd4267 /wd4458 /wd4127 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fobuild\ main.cpp model.cpp canvas.cpp system_tray.cpp shell_taskbar.cpp views.cpp tiles.cpp timers.cpp editor.cpp widgets.cpp extra_views.cpp radios.cpp app_tools.cpp jump_list.cpp window_previews.cpp data_feeds.cpp reference_widgets.cpp pro_widgets.cpp productivity.cpp /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS /MANIFEST:NO /OUT:build\Vetra.exe build\Vetra.res user32.lib gdi32.lib gdiplus.lib d2d1.lib dwrite.lib shell32.lib shlwapi.lib advapi32.lib dwmapi.lib winmm.lib winhttp.lib iphlpapi.lib ole32.lib oleaut32.lib oleacc.lib powrprof.lib pdh.lib runtimeobject.lib windowsapp.lib wlanapi.lib delayimp.lib /DELAYLOAD:wlanapi.dll /DELAYLOAD:winhttp.dll /DELAYLOAD:iphlpapi.dll /DELAYLOAD:pdh.dll
"@
$script = Join-Path $buildDir 'compile.cmd'
Set-Content -LiteralPath $script -Value $batch -Encoding Default
& cmd.exe /d /c $script
if ($LASTEXITCODE -ne 0) { throw "Vetra build failed ($LASTEXITCODE)." }

# Keep the Desktop shortcut/icon synced with the latest build\Vetra.exe
try {
    $desktop = [Environment]::GetFolderPath('Desktop')
    $exePath = Join-Path $buildDir 'Vetra.exe'
    $icoPath = Join-Path $source 'Vetra.ico'
    if ($desktop -and (Test-Path -LiteralPath $desktop)) {
        $desktopExe = Join-Path $desktop 'Vetra.exe'
        if (Test-Path -LiteralPath $desktopExe) {
            Copy-Item -LiteralPath $exePath -Destination $desktopExe -Force
        }
        $desktopLnk = Join-Path $desktop 'Vetra.lnk'
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $shell.CreateShortcut($desktopLnk)
        $shortcut.TargetPath = $exePath
        $shortcut.WorkingDirectory = $buildDir
        if (Test-Path -LiteralPath $icoPath) {
            $shortcut.IconLocation = "$icoPath,0"
        } else {
            $shortcut.IconLocation = "$exePath,0"
        }
        $shortcut.Save()
    }
} catch {
    Write-Warning "Masaüstü kısayolu güncellenemedi: $_"
}

if (-not $NoLaunch) {
    Start-Process -FilePath (Join-Path $buildDir 'Vetra.exe') | Out-Null
}










