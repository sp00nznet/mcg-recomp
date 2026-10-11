# One-click setup for the MechCommander Gold static recompilation. Run it by
# double-clicking Setup.cmd in the repo folder; the README's "Step by step" is
# the same thing by hand, command for command.
#
# It checks the tools, copies your copy of MechCommander Gold into game\,
# reads the function table out of MCX.EXE, lifts it to C, builds
# build\mcg_recomp.exe and leaves a shortcut to it in this folder. Each step is
# skipped when its output already exists (-Force redoes them), so a rerun after
# a failure resumes. Everything it does goes to setup.log.
#
#   -Game <drive, folder, .iso or .zip>   skip the question
#   -Yes                                  answer yes to every install question
param([switch]$Force, [switch]$Yes, [string]$Game = "")

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Toolkit = if ($env:PCRECOMP) { $env:PCRECOMP } else { Join-Path (Split-Path -Parent $Root) 'tools' }
$Log = Join-Path $Root 'setup.log'
Set-Location $Root
function Log($t) { Add-Content -Path $Log -Value $t -Encoding UTF8 }
Log "==== setup $(Get-Date -Format s)"

function Say($t, $c = 'Gray') { Write-Host $t -ForegroundColor $c; Log $t }
function Step($n, $t) { Write-Host ""; Say "[$n/7] $t" 'Cyan' }
function Pause-End { if (-not $Yes) { Read-Host "Press Enter to close" | Out-Null } }
function Fail($t) {
  Say ""; Say "Setup stopped: $t" 'Red'
  Say "The details are in $Log. Fix that and run Setup.cmd again; finished steps are skipped." 'Yellow'
  Pause-End; exit 1
}
function Ask($q) { if ($Yes) { Say "$q yes"; return $true }; $a = Read-Host "$q [Y/n]"; return -not ($a -match '^[nN]') }
function Refresh-Path {
  $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}
# Run a program with its output in the log. Windows PowerShell turns a native
# program's stderr into errors, so 'Stop' is off while it runs.
function Exec([string[]]$cmd) {
  $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $rest = @($cmd | Select-Object -Skip 1)   # @(): a one-element slice would splat as characters
  & $cmd[0] @rest 2>&1 | ForEach-Object { Log "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = $old
  return $code
}
function Run($what, [string[]]$cmd) {
  Say "  $what..."
  $code = Exec $cmd
  if ($code -ne 0) { Fail "$what failed (exit code $code)." }
}

Clear-Host
Say "MechCommander Gold: static recompilation setup" 'White'
Say "You need your own copy of MechCommander Gold (the CD, an image of it, or an installed game folder) and about 2 GB free."

# ---------------------------------------------------------------- tools
Step 1 "Checking the tools"
# A Python that answers "Python 3.x". The Store's placeholder "python" (the
# one that opens the Store) answers nothing, so asking is the reliable test.
function Find-Python {
  foreach ($c in @(@('py', '-3'), @('python'), @('python3'))) {
    if (-not (Get-Command $c[0] -ErrorAction SilentlyContinue)) { continue }
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $rest = @($c | Select-Object -Skip 1)
    $v = (& $c[0] @rest --version 2>&1 | Out-String).Trim()
    $ErrorActionPreference = $old
    if ($v -match '^Python 3\.(\d+)' -and [int]$Matches[1] -ge 10) { return ,$c }
  }
  return $null
}
$pyargs = Find-Python
if (-not $pyargs) {
  Say "  Python 3.10 or newer is not installed."
  if (-not (Ask "  Install Python 3.12 now (winget, for your user only, about 30 MB)?")) { Fail "Python 3 is required." }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Fail "winget is missing. Install 'App Installer' from the Microsoft Store, or install Python yourself."
  }
  Exec @('winget', 'install', '-e', '--id', 'Python.Python.3.12', '--scope', 'user', '--accept-package-agreements', '--accept-source-agreements') | Out-Null
  Refresh-Path
  $pyargs = Find-Python
  if (-not $pyargs) { Fail "Python installed, but Windows has not picked it up yet: close this window and run Setup.cmd again." }
}
Say "  Python: $($pyargs -join ' ')"

if ((Exec ($pyargs + @('-c', 'import pefile, capstone'))) -ne 0) {
  Say "  The Python packages pefile and capstone are missing (about 20 MB)."
  if (-not (Ask "  Install them now (pip, for your user only)?")) { Fail "pefile and capstone are required." }
  Run "Installing pefile and capstone" ($pyargs + @('-m', 'pip', 'install', '--user', 'pefile', 'capstone'))
}

if (-not (Test-Path (Join-Path $Toolkit 'tools\lift\lift32.py'))) {
  Say "  The pcrecomp toolkit is not beside this folder ($Toolkit)."
  if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "git is needed to fetch pcrecomp. Install Git for Windows, then run Setup.cmd again." }
  if (-not (Ask "  Download it now (git, about 20 MB)?")) { Fail "pcrecomp is required at $Toolkit." }
  Run "Cloning pcrecomp" @('git', 'clone', '--depth', '1', 'https://github.com/sp00nznet/pcrecomp', $Toolkit)
}
Say "  pcrecomp: $Toolkit"
$env:PCRECOMP = $Toolkit

# Visual Studio 2022 or its Build Tools, with the C++ tools; CMake, Ninja and
# the Windows SDK (whose x86 import libraries gen_imports.py reads) ship
# inside that workload, so build.cmd needs nothing else.
function Find-VS {
  foreach ($p in @("${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
                   "$env:ProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe")) {
    if (Test-Path $p) {
      $vs = & $p -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
      if ($vs) { return $vs }
    }
  }
  foreach ($d in (Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio\2022", "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022" -Directory -ErrorAction SilentlyContinue)) {
    if (Test-Path "$($d.FullName)\VC\Auxiliary\Build\vcvarsall.bat") { return $d.FullName }
  }
  return $null
}
if (-not (Find-VS)) {
  Say "  Visual Studio 2022 with the C++ tools is not installed. The recompiled game is C, and this is the compiler."
  if (-not (Ask "  Install the Visual Studio 2022 Build Tools with the C++ workload now (winget, about 3 GB)?")) { Fail "a C compiler (Visual Studio 2022 C++) is required." }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "winget is missing. Install Visual Studio 2022 Build Tools with 'Desktop development with C++' yourself." }
  Exec @('winget', 'install', '-e', '--id', 'Microsoft.VisualStudio.2022.BuildTools', '--accept-package-agreements', '--accept-source-agreements',
         '--override', '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended') | Out-Null
  if (-not (Find-VS)) { Fail "the Build Tools did not install. Install Visual Studio 2022 with 'Desktop development with C++' yourself." }
}
Say "  C compiler: $(Find-VS)"

# ---------------------------------------------------------------- the game
Step 2 "Copying your copy of MechCommander Gold into game\"
if ((Test-Path 'game\MCX.EXE') -and -not $Force) {
  Say "  Already there (skipping)."
} else {
  $Game = $Game.Trim('"', ' ')
  if (-not $Game) {
    # A drive holding the CD, or the usual install folder, answers for itself.
    $cands = @(Get-PSDrive -PSProvider FileSystem | ForEach-Object { $_.Root }) +
             @("${env:ProgramFiles(x86)}\MicroProse\MechCommander Gold", "$env:ProgramFiles\MicroProse\MechCommander Gold")
    foreach ($c in $cands) { if (Test-Path (Join-Path $c 'MCX.EXE')) { $Game = $c; Say "  Found MechCommander Gold in $Game"; break } }
  }
  while (-not $Game) {
    $Game = (Read-Host "  Where is MechCommander Gold? A drive (E:\), a folder with MCX.EXE, or a .iso/.zip file").Trim('"', ' ')
  }
  $mounted = $null
  if ($Game -match '\.iso$') {
    # Windows mounts an ISO itself; no tool needed.
    $mounted = Mount-DiskImage -ImagePath (Resolve-Path $Game) -PassThru
    $Game = ($mounted | Get-Volume).DriveLetter + ':\'
  } elseif ($Game -match '\.zip$') {
    $tmp = Join-Path $Root 'work\unzip'
    Say "  Unzipping $Game..."
    Expand-Archive -Path $Game -DestinationPath $tmp -Force
    $exe = Get-ChildItem $tmp -Recurse -Filter 'MCX.EXE' | Select-Object -First 1
    if (-not $exe) { Fail "there is no MCX.EXE in $Game." }
    $Game = $exe.DirectoryName
  }
  if (-not (Test-Path (Join-Path $Game 'MCX.EXE'))) { Fail "there is no MCX.EXE in $Game." }
  Say "  Copying the game (about 640 MB)..."
  New-Item -ItemType Directory -Force 'game' | Out-Null
  $code = Exec @('robocopy', $Game, (Join-Path $Root 'game'), '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/R:1', '/W:1')
  if ($mounted) { Dismount-DiskImage -ImagePath $mounted.ImagePath | Out-Null }
  if ($code -ge 8) { Fail "copying the game failed (robocopy exit code $code)." }
  # Off a CD every file arrives read-only, and the game writes its saves and
  # settings beside itself.
  Get-ChildItem 'game' -Recurse -File | ForEach-Object { $_.IsReadOnly = $false }
}

# ---------------------------------------------------------------- function table
Step 3 "Reading the function table out of MCX.EXE (seconds)"
if ((Test-Path 'work\functions.json') -and -not $Force) { Say "  Already done (skipping)." }
else { Run "Reading CodeView" ($pyargs + @('tools\cv_extract.py')) }

# ---------------------------------------------------------------- lift
Step 4 "Recompiling MCX.EXE to C (under a minute)"
if ((Test-Path 'src\recomp\gen\recomp_dispatch.c') -and -not $Force) { Say "  Already done (skipping)." }
else { Run "Lifting" ($pyargs + @('run_pipeline.py')) }

# ---------------------------------------------------------------- imports
Step 5 "Generating the Windows import bridges"
if ((Test-Path 'src\runtime\imports_gen.c') -and -not $Force) { Say "  Already done (skipping)." }
else { Run "Generating" ($pyargs + @('gen_imports.py')) }

# ---------------------------------------------------------------- build
Step 6 "Building build\mcg_recomp.exe (several minutes)"
if ((Test-Path 'build\mcg_recomp.exe') -and -not $Force) { Say "  Already built (skipping)." }
else {
  $env:CMAKE_ARGS = "-DPCRECOMP=$($Toolkit -replace '\\', '/')"
  Run "Compiling" @('cmd', '/d', '/c', "`"$Root\build.cmd`"")
}

# ---------------------------------------------------------------- shortcut
Step 7 "Making the shortcut"
$lnk = Join-Path $Root 'MechCommander Gold.lnk'
$sh = New-Object -ComObject WScript.Shell
$s = $sh.CreateShortcut($lnk)
$s.TargetPath = Join-Path $Root 'build\mcg_recomp.exe'
$s.Arguments = '--run'
$s.WorkingDirectory = $Root
$s.IconLocation = (Join-Path $Root 'game\MCX.EXE') + ',0'
$s.Save()
Say "  $lnk"

Write-Host ""
Say "Done. Double-click 'MechCommander Gold' in this folder to play." 'Green'
Pause-End
