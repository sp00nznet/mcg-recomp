# Run the game and grab the client area at each of -At (seconds from launch).
#
# The only reliable way to see what the recompiled engine is doing: it draws
# through the DirectDraw mock into a normal window, so PrintWindow on the client
# area is a true frame. Times are seconds from launch, which is also the clock
# MCG_CLICK uses, so the two line up:
#
#   $env:MCG_CLICK = '477,165@8;477,165@18'
#   .	oolsringup\shots.ps1 -At 12,30 -Seconds 60
#
# ponytail: PrintWindow, not a D3D capture. The window IS the output here.
param([string]$At = "60,120", [int]$Seconds = 180, [string]$Prefix = "t")
$src = @'
using System; using System.Drawing; using System.Runtime.InteropServices;
public class Shots {
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l,t,r,b; }
  public static void Grab(IntPtr h, string path) {
    RECT rc; GetClientRect(h, out rc);
    using (var bmp = new Bitmap(Math.Max(rc.r,1), Math.Max(rc.b,1)))
    using (var g = Graphics.FromImage(bmp)) {
      IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 1); g.ReleaseHdc(dc);
      bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
    }
  }
}
'@
if (-not ('Shots' -as [type])) { Add-Type -TypeDefinition $src -ReferencedAssemblies System.Drawing,System.Windows.Forms }
Get-Process mcg_recomp -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
$env:MCG_WATCHDOG = '1'
$p = Start-Process '.\build-trace\mcg_recomp.exe' -ArgumentList 'MCX.EXE' `
     -RedirectStandardError 'run.log' -RedirectStandardOutput 'run.out' -PassThru -NoNewWindow
$t0 = Get-Date
$marks = $At.Split(',') | ForEach-Object { [double]$_ }
$i = 0
foreach ($m in $marks) {
  $i++
  while (((Get-Date) - $t0).TotalSeconds -lt $m -and -not $p.HasExited) { Start-Sleep -Milliseconds 400 }
  if ($p.HasExited) { Write-Output "EXITED before $m s"; break }
  $p.Refresh(); $h = $p.MainWindowHandle
  if ($h -ne [IntPtr]::Zero) {
    $f = "{0}{1}.png" -f $Prefix, $i
    [Shots]::Grab($h, (Join-Path $PWD $f))
    Write-Output ("captured {0} at {1}s" -f $f, [int]((Get-Date) - $t0).TotalSeconds)
  }
}
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds -and -not $p.HasExited) { Start-Sleep -Milliseconds 500 }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
