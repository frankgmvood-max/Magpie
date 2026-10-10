param([string]$LSFolder)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class NativeIni {
 [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern uint GetPrivateProfileInt(string section,string key,int fallback,string file);
 [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern uint GetPrivateProfileString(string section,string key,string fallback,System.Text.StringBuilder value,uint size,string file);
 [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern bool WritePrivateProfileString(string section,string key,string value,string file);
}
'@
if (!$LSFolder -and (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'InstalledFolder.txt'))) { $LSFolder = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'InstalledFolder.txt') -Raw).Trim() }
if (!$LSFolder) {
    $picker = New-Object System.Windows.Forms.FolderBrowserDialog; $picker.Description = 'Select the installed Lossless Scaling folder'
    if ($picker.ShowDialog() -ne 'OK') { exit 0 }; $LSFolder = $picker.SelectedPath
}
$ini = [IO.Path]::GetFullPath((Join-Path $LSFolder 'NativeDLSS.ini'))
if (!(Test-Path -LiteralPath $ini)) { [System.Windows.Forms.MessageBox]::Show('NativeDLSS.ini not found. Run Install.cmd first.') | Out-Null; exit 1 }
function ReadIni([string]$section,[string]$key,[string]$fallback) {
    $b = New-Object Text.StringBuilder 512; [void][NativeIni]::GetPrivateProfileString($section,$key,$fallback,$b,512,$ini); return $b.ToString()
}
function WriteIni([string]$section,[string]$key,[string]$value) {
    if (![NativeIni]::WritePrivateProfileString($section,$key,$value,$ini)) { throw "Не удалось сохранить $key" }
}
[System.Windows.Forms.Application]::EnableVisualStyles()
$form = New-Object System.Windows.Forms.Form; $form.Text = 'Native DLSS — 0.2.0'
$form.ClientSize = New-Object Drawing.Size(760,680); $form.StartPosition = 'CenterScreen'; $form.FormBorderStyle = 'FixedDialog'; $form.MaximizeBox = $false
function Label($parent,[string]$text,[int]$y,[int]$height=42) {
    $l = New-Object Windows.Forms.Label; $l.Text=$text; $l.Location=New-Object Drawing.Point(16,$y); $l.Size=New-Object Drawing.Size(690,$height); $parent.Controls.Add($l); return $l
}
function Combo($parent,[string[]]$values,[int]$y) {
    $c=New-Object Windows.Forms.ComboBox; $c.DropDownStyle='DropDownList'; $c.Location=New-Object Drawing.Point(16,$y); $c.Size=New-Object Drawing.Size(680,30); $c.Items.AddRange($values); $c.SelectedIndex=0; $parent.Controls.Add($c); return $c
}
function Check($parent,[string]$text,[int]$y) {
    $c=New-Object Windows.Forms.CheckBox; $c.Text=$text; $c.Location=New-Object Drawing.Point(16,$y); $c.Size=New-Object Drawing.Size(680,30); $parent.Controls.Add($c); return $c
}
function Number($parent,[string]$title,[int]$min,[int]$max,[int]$y) {
    Label $parent $title $y 30 | Out-Null
    $n=New-Object Windows.Forms.NumericUpDown; $n.Minimum=$min; $n.Maximum=$max; $n.Location=New-Object Drawing.Point(590,$y); $n.Size=New-Object Drawing.Size(105,26); $parent.Controls.Add($n); return $n
}
Label $form 'Профиль: выбери существующий или введи имя приложения, например WoW.exe. Выбор явный; переключение окон его не меняет.' 12 42 | Out-Null
$profile=New-Object Windows.Forms.ComboBox; $profile.Location=New-Object Drawing.Point(18,58); $profile.Size=New-Object Drawing.Size(520,30); [void]$profile.Items.Add('Общий')
foreach ($line in [IO.File]::ReadAllLines($ini)) { if ($line -match '^\[Profile:([^\]]+)\]$') { [void]$profile.Items.Add($Matches[1]) } }; $form.Controls.Add($profile)
$load=New-Object Windows.Forms.Button; $load.Text='Загрузить'; $load.Location=New-Object Drawing.Point(552,56); $load.Size=New-Object Drawing.Size(180,32); $form.Controls.Add($load)
$tabs=New-Object Windows.Forms.TabControl; $tabs.Location=New-Object Drawing.Point(18,102); $tabs.Size=New-Object Drawing.Size(714,495); $form.Controls.Add($tabs)
$general=New-Object Windows.Forms.TabPage; $general.Text='Режим'; [void]$tabs.TabPages.Add($general)
$quality=New-Object Windows.Forms.TabPage; $quality.Text='Качество'; [void]$tabs.TabPages.Add($quality)
$hud=New-Object Windows.Forms.TabPage; $hud.Text='Защита HUD'; [void]$tabs.TabPages.Add($hud)
$diagnostics=New-Object Windows.Forms.TabPage; $diagnostics.Text='Диагностика'; [void]$tabs.TabPages.Add($diagnostics)
Label $general 'В LS оставь LSFG3: Fixed ×2, HDR выключен. Захват, вывод и курсор остаются у LS.' 16 | Out-Null
$enabled=Check $general 'Включить NativeDLSS (общая настройка)' 62
Label $general 'Обработка промежуточных кадров:' 108 30 | Out-Null
$mode=Combo $general @('Штатная генерация LS','DLSS: гибрид, резервный кадр LS','DLSS: экономичный, резервный исходный кадр') 142
$duplicate=Check $general 'Защита от повторов исходного кадра' 192
$delta=Number $general 'Допустимое отличие RGB (0 = точное совпадение)' 0 8 235
$cut=Check $general 'Резкая смена сцены: исходник и сброс истории DLSS' 283
$cutdelta=Number $general 'Порог изменения пикселя при смене сцены (0–255)' 1 255 326
$cutpercent=Number $general 'Доля изменившихся пикселей для смены сцены (%)' 1 100 365
Label $general 'Ctrl+Alt+F6: смена режима. F7 с теми же модификаторами: сброс истории. F8: перечитать режим, HUD и фильтры. Смена режима действует со следующего исходного кадра.' 412 45 | Out-Null
Label $quality 'Optical Flow строит движение по готовой картинке. Эти параметры относятся к анализу движения.' 16 | Out-Null
$flow=Check $quality 'Использовать NVIDIA Optical Flow' 64
Label $quality 'Скорость анализа Optical Flow:' 110 30 | Out-Null
$preset=Combo $quality @('Быстро','Средне','Точно') 144
Label $quality 'Сетка Optical Flow (2 px требует поддержки GPU):' 188 30 | Out-Null
$grid=Combo $quality @('4 px','2 px') 222
$scale=Number $quality 'Размер анализа (% ширины и высоты)' 10 100 272
$slots=Number $quality 'Заранее выделенные задания GPU' 2 4 314
$ordered=Check $quality 'Упорядочить зависимость на GPU' 355
$presets=Combo $quality @('Выбрать набор…','Экономичный: Fast / 4 px / 50%','Сбалансированный: Medium / 4 px / 50%','Качество: Slow / 2 px / 100%') 400
$presets.Add_SelectedIndexChanged({ if($presets.SelectedIndex -gt 0) { $preset.SelectedIndex=$presets.SelectedIndex-1; $grid.SelectedIndex=[int]($presets.SelectedIndex -eq 3); $scale.Value=50; if($presets.SelectedIndex -eq 3){$scale.Value=100} } })
Label $hud 'Укажи до 8 прямоугольников: слева, сверху, справа, снизу — в процентах захваченного изображения. В промежуточном кадре эти области берутся из текущего исходника.' 16 60 | Out-Null
$rects=New-Object Windows.Forms.DataGridView; $rects.Location=New-Object Drawing.Point(16,88); $rects.Size=New-Object Drawing.Size(680,285)
$rects.AllowUserToAddRows=$false; $rects.AllowUserToDeleteRows=$false; $rects.AutoSizeColumnsMode='Fill'; $rects.RowHeadersVisible=$false
foreach($title in @('Слева %','Сверху %','Справа %','Снизу %')) { [void]$rects.Columns.Add($title,$title) }; [void]$rects.Rows.Add(8); $hud.Controls.Add($rects)
Label $hud 'Пустая строка отключает область. Координаты нужно подобрать под своё разрешение и интерфейс игры. Защита HUD не создаёт данных глубины или игрового движения.' 388 70 | Out-Null
Label $diagnostics 'Последние записи текущего лога. Счётчики показывают задания и подмены; физический FPS и VRR здесь не измеряются.' 16 42 | Out-Null
$log=New-Object Windows.Forms.RichTextBox; $log.ReadOnly=$true; $log.WordWrap=$false; $log.Font=New-Object Drawing.Font('Consolas',9); $log.Location=New-Object Drawing.Point(16,68); $log.Size=New-Object Drawing.Size(680,385); $diagnostics.Controls.Add($log)
function SelectedSection {
    $name=$profile.Text.Trim(); if(!$name -or $name -eq 'Общий'){return 'NativeDLSS'}
    if($name -notmatch '^[^/\\:\[\]\x00-\x1f]{1,100}\.exe$'){throw 'Название профиля должно быть именем файла приложения, например WoW.exe'}
    return "Profile:$name"
}
function LoadProfile {
    $section=SelectedSection
    function V([string]$key,[int]$fallback) { return [NativeIni]::GetPrivateProfileInt($section,$key,[NativeIni]::GetPrivateProfileInt('NativeDLSS',$key,$fallback,$ini),$ini) }
    $enabled.Checked=[NativeIni]::GetPrivateProfileInt('NativeDLSS','Enabled',1,$ini) -eq 1
    $mode.SelectedIndex=[Math]::Min(2,(V 'Mode' 2)); $duplicate.Checked=(V 'DuplicateFilter' 1) -eq 1; $cut.Checked=(V 'SceneCut' 1) -eq 1
    $delta.Value=[Math]::Min(8,(V 'DuplicateDelta' 0)); $cutdelta.Value=[Math]::Min(255,[Math]::Max(1,(V 'CutDelta' 64))); $cutpercent.Value=[Math]::Min(100,[Math]::Max(1,(V 'CutPercent' 65)))
    $legacy=[Math]::Min(5,[Math]::Max(1,(V 'Quality' 2))); $oldpreset=2; if($legacy -eq 1){$oldpreset=1}; if($legacy -in @(3,5)){$oldpreset=3}
    $fp=V 'FlowPreset' 0; if(!$fp){$fp=$oldpreset}; $preset.SelectedIndex=[Math]::Min(3,[Math]::Max(1,$fp))-1
    $fg=V 'FlowGrid' 0; if(!$fg){$fg=4; if($legacy -ge 4){$fg=2}}; $grid.SelectedIndex=[int]($fg -eq 2)
    $flow.Checked=(V 'OpticalFlow' 1) -eq 1; $ordered.Checked=(V 'GPUOrdered' 1) -eq 1
    $scale.Value=[Math]::Min(100,[Math]::Max(10,(V 'AnalysisPercent' 50))); $slots.Value=[Math]::Min(4,[Math]::Max(2,(V 'Slots' 3)))
    for($i=0;$i -lt 8;$i++) {
        $parts=(ReadIni $section "HUD$($i+1)" (ReadIni 'NativeDLSS' "HUD$($i+1)" '')) -split ','
        for($j=0;$j -lt 4;$j++) { $rects.Rows[$i].Cells[$j].Value=''; if($parts.Count -eq 4){ $n=0; if([int]::TryParse($parts[$j],[ref]$n)){$rects.Rows[$i].Cells[$j].Value=([decimal]$n/100).ToString()} } }
    }
    $presets.SelectedIndex=0
}
$load.Add_Click({try{LoadProfile}catch{[Windows.Forms.MessageBox]::Show($_.Exception.Message)|Out-Null}})
$profile.Text=ReadIni 'NativeDLSS' 'ActiveProfile' 'Общий'; if(!$profile.Text){$profile.Text='Общий'}; LoadProfile
Label $form 'Качество и активный профиль: полный перезапуск LS. Режим, HUD и фильтры: сохранение + Ctrl+Alt+F8. G-SYNC настраивается вашим профилем драйвера.' 604 56 | Out-Null
$save=New-Object Windows.Forms.Button; $save.Text='Сохранить'; $save.Location=New-Object Drawing.Point(570,637); $save.Size=New-Object Drawing.Size(160,30); $form.Controls.Add($save)
function SaveProfile {
    $section=SelectedSection; $regions=@()
    for($i=0;$i -lt 8;$i++) {
        $values=@($rects.Rows[$i].Cells | ForEach-Object { ([string]$_.Value).Trim() }); $filled=@($values | Where-Object {$_}).Count
        if(!$filled){$regions+= '';continue}; if($filled -ne 4){throw "Заполни все 4 координаты области $($i+1)"}
        $numbers=@();foreach($value in $values){$n=0m; if(![decimal]::TryParse($value,[ref]$n) -or $n -lt 0 -or $n -gt 100){throw 'Координаты HUD должны быть от 0 до 100'};$numbers += [int][Math]::Round($n*100)}
        if($numbers[0] -ge $numbers[2] -or $numbers[1] -ge $numbers[3]){throw "Неверные границы области $($i+1)"};$regions+=($numbers -join ',')
    }
    $settings=@{Mode=$mode.SelectedIndex;DuplicateFilter=[int]$duplicate.Checked;SceneCut=[int]$cut.Checked;DuplicateDelta=$delta.Value;CutDelta=$cutdelta.Value;CutPercent=$cutpercent.Value;FlowPreset=$preset.SelectedIndex+1;FlowGrid=4;AnalysisPercent=$scale.Value;Slots=$slots.Value;OpticalFlow=[int]$flow.Checked;GPUOrdered=[int]$ordered.Checked}
    if($grid.SelectedIndex -eq 1){$settings.FlowGrid=2}
    foreach($key in $settings.Keys){WriteIni $section $key $settings[$key].ToString()}; for($i=0;$i -lt 8;$i++){WriteIni $section "HUD$($i+1)" $regions[$i]}
    WriteIni 'NativeDLSS' 'Enabled' ([int]$enabled.Checked).ToString(); $active='';if($section -ne 'NativeDLSS'){$active=$profile.Text.Trim()};WriteIni 'NativeDLSS' 'ActiveProfile' $active
    if($active -and !$profile.Items.Contains($active)){[void]$profile.Items.Add($active)}
}
$save.Add_Click({try {
    SaveProfile
    [Windows.Forms.MessageBox]::Show('Сохранено. Качество и профиль требуют полного перезапуска LS. Режим, HUD и фильтры можно перечитать через Ctrl+Alt+F8.')|Out-Null
} catch {[Windows.Forms.MessageBox]::Show($_.Exception.Message)|Out-Null}})
$timer=New-Object Windows.Forms.Timer; $timer.Interval=1000
$timer.Add_Tick({ if($tabs.SelectedTab -ne $diagnostics){return};try {
    $file=Get-ChildItem -LiteralPath (Join-Path $LSFolder 'logs') -Filter 'native-dlss-*.log' -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if(!$file){$log.Text='Лог пока не создан.';return}
    $reader=[IO.File]::Open($file.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    try{$start=[Math]::Max(0,$reader.Length-32768);[void]$reader.Seek($start,[IO.SeekOrigin]::Begin);$buffer=New-Object byte[] ([int]($reader.Length-$start));$count=$reader.Read($buffer,0,$buffer.Length);$text=[Text.Encoding]::UTF8.GetString($buffer,0,$count)}finally{$reader.Dispose()}
    $log.Text=$file.Name+"`r`n"+((($text -split "`n") | Select-Object -Last 65) -join "`n");$log.SelectionStart=$log.TextLength;$log.ScrollToCaret()
} catch{$log.Text=$_.Exception.Message} })
$timer.Start();try{[void]$form.ShowDialog()}finally{$timer.Stop();$timer.Dispose();$form.Dispose()}
