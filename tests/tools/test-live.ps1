# TVTest を実際に走らせて TSMemory.tvtp の取り込みから連携までを確認する。
# tests/tools/test-live.sh から呼ばれる。
param(
	[Parameter(Mandatory=$true)][string]$TVTestDir,
	[Parameter(Mandatory=$true)][string]$TsFile,
	[Parameter(Mandatory=$true)][string]$BuildDir,
	[int]$BufferSeconds = 20,
	[int]$TimeoutSeconds = 90
)

$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Win {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  public delegate bool EnumProc(IntPtr h, IntPtr p);
}
'@

function Get-MainWindow([int]$ProcessId) {
	$found = [IntPtr]::Zero
	$cb = [Win+EnumProc]{
		param($h, $l)
		$q = 0
		[Win]::GetWindowThreadProcessId($h, [ref]$q) | Out-Null
		if ($q -eq $ProcessId -and [Win]::IsWindowVisible($h)) {
			$c = New-Object Text.StringBuilder 256
			[Win]::GetClassNameW($h, $c, 256) | Out-Null
			if ($c.ToString() -eq 'TVTest Window') { $script:found = $h; return $false }
		}
		return $true
	}
	[Win]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
	return $script:found
}

$receiverExe = Join-Path $BuildDir 'test_receiver.exe'
$receiverLog = Join-Path $BuildDir 'receiver.log'
$aux2 = Join-Path $BuildDir 'plugin\TSMemory-TVTestSrc.aux2'
$prefix = Join-Path $BuildDir 'live'
$tvtestLog = Join-Path $TVTestDir 'TVTest.exe.log'

Remove-Item -ErrorAction SilentlyContinue $receiverLog, $tvtestLog
Remove-Item -ErrorAction SilentlyContinue (Join-Path $TVTestDir 'Plugins\tsmemory*.tvtv')

# 前回の残骸が居ると TvtPlay がそちらにファイルを渡してしまうので落としておく
Get-Process -Name TVTest -ErrorAction SilentlyContinue | ForEach-Object {
	Write-Output ('  killing a leftover TVTest (pid ' + $_.Id + ')')
	$_.Kill()
	$_.WaitForExit(5000) | Out-Null
}
Get-Process -Name test_receiver -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill() }
Start-Sleep -Seconds 1

# TVTest は終了時に設定を書き戻すので、毎回同じ状態から始められるように
# 必要な設定だけの ini を作り直す。
# ※ 真偽値は yes/no でないと読まれない (CSettings::Read)
$ini = Join-Path $TVTestDir 'TVTest.ini'
@'
[PluginList]
PluginCount=2
Plugin0_Name=TSMemory.tvtp
Plugin0_Enable=yes
Plugin1_Name=TvtPlay.tvtp
Plugin1_Enable=yes

[Settings]
Driver=BonDriver_Pipe.dll
'@ | Set-Content -Path $ini -Encoding Unicode

Write-Output '=== starting the AviUtl2 stand-in (test_receiver) ==='
$receiver = Start-Process -FilePath $receiverExe `
	-ArgumentList $aux2, $prefix, $TimeoutSeconds, $TsFile `
	-RedirectStandardOutput $receiverLog -NoNewWindow -PassThru
Start-Sleep -Seconds 2

Write-Output "=== starting TVTest with $TsFile ==="
$tvtest = Start-Process -FilePath (Join-Path $TVTestDir 'TVTest.exe') `
	-ArgumentList '/d', 'BonDriver_Pipe.dll', '/log', $TsFile `
	-WorkingDirectory $TVTestDir -PassThru

$started = Get-Date
Write-Output "    buffering for $BufferSeconds seconds ..."
Start-Sleep -Seconds $BufferSeconds

$tvtest.Refresh()
if ($tvtest.HasExited) {
	Write-Output ('TVTest exited early, code=0x{0:X8}' -f $tvtest.ExitCode)
} else {
	$hwnd = Get-MainWindow $tvtest.Id
	if ($hwnd -eq [IntPtr]::Zero) {
		Write-Output 'could not find the TVTest main window'
	} else {
		# プラグインのコマンドは CM_PLUGINCOMMAND_FIRST (15000) から
		# プラグインの読み込み順に割り当てられる。TSMemory.tvtp は
		# TvtPlay.tvtp より先に読み込まれ、コマンドを 1 つだけ登録するので 15000。
		$elapsed = ((Get-Date) - $started).TotalSeconds
		Write-Output ('=== sending the TSMemory Execute command ({0:N1} sec after launching TVTest) ===' -f $elapsed)
		[Win]::PostMessage($hwnd, 0x0111, [IntPtr]15000, [IntPtr]0) | Out-Null
	}
}

Write-Output '=== waiting for the receiver ==='
if (-not $receiver.WaitForExit($TimeoutSeconds * 1000 + 15000)) {
	$receiver.Kill()
	Write-Output 'receiver timed out'
}

if (-not $tvtest.HasExited) {
	$tvtest.CloseMainWindow() | Out-Null
	Start-Sleep -Seconds 5
	if (-not $tvtest.HasExited) { $tvtest.Kill() }
}

Write-Output ''
Write-Output '=== receiver output ==='
if (Test-Path $receiverLog) { Get-Content $receiverLog }

Write-Output ''
Write-Output '=== TVTest log (TSMemory / TvtPlay related) ==='
if (Test-Path $tvtestLog) {
	Get-Content $tvtestLog | Where-Object { $_ -match 'TSMemory|TvtPlay|BonDriver|起動|終了|エラー' }
}

exit $receiver.ExitCode
