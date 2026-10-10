param([string]$LSFolder)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class NativeIni {
 [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern uint GetPrivateProfileInt(string section,string key,int fallback,string file);
 [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern bool WritePrivateProfileString(string section,string key,string value,string file);
}
'@
if (!$LSFolder -and (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'InstalledFolder.txt'))) { $LSFolder = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'InstalledFolder.txt') -Raw).Trim() }
if (!$LSFolder) {
    $picker = New-Object System.Windows.Forms.FolderBrowserDialog
    $picker.Description = 'Select the installed Lossless Scaling folder'
    if ($picker.ShowDialog() -ne 'OK') { exit 0 }
    $LSFolder = $picker.SelectedPath
}
$ini = [IO.Path]::GetFullPath((Join-Path $LSFolder 'NativeDLSS.ini'))
if (!(Test-Path -LiteralPath $ini)) { [System.Windows.Forms.MessageBox]::Show('NativeDLSS.ini not found. Run Install.cmd first.') | Out-Null; exit 1 }
[System.Windows.Forms.Application]::EnableVisualStyles()
$form = New-Object System.Windows.Forms.Form
$form.Text = 'Native DLSS for Lossless Scaling — experimental 0.1.1'
$form.ClientSize = New-Object System.Drawing.Size(600, 510)
$form.FormBorderStyle = 'FixedDialog'; $form.MaximizeBox = $false; $form.StartPosition = 'CenterScreen'
function Label([string]$text, [int]$y) {
    $l = New-Object System.Windows.Forms.Label; $l.Text = $text; $l.Location = New-Object System.Drawing.Point(18,$y)
    $l.Size = New-Object System.Drawing.Size(565,35); $form.Controls.Add($l); return $l
}
function Check([string]$text, [string]$key, [int]$fallback, [int]$y) {
    $c = New-Object System.Windows.Forms.CheckBox; $c.Text = $text; $c.Tag = $key
    $c.Checked = [NativeIni]::GetPrivateProfileInt('NativeDLSS',$key,$fallback,$ini) -eq 1
    $c.Location = New-Object System.Drawing.Point(18,$y); $c.Size = New-Object System.Drawing.Size(560,28); $form.Controls.Add($c); return $c
}
Label 'LSFG3 должен быть включён: Fixed ×2, HDR выключен. LS сохраняет захват и вывод.' 16 | Out-Null
$enabled = Check 'Включить подмену промежуточных кадров на NVIDIA DLSS FG' 'Enabled' 1 58
$flow = Check 'Optical Flow NVIDIA (может увеличивать нагрузку)' 'OpticalFlow' 1 94
Label 'Качество Optical Flow — выше дороже для GPU:' 132 | Out-Null
$quality = New-Object System.Windows.Forms.TrackBar
$quality.Minimum=1; $quality.Maximum=5; $quality.Value=[Math]::Min(5,[Math]::Max(1,[NativeIni]::GetPrivateProfileInt('NativeDLSS','Quality',2,$ini)))
$quality.Location=New-Object System.Drawing.Point(18,160); $quality.Size=New-Object System.Drawing.Size(550,45); $form.Controls.Add($quality)
$qtext = Label '' 210
$names = @('Быстро, сетка 4 px','Средне, сетка 4 px','Точно, сетка 4 px','Средне, сетка 2 px','Точно, сетка 2 px')
$quality.Add_ValueChanged({$qtext.Text="$($quality.Value)/5 — $($names[$quality.Value-1])"}); $qtext.Text="$($quality.Value)/5 — $($names[$quality.Value-1])"
Label 'Размер анализа Optical Flow (% ширины и высоты):' 246 | Out-Null
$scale = New-Object System.Windows.Forms.NumericUpDown; $scale.Minimum=10; $scale.Maximum=100; $scale.Increment=10
$scale.Value=[Math]::Min(100,[Math]::Max(10,[NativeIni]::GetPrivateProfileInt('NativeDLSS','AnalysisPercent',50,$ini)))
$scale.Location=New-Object System.Drawing.Point(440,248); $scale.Size=New-Object System.Drawing.Size(115,25); $form.Controls.Add($scale)
$ordered = Check 'Упорядочить зависимость на GPU (рекомендуется)' 'GPUOrdered' 1 290
Label 'Число заранее выделенных заданий (не Max frame latency LS):' 328 | Out-Null
$slots=New-Object System.Windows.Forms.NumericUpDown; $slots.Minimum=2; $slots.Maximum=4
$slots.Value=[Math]::Min(4,[Math]::Max(2,[NativeIni]::GetPrivateProfileInt('NativeDLSS','Slots',3,$ini)))
$slots.Location=New-Object System.Drawing.Point(500,329); $slots.Size=New-Object System.Drawing.Size(55,25); $form.Controls.Add($slots)
Label 'После сохранения полностью перезапусти LS. При несовпавшем кадре или запрете DLSS остаётся штатный кадр LS. Лог: logs\native-dlss-*.log.' 372 | Out-Null
$save=New-Object System.Windows.Forms.Button; $save.Text='Сохранить'; $save.Location=New-Object System.Drawing.Point(420,455)
$save.Size=New-Object System.Drawing.Size(140,35); $form.Controls.Add($save)
$save.Add_Click({
    try {
        foreach ($c in @($enabled,$flow,$ordered)) {
            if (![NativeIni]::WritePrivateProfileString('NativeDLSS',[string]$c.Tag,([int]$c.Checked).ToString(),$ini)) { throw 'Cannot save settings.' }
        }
        foreach ($pair in @(@('Quality',$quality.Value),@('AnalysisPercent',$scale.Value),@('Slots',$slots.Value))) {
            if (![NativeIni]::WritePrivateProfileString('NativeDLSS',$pair[0],$pair[1].ToString(),$ini)) { throw 'Cannot save settings.' }
        }
        [System.Windows.Forms.MessageBox]::Show('Настройки сохранены. Полностью перезапусти Lossless Scaling.') | Out-Null
        $form.Close()
    } catch { [System.Windows.Forms.MessageBox]::Show($_.Exception.Message) | Out-Null }
})
[void]$form.ShowDialog()
