# LG C1: исследование системного Dolby Vision с ПК

Цель: исследовать постоянный вывод Windows в Dolby Vision на LG C1 с RTX 3080.
Пользователь уже менял EDID и скрытые флаги Windows; телевизор оставался в HDR.
Исследование ведётся на скачанной официальной прошивке, независимо от версии,
установленной на телевизоре. Код и сборки Magpie этим каталогом не изменяются.

## Результат первого этапа

**Найдена конкретная разница программного приёма сигнала между C1 и C3.**
В HDMI-библиотеке C1 проверка `is_dolby_valid` отклоняет LLDV с RGB Full.
Соответствующая проверка C3 такое сочетание принимает. У C3 также добавлен приём
LLDV с YCbCr 4:2:0. Результат подтверждён Ghidra и выполнением оригинального ARM
машинного кода в Unicorn; это не проверка реального HDMI-сигнала или телевизора.

| Сочетание в изолированной проверке | C1 03.53.45 | C3 23.25.55 |
|---|---|---|
| DolbyVision, RGB Full | Принимается | Принимается |
| DolbyVision, RGB Limited | Отклоняется | Отклоняется |
| LLDV, RGB Full | Отклоняется | Принимается |
| LLDV, RGB Limited | Принимается | Отклоняется |
| LLDV, YCbCr 4:2:2 Limited | Принимается | Принимается |
| LLDV, YCbCr 4:4:4 Limited | Принимается | Принимается |
| LLDV, YCbCr 4:2:0 Limited | Отклоняется | Принимается |

Для C3 таблица относится к переданным значениям timing-структуры 3840/2160.
Его функция имеет дополнительный аргумент timing и отдельную проверку двух
16-битных полей по смещениям `0x14`/`0x16`. При значениях 4096/2160 все 192
сочетания отклоняются. Эмулятор явно задаёт этот дополнительный аргумент.

В RAM эмулятора проверен **14-байтный эксперимент**, расширяющий ветку LLDV RGB
у C1: принимаются Limited и Full. Из 192 сочетаний изменились ровно три результата
(три значения неиспользуемого YCC-поля при LLDV/RGB Full); прежние разрешённые
сочетания не потеряны. Простая замена одного сравнения Limited → Full также
проверена как контрпример: она ломает прежний приём RGB Limited.

**Эксперимент не является готовой прошивкой.** Скрипт меняет байты только внутри
Unicorn, сохраняет исходную библиотеку и не создаёт файл для установки в ТВ.
Успех этой проверки не доказывает правильность тонального отображения, цвета,
взаимодействия с драйвером, VRR или возможности загрузки изменённого компонента.

## Подтверждённые материалы

- [firmware-manifest.json](firmware-manifest.json): официальные URL, SHA-256,
  размеры, версии, идентификаторы платформ и использованные инструменты.
- [evidence/](evidence/): хеши библиотек, адреса функций, таблицы результатов,
  сравнение и область выполненных проверок.
- [scripts/](scripts/): загрузка, инвентаризация ELF, поиск настроек,
  экспорт Ghidra, ARM-эмуляция и локальный сбор состояния HDMI без изменений.

Оба EPK расшифрованы; проверки 248 блоков C1 и 287 блоков C3 прошли инструментом
`epk2extract -s -n` с публичными ключами из зафиксированного репозитория openlgtv.
ZIP CRC проверены при извлечении. Хеши архивов вычислены локально и закреплены
для воспроизводимости; это не опубликованные LG контрольные суммы.

C1 и C3 используют ARM32, однако зависимости различаются: например,
`libboost_thread.so.1.68.0` против `libboost_thread.so.1.78.0`. Одинаковая архитектура
сама по себе не подтверждает возможность переноса целой библиотеки.

В полном извлечённом rootfs C1 и BSP не найдены четыре буквальных варианта имени
PC-переключателя. В выбранных компонентах C3 найдены `enableDolbyVisionPC`, его
описания и значения по умолчанию `off`. В схеме C3 есть условия скрытия для
некоторых плат, в том числе `_O22N_`. Наличие схемы не доказывает доступность
переключателя на конкретной модели; отсутствие строки не исключает эквивалентную
логику под другим именем.

## Точные адреса

Адреса ниже — ELF VMA относительно библиотеки, **не адреса в памяти работающего
телевизора**. Ghidra при импорте выбрала image base `0x10000`, поэтому её адреса
на `0x10000` больше. Thumb-бит символа при отображении адресов убран.

| Библиотека / функция | C1 ELF VMA | C3 ELF VMA |
|---|---|---|
| HDMI element / `is_dolby_valid` | `0x3e558` | `0x3ce50` |
| HDMI element / `is_dolby_hdr_supported` | `0x3ee44` | `0x3e3c4` |
| HDMI element / `rule_dolby_hdr` | `0x3ef04` | `0x3e6cc` |
| HDMI connection / `_EDID_CTRL_GetDolbyCriterionType` | `0x1689c` | `0x18a68` |

Имена взяты из символов ELF. Псевдокод Ghidra, типы параметров и автоматически
восстановленные прототипы нельзя считать оригинальными исходниками. Имена
числовых перечислений проверены исполнением форматирующих функций исходной
библиотеки; результат не основан на догадке по значениям 0/1/2/3.

## Воспроизведение

Нужны Python 3.11+, `c++filt`, Ghidra 12.1.4 с JDK 21, `epk2extract` по указанному
коммиту и `unsquashfs` с LZO. Python-зависимости:

```sh
python -m venv work/venv
work/venv/bin/python -m pip install -r requirements.txt
work/venv/bin/python scripts/fetch_firmware.py c1
work/venv/bin/python scripts/fetch_firmware.py c3
```

После установки `epk2extract` с ключами по его README:

```sh
epk2extract -s -n work/c1/lib32-starfish-global-secured-o20n-koli-47-03.53.45_prodkey_usb_V3_SECURED.epk
epk2extract -s -n work/c3/lib32-starfish-global-secured-o22n-okapi.pine-61-23.25.55_prodkey_usb_V3_SECURED.epk
unsquashfs -d work/c1-root work/c1/03.53.45.01-HE_DTV_W21O_AFABATAA/rootfs.pak
unsquashfs -d work/c3-root work/c3/23.25.55.01-HE_DTV_W23O_AFABATAA/rootfs.pak
```

Копии файлов в `work/` не отслеживаются Git. Исходный архив воспроизводимо
скачивается с LG по закреплённому хешу; бинарные образы не помещаются в историю Git.

```sh
work/venv/bin/python scripts/probe_dolby_gate.py work/c1-root/usr/lib/element/libelement-video-hdmi.so.1.0.0 --output work/c1-gate.json
work/venv/bin/python scripts/probe_dolby_gate.py work/c3-root/usr/lib/element/libelement-video-hdmi.so.1.0.0 --output work/c3-gate.json
work/venv/bin/python scripts/probe_dolby_gate.py work/c1-root/usr/lib/element/libelement-video-hdmi.so.1.0.0 --experiment-c1-rgb-both --output work/c1-experiment.json
```

Для пересоздания сводок в `evidence/` используются имена выходных файлов,
ожидаемые `summarize_results.py`:

```sh
work/venv/bin/python scripts/elf_inventory.py work/c1-root/usr/lib/element/libelement-video-hdmi.so.1.0.0 work/c1-root/usr/lib/libhdmi-conn-protocol.so.1.0.0 --disassemble --output work/c1-hdmi-inventory.json
work/venv/bin/python scripts/elf_inventory.py work/c3-root/usr/lib/element/libelement-video-hdmi.so.1.0.0 work/c3-root/usr/lib/libhdmi-conn-protocol.so.1.0.0 --disassemble --output work/c3-hdmi-inventory.json
work/venv/bin/python scripts/probe_dolby_gate.py work/c1-root/usr/lib/element/libelement-video-hdmi.so.1.0.0 --output work/c1-dolby-gate-emulation.json
work/venv/bin/python scripts/probe_dolby_gate.py work/c3-root/usr/lib/element/libelement-video-hdmi.so.1.0.0 --output work/c3-dolby-gate-emulation.json
work/venv/bin/python scripts/probe_dolby_gate.py work/c1-root/usr/lib/element/libelement-video-hdmi.so.1.0.0 --experiment-c1-rgb-full --output work/c1-rgb-full-experiment.json
work/venv/bin/python scripts/probe_dolby_gate.py work/c1-root/usr/lib/element/libelement-video-hdmi.so.1.0.0 --experiment-c1-rgb-both --output work/c1-rgb-both-experiment.json
work/venv/bin/python scripts/summarize_results.py
```

Линейная дизассембляция ELF может отображать встроенные таблицы/литералы как
инструкции. Для выводов о ветвлениях здесь использованы анализ Ghidra и эмуляция.

Экспорт декомпиляции выполняется штатным headless-режимом Ghidra. Каталог проекта
должен существовать; `JAVA_HOME` должен указывать на JDK 21:

```sh
analyzeHeadless work/ghidra-projects C1_HDMI -import work/c1-root/usr/lib/element/libelement-video-hdmi.so.1.0.0 -scriptPath scripts -postScript ExportDolbyFunctions.java work/decompiled -analysisTimeoutPerFile 240 -max-cpu 2
```

## Что остаётся установить

1. Передаёт ли Windows/NVIDIA в проблемном режиме Dolby VSIF и какой именно
   Dolby-тип, pixel encoding и диапазон видит приёмник C1. Из факта «остался HDR»
   это определить нельзя. `collect_hdmi_state.sh` подготовлен для чтения данных
   через уже имеющуюся авторизованную оболочку; на телевизоре он не запускался.
2. Согласуется ли расширение ветки RGB Full с последующей обработкой цвета и
   Dolby-метаданных, особенно при 4K/120 и VRR. Эмуляция одной функции этого не проверяет.
3. Как на выбранной версии C1 загрузить изменённый компонент с сохранением
   целостности системы и возможности отката. Штатное подписанное USB-обновление
   само по себе не является способом установки изменённого образа.

Следующий предмет исследования — путь `AVI/VSIF → is_dolby_valid →
on_dolby_hdr_signal_changed → video output/PQ`, а также построение VSVDB в новой
ветке EDID. Подтверждённое различие даёт место для проверки патча, но пока не
доказывает, что именно оно вызвало отказ пользовательской связки.
