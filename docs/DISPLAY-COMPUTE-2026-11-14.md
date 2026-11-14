# HDMI/USB-C, zero-copy и Geekbench Metal

## Требуемый результат

Реальные внешние HDMI и USB-C/DP мониторы с modeset/hotplug; прямой scanout
отрисованных GPU поверхностей без CPU копирования кадров; полный Geekbench 6
Metal со всеми workloads. Частичный прогон и отключение workload не являются
завершением. Рабочий обход B-58 (ожидание GPU в WindowServer) сохраняется.

## Что обнаружено

HDMI/DMUB headers существуют, но не включены рабочими phxaccel/phxbringup.
Комментарии ссылаются на отсутствующие tools/dml и testl. Их наличие само по
себе не означает работающий modeset. Добавлена preflight проверка modeset
до DPMS off: невалидные тайминги и неполный DML-план не пишут регистры.
Тест с настоящим modeset и fake callbacks сначала падал, после исправления
прошёл с ASan/UBSan; общий make test тоже прошёл. На Маке header не развёрнут.

## Read-only снимок HUBP на Маке

Через установленный phxctl regs, без register writes:

| DWORD index | Значение | Назначение |
|---|---|---|
| 0x3aa7 | 0x80 | HUBP0 tiling config |
| 0x3ac7 | 0x9ff | pitch: 2559 => 2560 pixels |
| 0x3ac9 | 0 | VMID settings |
| 0x3aca/0x3acb | 0 / 0x80 | primary address: 0x8000000000 |
| 0x3ada | 0 | surface control |
| 0x3adb | 0x04100000 | flip control |
| 0x3ae1/0x3ae2 | 0 / 0 | surface inuse |
| 0x3ae5/0x3ae6 | 0 / 0x80 | earliest inuse: 0x8000000000 |

Primary адреса HUBPREQ1/2/3 (0x3ba6/7, 0x3c82/3, 0x3d5e/f) равны 0.
Это снимок текущего состояния, не доказательство hotplug/работы всех портов.

Linux DCN3.1.4 требует pitch в pixels-1, HIGH затем LOW для latch и
подтверждение pending=0 плюс earliest-inuse=requested address. CPU physical
и MC адреса не взаимозаменяемы; пригодность произвольного wired RAM для DCN
не доказана. Существующий PhxAMDFB flip пишет pitch без -1, использует
getPhysicalSegment без проверки DCN address domain и не ждёт acknowledgement.
Запрет AMD zero-copy пока не снят.

Источники (Linux v6.12, фиксированный commit):
- [Регистры](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_1_4_offset.h)
- [Размер и flip pending](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/display/dc/hubp/dcn20/dcn20_hubp.c)
- [Порядок flip](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/display/dc/hubp/dcn30/dcn30_hubp.c)
- [MC/CPU адреса](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.c)

## Geekbench Metal

Исходники переводчика доступны локально: reference/NullMoth commit
118581571102ed40e835e45569ade9902d40f689. Старый вывод об обязательной
недоступности исправления закрытого переводчика устарел. Подтверждён отказ
смешанного Function/Private pointer phi. Начат ограниченный воспроизводимый
тест и исправление lowering в отдельной ветви работы. Долгий реальный MPS
перевод пока не повторялся. Geekbench целиком ещё не прошёл.
