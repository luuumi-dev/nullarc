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

## HDMI подключён: снимок после подтверждения владельца

HPD0/1 status (DWORD 0x53d4/0x53dc): 0x17000012 / 0xff000013.
У обоих выставлены SENSE (bit 1) и SENSE_DELAYED (bit 4); HPD2–4 равны 0.
Без сравнения с отключённым кабелем нельзя определить, какой HPD соответствует
HDMI. OTG0 control 0x11201, OTG1–3 0x300: включён только OTG0.

DCN VM FB BASE/TOP/OFFSET (0x3935–37): 0x8000 / 0x801f / 0x800;
AGP BOT/TOP/BASE (0x3938–3a): все 0. Это подтверждает необходимость
перевода адресного домена перед zero-copy; регистры не менялись.

Тест Function/Private добавлен в ignored reference/NullMoth; rustfmt и
разбор LLVM IR Clang прошли. Cargo offline остановился до компиляции:
нет serde и spirv в локальных кэшах. Исправления lowering пока нет.

## Уточнение адресов и текущего backing для zero-copy

IORegistry PhxBringup подтверждает: phx-fb-mc-base=0x8000000000,
phx-fb-phys=0x800000000, phx-fb-size=0x20000000;
phx-fb-aper-phys=0x820000000, phx-fb-aper-len=0x10000000.
DCN VM context0 PAGE_TABLE_BASE/START/END HI/LO (0x3a1a–0x3a1f)
все равны нулю. Активной таблицы context0 из этого снимка не видно.

В NVMTLDevice.m nvmtl_surface_image_create уже импортирует собственные
страницы IOSurface через VK_EXT_external_memory_host для GPU-рендеринга.
Отдельный vramAlloc в PhxAMDFB выдаёт физически непрерывный RAM pool;
это не та же память и не доказанно DCN-доступная поверхность. Поэтому
снятие одного metaCast guard не даст полноценного zero-copy. Требуется
scanout именно отрендеренного backing IOSurface с корректным DCN mapping,
а также pin/lifetime, pitch и подтверждение latch. Рассматривается upstream
DCHVM/VMID путь, прежде чем менять работающий дисплей.

Cargo online успешно загрузил существующие зависимости проекта (без
установки системных пакетов). Оба новых теста реально скомпилировались
и воспроизводят pointer merge storage mismatch Function vs Private.
Offline dependency blocker снят; исправление lowering продолжается.

## DMUB preflight и реальный статус прошивки

Modeset теперь проверяет callbacks, inbox, размер и выравнивание ring pointers,
а также firmware-ready перед DPMS off. Новый сценарий с корректным DML и
пустым DMUB воспроизводил ASan SEGV; после правки ASan/UBSan тест и общий
make test CXX=clang++ прошли. Код на Мак не развёрнут.

Read-only DMCUB: CNTL 0x36b6=0x001900c6 (enable=1), SCRATCH0
0x36a3=0x2 (mailbox_rdy=1, dal_fw=0). Inbox BASE/SIZE/WPTR/RPTR:
0x80000000 / 0x2000 / 0x900 / 0x900. CW4 OFFSET/HIGH/BASE:
0x1fe40000 / 0x80 / 0x04000000. Готовность нужной driver DAL firmware
не подтверждается; текущий modeset preflight должен отказать. Понадобится
отдельно проверить загрузку/владение DMUB до интеграции HDMI; успешный
modeset или hotplug этот снимок не доказывает.

## Выбранный upstream путь zero-copy

Для разрозненных страниц IOSurface требуется плоская GART/PTE таблица,
не расширение AGP: 8 байт PTE на 4 KiB страницу, depth=0/block_size=0.
Нужно получить именно DMA-адреса удерживаемого IOSurface memory descriptor,
а не отдельного vramAlloc. CPU physical==DMA при IOMMU не предполагается.

Linux v6.12 dcn20_vmid_setup задаёт START/END в страницах (>>12),
CNTL depth/block, BASE_HI и BASE_LO последним; root не сдвигается на 12.
Далее ждёт BASE_LO bit0. Это ACK контекста, а не display completion.
В hubbub31_init_dchub_sys_ctx таблицы 0 и 15 одинаковы; отдельный VMID15
сам по себе не доказывает безопасность изоляции от firmware scanout.

Нужны также HUBP system aperture/TLB и DCHVM hostVM/rIOMMU-последовательность;
эти настройки влияют на общий display. Значения AGP 0/0/0 нельзя считать
отключением AGP (upstream отключает BOT>TOP). Сначала сохранить регистры
и проверить DMA mapping. Для первого запуска использовать постоянные PTE
удерживаемого пула; изменение живых PTE требует проверенного invalidation.

Обычный vblank flip (immediate=0) переключает VMID, затем address HIGH/LOW;
producer fence ждётся до flip. Старые pages/table/map держать до pending=0
и EARLIEST_INUSE==новый адрес на всех использующих HUBP. До готовности этих
частей существующий guard не снимать.

Проверенные первоисточники (тот же pinned commit, ссылки выше):
- display/dc/dcn20/dcn20_vmid.c, dcn20_vmid_setup (44–98).
- display/dc/hubbub/dcn31/dcn31_hubbub.c, init_dchub_sys_ctx (905–938).
- display/dc/hubbub/dcn21/dcn21_hubbub.c, dchvm_init (67–105).
- display/dc/hubp/dcn30/dcn30_hubp.c, aperture и flip (45–131).
- display/amdgpu_dm/amdgpu_dm_plane.c, framebuffer GART binding (959–986).
- amdgpu/amdgpu_gart.c, flush/invalidation (394–415).

Для нативной сборки исправленного переводчика Rust установлен из официального
rustup в /Users/dripside/gpu/rust-build/{cargo,rustup}, без изменения shell PATH
и без sudo. Sources staging: /Users/dripside/gpu/nullarc-translator-b29.
Реальный Geekbench IR найден в ~/gpu/linkdump/nvmtl-link-4925:
depthwiseConv_3x3.linked.ll (570032 bytes) и .fc.txt (8 constants, 130 bytes).
Он скопирован для проверки; повторная трансляция новой библиотекой ещё впереди.
