# Пускане на Gateway-а на ESP32-P4 (FireBeetle 2, DFR1172)

Бележки от фаза 0 на `ROADMAP_GATEWAY_P4.md`. Всичко тук е проверено в
кода и документацията на Espressif и DFRobot, или е измерено на платката.

## 0.1 Версии (2026-11-11)

### Платката

Прочетено с `esptool v5.4.0` (`chip-id`, `flash-id`) и `espefuse summary`,
без прошиване:

| | |
|---|---|
| Чип | ESP32-P4, **ревизия v1.3** |
| eFuse | `WAFER_VERSION_MAJOR` = 1, `WAFER_VERSION_MINOR` = 3, `BLK_VERSION` = 0.3, `PKG_VERSION` = 0 |
| ROM | `ESP-ROM:esp32p4-eco2-20240710` |
| Кристал | 40 MHz |
| Flash | 16 MB |
| PSRAM | 32 MB по wiki-то на DFRobot (eFuse `PSRAM_CAP` = 2, `PSRAM_VENDOR` = 1) |
| USB | USB-Serial/JTAG, на Mac `/dev/cu.usbmodem1101` |
| MAC (eFuse) | `30:ed:a0:ea:1b:a0` |

Wiki-то на DFRobot пише само „ESP32-P4R32“ и не казва ревизията, затова тя е
взета от чипа.

Заводският firmware на P4 стига до `entry 0x4ff2abd0` от втория bootloader и
не печата нищо повече на USB-Serial/JTAG.

### ESP-IDF

- Локално: `~/esp-idf` на `v6.0.2` (commit `7101770dc6d`). Gateway-ът иска `>=6.0.2`.
- Upstream има и `v6.0.3`. Засега оставаме на `v6.0.2`, като S3 Gateway-а.

**Ревизия на P4 в IDF 6.0.2** (`components/esp_hw_support/port/esp32p4/Kconfig.hw_support`):
- поддръжката на ревизии под 3.0 и от 3.0 нагоре е взаимно изключваща се
  („NOTE! Support of ESP32-P4 rev. <3.0 and >=3.0 is mutually exclusive“);
- по подразбиране IDF 6.0.2 строи за **v3.1+** (`ESP32P4_REV_MIN_301`). Такъв
  образ не тръгва на нашия v1.3;
- за нашата платка трябва:
  ```
  CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
  CONFIG_ESP32P4_REV_MIN_100=y        # v1.0, максимумът става v1.99
  ```
- последици от `SELECTS_REV_LESS_V3`: CPU по подразбиране **360 MHz** (400 MHz е
  само за v3), PSRAM най-много 200 MHz (250 MHz е само за v3);
- образ за v1.x не тръгва на P4 v3.x и обратно. За Custom платка с P4 v3
  ревизията трябва да е отделна настройка (за фаза 2).

### esp_hosted и esp_wifi_remote

От API-то на регистъра (`components.espressif.com/api/components/espressif/<име>`):

| Компонент | Последна | Дата | Зависимости |
|---|---|---|---|
| `espressif/esp_hosted` | **3.1.0** | 2026-11-05 | `idf >=5.5`; USB компоненти само при USB транспорт |
| `espressif/esp_hosted` | 3.0.10 | 2026-10-22 | `idf >=5.5` |
| `espressif/esp_wifi_remote` | **1.6.5** | 2026-09-23 | `esp_hosted >=2.11` (за esp32p4/esp32h2), `wifi_remote_over_eppp >=0.1`, `idf >=5.3` |

- `esp_wifi_remote` 1.6.5 има `idf_v6.0/` и го избира по `IDF_VERSION_MAJOR.MINOR`
  (няма `idf_tag_v6.0.x`, така че 6.0.2 взима `idf_v6.0`).
- В примерите на esp_hosted 3.1.0 `esp_wifi_remote` е нужен само за IDF `< 6.1`.
  От 6.1 е в самия IDF. За 6.0.2 е външен компонент.
- В `esp_wifi_remote` 1.6.3 имаше ограничение `esp_hosted <3.0`. В 1.6.4 е махнато.

**Съвместимост host ↔ C6** (`docs/migration.md` и `docs/troubleshooting.md` на esp_hosted 3.1.0):

| Host (P4) | Firmware на C6 | |
|---|---|---|
| 3.x | 3.x | поддържано |
| 3.x | 0.0.6+ / 1.x / 2.x | обратно съвместимо, препоръчва се обновяване на C6 |
| < 3.x | 3.x | не се поддържа |

Препоръката на Espressif е една и съща версия на host-а и на C6. Нашият C6
firmware (фаза 1) ще е на същата версия на esp_hosted.

**Проба с build:** малък проект в scratchpad (не е в репото): `esp_wifi_init()` през
`esp_wifi_remote`, `esp_hosted_get_coprocessor_fwversion()`,
`esp_hosted_get_coprocessor_app_desc()`. IDF 6.0.2, target `esp32p4`, с
`SELECTS_REV_LESS_V3` и `REV_MIN_100`. Компилира се. Единственото предупреждение
е в esp_hosted (`eh_host_sta_fsm.c:152`, `start_req` не се ползва). В получения
`sdkconfig`: CPU 360 MHz, `REV_MIN_FULL` = 100, `REV_MAX_FULL` = 199. Пробата не е
прошивана на платката.

Стойностите по подразбиране на esp_hosted 3.1.0 за P4 съвпадат със схемата на
DFR1172: SDIO slot 1, CMD 19, CLK 18, D0–D3 14–17, reset на C6 54 (проверява се в 0.3).

### Заключени версии

Решение на Кристиан (2026-11-11): последните версии на esp_hosted и esp_wifi_remote.
Версията на ESP-IDF още се решава (v6.0.2, v6.0.3 или v6.1). Пробата с v6.1 е по-долу.

### Проба с ESP-IDF v6.1 (2026-11-11)

v6.1 (публикувана на 2026-08-27, commit `fff9895c82d`) е инсталирана отделно в
`~/esp-idf-v6.1` (`install.sh esp32s3,esp32p4,esp32c6`). `~/esp-idf` (v6.0.2) не е пипан.
Компилаторите са същите като в 6.0.2 (`esp-15.2.0_20251204`), нови са само openocd и Python средата.

**esp_wifi_remote е в самия IDF** (`components/esp_wifi/remote`, включва се с
`CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED`). На v6.1 за P4 трябва само `espressif/esp_hosted`.
Външният `esp_wifi_remote` и косвените му зависимости (`wifi_remote_over_eppp`, `eppp_link`,
`esp_serial_slave_link`) отпадат.

**Миграция 6.0 → 6.1** (ръководство за миграция и „Breaking Changes“ в бележките на v6.1),
сверено с `main/`, `common/`, `components/`, `speaker/main/` и `esp-airplay2-core/components`:
- LCD, UART wakeup, MQTT, SPI `ESP_INTR_FLAG_SHARED`, SPI Flash, GPIO макроси и ROM функции,
  `soc/uart_channel.h`: не ги ползваме;
- mbedTLS 4.1.0: 6.0.2 вече е на 4.1.0, а нашата криптография е през libsodium;
- P4 по подразбиране v3.0: и в 6.0.2 е така, задаваме `SELECTS_REV_LESS_V3` ръчно;
- `idf.py flash` прошива само промененото (`-a` за пълно прошиване).

**Build от чисто копие на `HEAD` (`4550bc9`)**, с нов `sdkconfig` от defaults, при еднакви условия:

| | v6.0.2 | v6.1 |
|---|---|---|
| Gateway S3 | OK, 0 предупреждения, `0x14a6d0` | OK, 0 предупреждения, `0x14ee60` (+18 KB) |
| Колона S3 | OK, 3 предупреждения (FDK), `0x1b7e60` | OK, същите 3, `0x1bc7b0` (+19 KB) |
| Проба P4 (esp_hosted 3.1.0) | OK, с външен esp_wifi_remote 1.6.5 | OK, само с esp_hosted, `0x8be90` |

Външните компоненти не се сменят: mdns 1.15.1, libsodium 1.0.22~1, esp_audio_codec 2.6.2,
cjson 1.7.19~2, led_strip 3.0.3, ap2_core и apple_clock v1.0.0.

**Разлики в получения `sdkconfig`** (Gateway и колона еднакво):
- сменена стойност по подразбиране: `CONFIG_ESP_INT_WDT_TIMEOUT_MS` 300 → 800 ms
  (watchdog на прекъсванията). Ако искаме старото поведение, задаваме 300 в `sdkconfig.defaults`;
- махнати опции, които не ползваме: `CONFIG_FREERTOS_SMP`, `CONFIG_ESP_TLS_USE_SECURE_ELEMENT`,
  `CONFIG_ESP_HTTPS_SERVER_EVENT_POST_TIMEOUT` и вътрешни `CONFIG_SOC_*`;
- 116 нови опции, повечето вътрешни `CONFIG_SOC_*`.

**Решение на Кристиан (2026-11-11): проектът минава на v6.1.** В репото:
- `main/idf_component.yml` и `speaker/main/idf_component.yml`: `idf: ">=6.1"`;
- `dependencies.lock` и `speaker/dependencies.lock`: `idf` 6.1.0, другите версии са същите;
- README (инструкцията за инсталиране) и коментарите за v6.0.2 в `log_stream.c` и `components/wifi/README.md`.

Поведението от тези коментари е проверено в кода на v6.1:
- WebSocket handshake-ът пак не вика handler-а, а само `ws_post_handshake_cb` (`httpd_uri.c`);
- `httpd_ws_recv_frame` има нов `left_len` и частично четене (`httpd_ws_recv_frame_part`).
  Нашето четене (първо с `max_len` = 0, после целият кадър) работи както преди;
- `httpd_queue_work` винаги брои местата с семафор и връща `ESP_FAIL` при пълна опашка.
  `log_stream.c` и `homekit_reset.c` проверяват резултата;
- `wifi_event_sta_scan_done_t` е същото (`status`, `number`, `scan_id`);
- новият `WIFI_AUTH_UNKNOWN` попада в `default` на `sec_of()` (`WL_SEC_OTHER`).

Build в репото на v6.1: Gateway `0x14ee60`, 0 предупреждения; колона `0x1bc760`, същите
3 предупреждения от FDK. Съществуващият `sdkconfig` на Gateway-а пази `INT_WDT_TIMEOUT_MS` = 300;
нов `sdkconfig` от defaults получава 800.

Тест на хардуера (2026-11-11): Кристиан потвърди, че S3 Gateway-ът и колоните работят на v6.1.
Отворени: CI на `esp-airplay2-core` (сега `espressif/idf:v6.0.2`) и
спецификациите в `docs/*-spec.md`, които още пишат ESP-IDF v6.0.2.

| | Версия |
|---|---|
| ESP-IDF | `v6.0.2` |
| `espressif/esp_hosted` | `==3.1.0` |
| `espressif/esp_wifi_remote` | `==1.6.5` |
| преходни, от lock файла | `wifi_remote_over_eppp` 0.3.3, `eppp_link` 1.1.6, `esp_serial_slave_link` 1.1.2 |

`esp_wifi_remote` винаги дърпа `wifi_remote_over_eppp`, дори да не се ползва.
Те се заключват в отделния lock файл за P4 (фаза 2.0), а не в `idf_component.yml`.

Алтернатива: `esp_hosted` `3.0.10`. Версия 3.1.0 е на 6 дни и сменя SDIO
(stream mode с 14 кадъра в движение, размерът на RX буфера идва от C6).
Espressif препоръчва последната версия. Ако 0.3 покаже проблем със SDIO, опитваме 3.0.10.

### Източници

- ESP-IDF v6.0.2: `components/esp_hw_support/port/esp32p4/Kconfig.hw_support`,
  `components/esp_system/port/soc/esp32p4/Kconfig.cpu`, `components/esp_psram/esp32p4/Kconfig.spiram`
- https://components.espressif.com/components/espressif/esp_hosted (3.1.0, commit `235aca9`)
- https://components.espressif.com/components/espressif/esp_wifi_remote (1.6.5, commit `ad54972`)
- esp_hosted 3.1.0: `CHANGELOG.md`, `docs/migration.md`, `docs/troubleshooting.md`, `README.md`
- DFRobot wiki: https://wiki.dfrobot.com/dfr1172/
- Схема: https://dfimg.dfrobot.com/wiki/21103/DFR1172_firebeetle-esp32-p4r32-development-board_schematics_V1.0.pdf

## 0.2 C6: заводски firmware и прошиване (в процес)

### Какво казва C6 на P4 (проба, 2026-11-11)

Пробата от 0.1 (IDF v6.1, esp_hosted 3.1.0) е прошита на P4. Заводският firmware на P4 е изтрит.
C6 не е пипан. Лог на P4:

```
eh_sdio: transport[host]: SDIO 4-bit 40000 kHz CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 RESET=54
eh_sdio: Card init success, TRANSPORT_RX_ACTIVE
eh_sdio: SDIO Host operating in STREAMING MODE
eh_init_evt: slave chip id: 0x0d (esp32c6)
eh_init_evt: capabilities: 0x0d   (WLAN over SDIO, HCI over SDIO, BLE only)
eh_init_evt: Default RPC to V2
eh_init_evt: esp-hosted fw versions: host=3.1.0 coprocessor=0.0.0
E eh_init_evt: major version mismatch — OTA coprocessor from host
eh_init_evt: CP without SDIO SW_AGGR; compatible streaming mode enabled
eh_init_evt: Missing SDIO_MODE TLV; Continue anyway.
W eh_host_feat_rpc: request: no response uid=3 msg_id=350 (5000 ms)   GetCoprocessorFwVersion
W eh_host_feat_rpc: request: no response uid=4 msg_id=350 (5000 ms)   app_desc
```

- SDIO по схемата работи (4 бита, 40 MHz), C6 е жив и с firmware на ESP-Hosted.
- C6 не изпраща в init събитието нито версия на firmware-а, нито версия на RPC. Затова host-ът показва 0.0.0.
- `esp_wifi_init()` през C6 мина, т.е. C6 отговаря на RPC V2.
- В 3.1.0 V2 са съобщенията `Rpc*` (`common/serializers/README.md`), същите като в MCU 0.0.x–2.x.
  V1 (`CtrlMsg`) е само за Linux host-ове.
- `GetCoprocessorFwVersion` (350) е добавен в 1.0.0 (`docs/design/rpc-reference.md`). C6 не му
  отговаря, значи заводският firmware е **по-стар от 1.0.0**.

### Инструкцията на DFRobot

https://wiki.dfrobot.com/dfr1172/docs/21646 („ESP32-C6 Firmware Update Guide“):
- USB-TTL към тестовите точки на гърба: `ESP32C6_RX`, `ESP32C6_TX`, `GND`, `3V3`,
  `ESP32C6_RST`, `ESP32C6_IO9/BOOT`. На схемата за свързване: 5V от USB-TTL към VCC на платката.
- Flash Download Tool, ESP32-C6, един файл на адрес `0x0` (`C6_v14_eco2_0022.bin`), SPI 40 MHz, DIO,
  115200. Erase, после START. Ако не тръгне: задържа се RST на платката и START.
- Заводски firmware: https://dfimg.dfrobot.com/62d52567aa9508d63a4247a1/wikien/db80be618f431512e03bce34dcc8403d.zip
  (655 221 байта, 2026-01-27, SHA-256 `a8833134…fd32586`). Вътре е един файл
  `C6_v14_eco2_0022.bin` (1 178 352 байта, 2025-02-12): bootloader + таблица на дяловете + app.
- Изходният код е посочен като `esp-hosted-mcu/tree/main/slave`, т.е. отпреди 3.x (в 3.x `slave/` вече го няма).

### OTA на C6 от P4 (esp_hosted 3.1.0)

- Веригата е `OTABegin` → `OTAWrite` → `OTAEnd` (RPC 272–274, от 0.0.6) → `OTAActivate`
  (RPC 266, само за firmware на C6 от 2.6.0 нагоре). При по-стар C6 `OTAEnd` сам задава boot дяла
  (`examples/ota/coprocessor_ota/README.md`).
- При несъвпадение на версиите host-ът не спира, за да остане OTA каналът
  (`eh_host_mcu_transport_init_event.c`).
### Какво има в заводския образ на C6

Прочетено от `C6_v14_eco2_0022.bin` (bootloader на 0x0, chip id 13 = ESP32-C6):

| Дял | Тип | Адрес | Размер |
|---|---|---|---|
| nvs | data/nvs | `0x9000` | 16 KB |
| otadata | data/ota | `0xd000` | 8 KB |
| phy_init | data/phy | `0xf000` | 4 KB |
| ota_0 | app | `0x10000` | 1536 KB |
| ota_1 | app | `0x190000` | 1536 KB |

`esp_app_desc` в ota_0: project `network_adapter`, version `83efce6-dirty`, 2024-10-24 19:05:47,
IDF `v5.4-dev-3886-gfcd26a2992-dirty`. ota_1 е празен.

Commit `83efce6` в `espressif/esp-hosted-mcu` (2024-10-14, клон `feature/esp_as_mcu_host`):
- `idf_component.yml`: `version: "0.0.22"` (в регистъра 0.0.22 е от 2024-10-14);
- `slave/CMakeLists.txt`: `PROJECT_VERSION` **0.0.6**. Това печата C6 като версия на firmware-а.
  По таблицата на esp_hosted 3.x (0.0.6+) е обратно съвместимо;
- `slave/main/slave_control.c`: има `OTABegin`/`OTAWrite`/`OTAEnd`. `OTAEnd` прави `esp_ota_end`,
  `esp_ota_set_boot_partition` и рестарт с таймер. `OTAActivate` и `GetCoprocessorFwVersion` ги няма.

**Изводи за OTA от P4 (б):**
- C6 има два OTA дяла, така че OTA от P4 е възможен без USB-TTL. Заводският firmware остава в ota_0.
- Host-ът (3.1.0) не трябва да чака `OTAActivate`, когато C6 не е съобщил версия (0.0.0):
  C6 0.0.6 не отговаря на него и сам се рестартира след `OTAEnd`.
- OTA сменя само app-а. Bootloader-ът (IDF v5.4-dev) и таблицата на дяловете остават заводските.
  Нашият firmware за C6 трябва да се побира в 1536 KB, да е за ota дял на `0x10000`/`0x190000`
  и да тръгва от bootloader-а на IDF v5.4-dev. Последното трябва да се провери на хардуера.

### Проба: OTA на C6 от P4 (2026-11-11)

Два проекта в scratchpad (не са в репото), IDF v6.1, esp_hosted `==3.1.0`:
- **C6:** минимален копроцесор (`CONFIG_ESP_HOSTED_CP=y`, `CP_FOR_MCU`, `CP_RPC_V2`, `CP_FEAT_WIFI`,
  транспорт SDIO, BT изключен), таблица на дяловете като заводската, `PROJECT_VER` `0.2-ota-test`.
  `c6_cp_test.bin` = 1 150 288 байта (`0x118d50`), 27 % свободно в 1536 KB дял;
- **P4:** образът за C6 е вграден (`EMBED_FILES`). Ако C6 не съобщи версия, P4 прави
  `esp_hosted_slave_ota_begin` → `write` на парчета по 1400 байта → `end`, без `activate`,
  чака 5 s и се рестартира. При следващо пускане само показва версията.

Лог на P4:
```
eh_init_evt: esp-hosted fw versions: host=3.1.0 coprocessor=0.0.0
p4_c6_ota: C6 reports no version (factory 0.0.6): OTA of 1150288 bytes, chunk 1400
p4_c6_ota: ota_begin: ESP_OK (241 ms)
p4_c6_ota: ota_write done: 8264 ms
p4_c6_ota: ota_end: ESP_OK
E eh_sdio: sdio_write_streaming_iter: ... Failed to send data      <- C6 се рестартира сам след OTAEnd
W eh_host_xport: TRANSPORT_FAILURE: forcing host reset             <- P4 се рестартира
--- ново пускане на P4 ---
eh_init_evt: esp-hosted fw versions: host=3.1.0 coprocessor=3.1.0 (match)
p4_c6_ota: C6 fw version ESP_OK: 3.1.0
```

**Резултат:** OTA от P4 работи със заводския C6. 1,15 MB за около 8,3 s (≈140 KB/s при парчета
по 1400 байта), плюс 0,24 s за `OTABegin`. Новият app (IDF v6.1) тръгва от заводския bootloader
(IDF v5.4-dev). Заводският firmware остава в ota_0, а C6 сега тръгва от ota_1.

Когато C6 се рестартира след `OTAEnd`, SDIO връзката пада и host-ът на 3.1.0 рестартира P4
(`TRANSPORT_FAILURE: forcing host reset`). В Gateway-а (фаза 5) това трябва да се има предвид:
P4 ще се рестартира след всяко обновяване на C6.

**`app_desc` на C6:** на host-а в 3.1.0 `esp_hosted_get_coprocessor_app_desc()`
(`eh_host_sys_get_cp_app_desc`, `host/features/eh_host_feat_system/src/eh_host_sys.c:231`)
не праща RPC. Той попълва само `version` = версията на esp_hosted (`3.1.0`), а `project_name`
и `idf_ver` остават празни. C6 има обработка за истинския `app_desc`
(`eh_cp_feat_rpc_ext_v2_handler_req_system.c`, `esp_app_get_description()`), но host-ът не я вика.
Затова точки 1.1 и 5.2 от картата (P4 познава нашия firmware по името и версията в `esp_app_desc`)
не могат да станат така с 3.1.0. Трябва друг път. Решава се във фаза 1.

**Състояние на платката сега:** на P4 е пробата `p4_c6_ota`; на C6 в ota_1 е стандартният
копроцесор на esp_hosted 3.1.0 (без BT); в ota_0 е заводският 0.0.6.

**USB-TTL (а):** не е пробван. Изисква USB-TTL адаптер на 3,3 V към тестовите точки и се ползва само за възстановяване.

## 0.3 Минимален пример: host на P4 + копроцесор на C6, Wi-Fi и throughput (2026-11-11)

**Пинове.** По wiki-то на DFRobot и по стойностите на esp_hosted 3.1.0 за P4:
SDIO slot 1, CLK 18, CMD 19, D0–D3 14–17, reset на C6 54. SDIO тръгва на тях (4 бита, 40 MHz,
streaming mode). Схемата (PDF) още не е прегледана.

**Проба** `p4_wifi_tp` (scratchpad, не е в репото), IDF v6.1, esp_hosted 3.1.0 на двете страни
(на C6 е стандартният копроцесор от 0.2): STA, `WIFI_PS_NONE`, TCP сървър на порт 5001.
Клиентът е Mac (MacBook Air, също по Wi-Fi към същата точка за достъп), 4 × 10 s.

- Връзка: IP за 3,06–3,23 s от `esp_wifi_start()`. RSSI -56…-62 dBm, канал 11 (2,4 GHz),
  `phy_11ax` = 1 (Wi-Fi 6). `esp_wifi_sta_get_ap_info()` през C6 работи.
- Пинг Mac → P4: средно 20–22 ms (5,5–77 ms).

| Настройки | P4 TX | P4 RX |
|---|---|---|
| lwIP по подразбиране (`TCP_WND` / `SND_BUF` = 5760) | 2,7–2,8 Mbit/s | 2,7–2,8 Mbit/s |
| lwIP като Gateway-а (`TCP_WND` 65535, `SND_BUF` 32768, mbox 64) | 10,1–11,0 Mbit/s | 11,8–11,9 Mbit/s |
| + Wi-Fi като Gateway-а (`WIFI_RMT_` static RX 24, dynamic RX/TX 64, BA TX 12 / RX 32) | 11,8–12,1 Mbit/s | 13,5–15,7 Mbit/s |

- С настройките по подразбиране скоростта е ограничена от TCP прозореца: 5760 B / 22 ms ≈ 2 Mbit/s.
- Настройките `CONFIG_WIFI_RMT_*` на P4 се прилагат на C6 при `esp_wifi_init()` през RPC и помагат.
- Mac-ът също е по Wi-Fi, затова числата са за целия път Wi-Fi → AP → Wi-Fi. Не показват само
  тавана на P4 + C6. За сравнение трябва същото измерване със S3 или с жичен клиент.
- Грешки на SDIO или транспорта по време на теста: няма.

### 0.3 сравнение с данните на Espressif (2026-11-11)

esp_hosted 3.1.0 `docs/design/performance.md`, точка 1.1 (P4 + C6, SDIO 4 бита, 40 MHz, 2,4 GHz, „open-air“,
iperf към PC на кабел към AP):

| Тип | Посока | Mbit/s |
|---|---|--:|
| Raw (само SDIO) | P4 → C6 | 72 |
| Raw (само SDIO) | C6 → P4 | 80 |
| TCP | P4 → PC | 32 |
| UDP | P4 → PC | 50 |
| TCP | PC → P4 | 30 |
| UDP | PC → P4 | 49 |

Препоръчаните им настройки на P4: `WIFI_RMT_` static RX 16, dynamic RX/TX 64, BA TX/RX 32;
`LWIP_TCP_SND_BUF_DEFAULT` = `LWIP_TCP_WND_DEFAULT` = 65534, mbox 64, `LWIP_TCP_SACK_OUT=y`.

Нашите измервания с тези настройки (същото място, 4 × 10 s):

| SDIO | P4 TX | P4 RX |
|---|---|---|
| 40 MHz | 15,1–15,8 Mbit/s | 12,8–13,0 Mbit/s |
| `SDIO_CLK_KHZ=50000` | 12,7–13,7 Mbit/s | 10,2–11,7 Mbit/s (шум; часовникът пак е 40 MHz) |

- **50 MHz не е възможен.** `eh_host_port_sdio.c` подава `max_freq_khz` на SDMMC драйвера,
  а той дели 160 MHz на цяло число: 160/3 = 53,3 > 50, затова остава 160/4 = 40 MHz
  (`bus applied: clk_khz=40000`). Таванът е 40 MHz, като в измерванията на Espressif.
- Init събитието показва `SDIO SW_AGGR negotiated (e2h=15872B h2e=15872B)`, streaming и от двете страни.
- **Mac-ът не ограничава:** 5 GHz, канал 56, 80 MHz, 802.11ax, 1200 Mbit/s, -44 dBm.
  Ограничава 2,4 GHz частта: AP на канал 11, 20 MHz (`second 0`), RSSI -54…-62 dBm, пинг
  средно 20 ms. Съседни мрежи на 2,4 GHz: канал 9 (20 MHz), 6 (40 MHz), 3 (20 и 40 MHz), 1 (3 мрежи).
- ESP32-C6 има Wi-Fi само на 2,4 GHz (като S3). Разликата спрямо 30 Mbit/s на Espressif идва
  най-вероятно от радиосредата (лаборатория срещу дом с няколко съседни мрежи). Това не е доказано:
  трябва S3 на същото място или raw тест на SDIO.

**Колко трябва на Gateway-а** (от кода: `gw_relay.c` праща целия поток без промяна на всяка колона,
буферираният режим е ограничен до `GW_RELAY_PACE_FACTOR` 2,0 × реалното време, плюс 2 s на пълна скорост):
- нужно ≈ брой колони × битрейт на потока × 2 (буфериран) или × 1 (realtime), плюс входящия поток;
- стерео AAC 256 kbit/s към 3 колони: ≈ 1,5 Mbit/s;
- битрейтът на 5.1/7.1 AAC от Apple не е измерен тук. Трябва да се види от `RELAY rx` в лога на S3 Gateway-а.

### 0.3 сравнение със S3 на същото място (2026-11-11)

Проба `s3_wifi_tp`: същият код без esp_hosted, IDF v6.1, настройките на S3 Gateway-а от репото
(`ESP_WIFI_` static RX 24, dynamic RX 85, static TX 32, BA TX 12 / RX 32; lwIP `SND_BUF` 32768,
`WND` 65535, mbox 64; PSRAM octal 80 MHz, 240 MHz, Wi-Fi и lwIP на CPU1). Платка: ESP32-S3
(QFN56) v0.2, 8 MB PSRAM, 16 MB flash. Същото място, същият AP и Mac, 4 × 10 s.

- Връзка: IP за 4,2 s; RSSI -55 dBm, канал 11, `phy_11ax` = 0 (S3 е само до 802.11n). Пинг средно 23,5 ms.

| | TX (от платката) | RX (към платката) |
|---|---|---|
| **S3**, настройки на Gateway-а | 8,0–8,2 Mbit/s | 6,9–7,1 Mbit/s |
| **P4 + C6**, същите настройки (`WIFI_RMT_` 24/64/64, BA 12/32) | 11,8–12,1 Mbit/s | 13,5–15,7 Mbit/s |
| **P4 + C6**, настройките на Espressif | 15,1–15,8 Mbit/s | 12,8–13,0 Mbit/s |

**Извод:** на това място P4 + C6 дава 1,5–2 пъти повече TCP от S3, с който Gateway-ът работи днес.
Throughput-ът не е пречка за P4 Gateway-а. Пълният таван на Espressif (≈ 30 Mbit/s) не се достига
заради радиосредата на 2,4 GHz, която е обща и за двата чипа.

## 0.4 Функциите на Gateway-а през C6 (2026-11-12)

### По manifest-а на esp_hosted 3.1.0

Всички `esp_wifi_*` и `esp_netif_*`, които Gateway-ът вика (`components/wifi`, `web_server`, `main/gw_speakers.c`,
`common/surround/tsf_fit.c`, `esp-airplay2-core`): `init`, `set_storage`, `set_mode`, `set_config`
(STA и AP), `start`, `connect`, `disconnect`, `scan_start`, `scan_stop`, `scan_get_ap_num`,
`scan_get_ap_records`, `clear_ap_list`, `sta_get_ap_info`, `set_ps`, `esp_netif_set_hostname`,
`esp_netif_create_default_wifi_sta/ap`. Нито една не е в списъка с неподдържани на
`tools/wifi_coverage/wifi_coverage_manifest.yml`, значи esp_hosted ги дефинира.
Изключение: `esp_wifi_get_tsf_time: {unsupported: API is not implemented in hosted}`.

Полета на структурите: `wifi_sta_config_t`, `wifi_ap_config_t`, `wifi_ap_record_t`, `wifi_pmf_config_t`,
`wifi_scan_threshold_t` и събитията се пренасят изцяло. Единственото непренасяно поле от тях е
`wifi_scan_config_t.max_scan_ap_num` (`gap`); Gateway-ът не го задава.

### Проба `p4_gw_wifi`

Истинският `components/wifi` на Gateway-а (със `settings` и `helpers`), взет без промяна от репото
(`EXTRA_COMPONENT_DIRS`), на P4 с C6 (esp_hosted 3.1.0), `CONFIG_AIRPLAY_ROLE_GATEWAY=y`,
Wi-Fi/lwIP като Gateway-а. Редът е като в `main/main.c`: `nvs_flash_init`, `settings_init`, `wifi_init_apsta(NULL, NULL)`.

**Station** (една запазена мрежа):
- `wifi: scanning for saved networks`, `scan done: 14 access points`; `wifi_wait_connected`: да, след 7,47 s;
- `wifi_get_ip_str`: `ESP_OK`; `esp_wifi_sta_get_ap_info`: `ESP_OK`, RSSI -53, канал 11, auth 7, ax 1;
- `wifi_set_hostname("P4 Gateway Test")` → `host name "P4-Gateway-Test"`;
- `wifi_scan()` × 3: 10, 10 и 14 мрежи, по 2,86 s; връзката остава;
- `esp_wifi_get_tsf_time`: `W esp_wifi_remote_weak: esp_wifi_remote_get_tsf_time unsupported`, връща `UINT64_MAX`;
- **`wifi_get_mac_str()` връща празно:** `E system_api: 0 mac type is incorrect (not found)` при
  `esp_read_mac(ESP_MAC_WIFI_STA)`. Същото при името по подразбиране (`ESP32-AirPlay-000` вместо последните байтове на MAC-а).
  Засяга решение 6 и 7-те места с `esp_read_mac()`. Разглежда се в 0.10.

**Setup AP** (NVS изтрит, няма запазена мрежа):
- `access point on: SSID "ESP32-AirPlay-Setup" (open), no saved network`;
- `captive DNS on UDP 53, answers 192.168.4.1`. Без PSRAM в пробата първо беше `captive DNS did not start:
  ESP_ERR_NO_MEM`: `dns_server.c` прави задачата си с `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)`.
  С `CONFIG_SPIRAM=y` тръгва. На P4: `Found 32MB PSRAM device`, 200 MHz, HEX, тестът на паметта минава;
- iPhone: `client joined` / `client left`; три заявки `GET /hotspot-detect.html` (проверката на Apple за
  captive portal през нашия DNS); `GET /` и `/favicon.ico`. Кристиан вижда тестовата страница;
- `wifi_scan()` на всеки 15 s докато AP е включен: 11–16 мрежи, по 2,84 s, без прекъсване на AP-то.

**Извод:** функциите на Gateway-а за Wi-Fi работят през C6 без промяна в `components/wifi`. Остават
TSF (решава се с хардуерното засичане, фази 1 и 3) и MAC (0.10).

## 0.5 Multicast през C6 (2026-11-12)

Проба `p4_mcast` (scratchpad), IDF v6.1, esp_hosted 3.1.0, `espressif/mdns` `^1.14.1` (както Gateway-а),
mDNS настройките на Gateway-а (`MDNS_MAX_SERVICES=4`, задача и памет в PSRAM), `LWIP_NETBUF_RECVINFO=y`
за `IP_PKTINFO`. PTP сокетите са като в `apple_clock.c`: UDP 319 и 320 на `INADDR_ANY` + `IP_ADD_MEMBERSHIP` 224.0.1.129.

**mDNS:**
- Mac `dns-sd -B _p4test._tcp`: `Add … _p4test._tcp. P4 mDNS test` веднага. P4 получава multicast
  заявките и отговаря;
- Mac `dns-sd -G v4 p4-mcast-test.local`: 192.168.1.17, TTL 120; `ping p4-mcast-test.local` работи;
- P4 `mdns_query_ptr("_airplay", "_tcp")` на всеки 20 s: 7 отговора, всичките с IPv4, за 3,09 s.
  Multicast отговорите на Apple устройствата стигат до P4.

**PTP multicast** (тестови PTPv2 заглавки от Mac, 20 от всеки вид, през 50 ms):

| Порт | Към | Тип | Получени |
|---|---|---|---|
| 319 | 224.0.1.129 | Sync | 20 / 20 |
| 320 | 224.0.1.129 | Announce | 19 / 20 |
| 320 | 192.168.1.17 (unicast) | Delay_Resp | 20 / 20 |

`IP_PKTINFO` различава multicast от unicast на P4. Един изгубен multicast пакет: AP-то праща multicast
без потвърждение, на основна скорост. Това важи и за S3.

**Извод:** mDNS и PTP multicast стигат до P4 през C6 без специални настройки на esp_hosted.

## 0.6 Линията WAKEUP от край до край (2026-11-12)

### В esp_hosted 3.1.0

- C6 (CP): `ESP_HOSTED_CP_FEAT_HOST_PS` е `default n` (`coprocessor/features/eh_cp_feat_host_ps/Kconfig.ext`).
  Пинът `ESP_HOSTED_CP_FEAT_HOST_PS_HOST_WAKEUP_GPIO` е `default 2 if ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD`.
  Без функцията `EH_CP_FEAT_HOST_PS_WAKEUP_GPIO` е -1 (`eh_cp_master_config.h`) и esp_hosted не пипа пина.
- P4 (host): `ESP_HOSTED_HOST_FEAT_POWER_SAVE` е `default n`, а в нашите build-ове е `is not set`.
  `ESP_HOSTED_HOST_FEAT_POWER_SAVE_WAKEUP_GPIO` е `default 6 if ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD`.
- Значи на платката на Espressif линията е C6 GPIO2 → P4 GPIO6, като в картата и в wiki-то на DFRobot (WAKEUP = GPIO6).

### Проба

- C6: `c6_wake`, стандартният копроцесор 3.1.0 + задача, която на всеки 125 ms (`vTaskDelayUntil`)
  вдига GPIO2 за около 20 µs (`esp_rom_delay_us`). Само програмно засичане, ETM е за 0.7.
- P4: `p4_wake`, GPIO6 вход с pull-down, прекъсване на нарастващ фронт, `esp_timer_get_time()` в ISR-а.
  Първо 10 s със стария C6, после OTA на `c6_wake` (`begin`/`write`/`end`/`activate`, C6 вече е 3.1.0), после броене.

Резултат:
```
before OTA (C6 firmware already there): 0 rising edges in 10 s, level now 0
ota_end: ESP_OK / ota_activate: ESP_OK
C6 has the c6_wake image
c6_wake pulses: 40 rising edges in 5 s, level now 0
  interval mean 125003.0 us, min 125000…125003 us, max 125004…125009 us   (12 прозореца по 5 s)
```

- **Линията работи:** GPIO2 на C6 стига до GPIO6 на P4. 40 от 40 импулса за всеки 5 s, без изпуснати.
- **Стандартният ESP-Hosted не ползва линията**, когато енергоспестяването на host-а е изключено:
  0 фронта за 10 s със стандартния копроцесор 3.1.0.
- Средният интервал е 125 003 µs по часовника на P4 срещу 125 000 µs по FreeRTOS на C6: двата
  кристала се разминават с около 24 ppm. Разсейването с прекъсване и програмен импулс е около ±2 µs
  (едно отклонение +6 µs). Това е горната граница без ETM.
- След `OTAActivate` C6 се рестартира, SDIO пада (`SDIO aggr: unrecoverable write failure`) и P4 се рестартира, като в 0.2.

Схемата на DFRobot (PDF) още не е прегледана. Пинът е потвърден от wiki-то и от измерването.

## 0.7 ETM (в процес, 2026-11-12)

### В ESP-IDF v6.1

- `soc_caps.h` на esp32c6 и esp32p4: `SOC_ETM_SUPPORTED`, `SOC_GPIO_SUPPORT_ETM`, `SOC_TIMER_SUPPORT_ETM` = 1.
- GPTimer: събитие `GPTIMER_ETM_EVENT_ALARM_MATCH`, задачи `START/STOP/EN_ALARM/RELOAD/CAPTURE`
  (`esp_hal_timg/include/hal/timer_types.h`). `TIMER_LL_ETM_TASK_TABLE` на P4 има `TG0/TG1_TASK_CNT_CAP_TIMER0/1`,
  а на C6 `TIMER0/1_TASK_CNT_CAP_TIMER0`. API: `gptimer_new_etm_event()`, `gptimer_new_etm_task()`,
  `gptimer_get_captured_count()`. Callback има само `on_alarm`; capture няма прекъсване, затова
  стойността се чете от прекъсването на GPIO-то за същия фронт.
- GPIO: `gpio_new_etm_event()` + `gpio_etm_event_bind_gpio()` (фронт POS/NEG/ANY), `gpio_new_etm_task()` +
  `gpio_etm_task_add_gpio()` (SET/CLR/TOG). **Всички действия за един GPIO трябва да са от един
  GPIO ETM task канал** (`gpio_etm.c`: „the task does not belong to the GPIO ETM task channel that the
  GPIO has already binded to“). SET и CLEAR на един пин се правят с една `gpio_new_etm_task()` с `actions[]`.
- GPTimer източници: `PLL_F80M`, `RC_FAST`, `XTAL` и на двата чипа. За тестовете: `PLL_F80M`, 40 MHz (25 ns).

### Първи опит

- C6 `c6_etm`: GPTimer A (125 ms, auto reload) ALARM → ETM → GPIO2 SET; GPTimer B (същият период,
  20 µs по-късно) ALARM → ETM → GPIO2 CLEAR.
- P4 `p4_etm`: GPIO6 POS → ETM → GPTimer CAPTURE (40 MHz); прекъсването на GPIO6 само чете
  `gptimer_get_captured_count()` и `esp_timer_get_time()`.

**ETM capture на P4 със старите програмни импулси от `c6_wake`** (преди OTA, 79 интервала):
средно 125 002 944,9 ns, sd 319 ns, разлика между най-малкия и най-големия интервал 2,9 µs. Със същите фронтове
`esp_timer` в ISR-а дава sd 1,6 µs и разлика 15 µs. Остатъкът е от програмния импулс на C6, не от засичането на P4.

**Грешка в теста (моя):** в `c6_etm` SET и CLEAR бяха в два различни GPIO ETM task канала. Вторият
`gpio_etm_task_add_gpio()` връща `ESP_ERR_INVALID_ARG`, `ESP_ERROR_CHECK` спира програмата и C6 се
рестартира в цикъл. P4 вижда `Card init success`, но после `eh_reconfigure: bring-up timed out (5000ms)`
и `esp_wifi_init: ESP_FAIL`. OTA от P4 вече не е възможен. Заводският bootloader няма rollback, а ota_0
(`c6_wake`, третото OTA) не се пуска сам. Заводският 0.0.6 вече го няма в C6, той е само в zip-а на DFRobot.

**Възстановяване:** само с USB-TTL към тестовите точки на C6 (`RX`, `TX`, `GND`, `3V3`, `RST`,
`IO9/BOOT` към GND при рестарт → download mode) и `esptool`. Поправеният `c6_etm` е готов: един канал
с `actions = {SET, CLR}`, без `ESP_ERROR_CHECK` извън NVS и event loop.

**Поука за фаза 1 и 5:** firmware-ът на C6 не бива да спира при грешка извън ядрото на esp_hosted, иначе
OTA от P4 става невъзможен. При прошиване с USB-TTL може да се сложи нов bootloader с
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, за да се връща сам към предишния образ.

**Платка 1** (P4 MAC `30:ed:a0:ea:1b:a0`) е оставена с блокирания C6 до възстановяване с USB-TTL.
Работата продължава на **платка 2** (втора DFR1172).

Поправеният `c6_etm` има и **предпазна мрежа**: ETM частта тръгва 30 s след старта. Дори при грешка
в тестовия код C6 работи като копроцесор поне 30 s след всеки рестарт. OTA от P4 отнема около 9 s.

### Втори опит на платка 2 (P4 MAC `30:ed:a0:ea:1a:5e`, v1.3, eFuse като платка 1)

- Заводският C6 и тук е без версия (0.0.0). Тестът преди OTA показва 0 фронта на GPIO6. OTA
  (`begin`/`write`/`end`, без `activate`) мина. C6 е 3.1.0, Wi-Fi работи, 30 s по-късно идва само **1** фронт.
- Причина: `gptimer.c` (`gptimer_default_isr`): „when alarm event happens, the alarm will be disabled
  automatically by hardware“. Драйверът го включва отново само в прекъсването си, а без callback
  прекъсване няма. Значи alarm-ът се задейства само веднъж.
- Поправка без процесор: alarm събитието на всеки таймер пуска и ETM задачата
  `GPTIMER_ETM_TASK_EN_ALARM` на същия таймер (отделен ETM канал; `esp_etm_channel_connect()` позволява
  едно събитие в няколко канала). Auto reload на брояча е хардуерен.
  **Това е и решението за точка 1.3 на картата.**

### Резултат: ETM → GPIO на C6, GPIO → ETM → capture на P4 (2026-11-12)

```
5 s: 39 intervals | ETM capture: mean 125003027.6 ns, sd 20.3 ns, min-max -28..47 (span 75 ns) | ISR esp_timer: sd 276 ns, span 2000 ns
since start: 730 intervals | ETM capture: mean 125003028.4 ns, sd 20.3 ns, min-max -28..47 (span 75 ns) | ISR esp_timer: sd 686 ns, span 19000 ns
```

| Как | sd на интервала | разлика между най-малкия и най-големия |
|---|---|---|
| C6 програмен импулс (0.6), P4 `esp_timer` в ISR | 1,6 µs | 15 µs |
| C6 програмен импулс, P4 ETM capture | 0,32 µs | 2,9 µs |
| **C6 ETM импулс, P4 ETM capture** | **17–26 ns** | **75 ns** (730 интервала, 91 s) |
| C6 ETM импулс, P4 `esp_timer` в ISR (за сравнение) | 0,16–3,5 µs | до 19 µs |

- Без изпуснати импулси: 39 интервала за всеки 5 s прозорец, 730 за 91 s.
- 75 ns = 3 тика по 25 ns: квантоването на двата 40 MHz таймера (C6 при фронта, P4 при capture).
- Средният интервал 125 003 028 ns срещу 125 000 000 по таймера на C6: кристалите се разминават с
  +24,2 ppm и се движат бавно (+3 ns за 90 s, вероятно от температурата). Това е ролята на линията от 3.4.
- Целта от картата (rms на линията C6 → P4 под 0,5 µs) има запас: самото засичане на фронта дава около 20 ns.

**Състояние:** платка 2: на P4 е `p4_etm`, на C6 е `c6_etm` (копроцесор 3.1.0 + ETM импулс от 30 s след старта).
Платка 1: C6 чака USB-TTL.

## 0.8 `esp_wifi_get_tsf_time()` на C6 (2026-11-12)

### Документация

`esp_wifi.h` (IDF v6.1): „In Station mode … if station is not connected or station doesn't receive at least one beacon
after connected, will return 0“, и „Enabling power save may cause the return value inaccurate, except WiFi modem sleep“.
Връща TSF в **µs** (`int64_t`), т.е. стъпката е 1 µs. Нищо не пише за контекста, в който може да се вика.

### Код (дизасемблиране на `libnet80211.a` / `libpp.a` за esp32c6, IDF v6.1)

- `esp_wifi_get_tsf_time` (`ieee80211_api.o`) вика `g_osi_funcs_p->_is_from_isr()` (отместване 28 в `wifi_osi_funcs_t`):
  - **от прекъсване:** веднага `wifi_get_tsf_time_process()` → `esp_wifi_get_mode()`, проверки на `g_ic` →
    `hal_get_tsf_time()` (`hal_mac.o`): вдига бит 0 (STA) или 1 в регистър `0x600ad014` (latch), чете
    `0x600ad024` (горни 32 бита) и `0x600ad020` (долни 32 бита), сваля бита. Без lock;
  - **от задача:** заделя 24-байтово съобщение и вика `ieee80211_ioctl()` → `wifi_api_lock()` (mutex) →
    `pp_post()` към Wi-Fi задачата → чака семафор. Затова **не може** да се вика в критична секция от задача.

### Проба `c6_tsf` (платка 2, C6 = копроцесор 3.1.0 + тест, резултатите към P4 през `peer_data`)

Включени: `CONFIG_ESP_HOSTED_CP_FEAT_PEER_DATA=y` (C6), `CONFIG_ESP_HOSTED_HOST_FEAT_PEER_DATA=y` (P4),
`CONFIG_GPTIMER_ISR_HANDLER_IN_IRAM=n` на C6. Прекъсването вика код от flash и не трябва да тръгва при изключен кеш
(OTA, NVS). Часовникът на C6 е GPTimer 40 MHz (`PLL_F80M`).

| Контекст | Продължителност на едно извикване | Работи ли |
|---|---|---|
| задача | 37,0 µs средно (36,9–48,6 µs), 200 извиквания | да, но през Wi-Fi задачата |
| прекъсване (GPTimer alarm), в `portENTER_CRITICAL_ISR` | **1193 такта = 7,46 µs**, понякога 373 такта = 2,3 µs (CPU 160 MHz) | да: 1840 проби, 0 нули, без срив |
| `gptimer_get_raw_count()` в прекъсване | 206 такта = 1,29 µs | да |

- Двойката „таймер → TSF → таймер“ в прекъсване: най-малка 8,75 µs (понякога 3,6 µs, когато TSF минава по бързия път).
  Двата вида двойки дават различно отместване (до около 5 µs). Във фаза 1 трябва да се ползва само единият вид или да се коригира.
- **TSF спрямо таймера на C6** (права линия по 80 проби = 10 s): наклон -25,7…-26,7 ppm (кристалът на AP-то срещу този
  на C6), остатък **4,9–7,2 µs rms**, най-много 9–14 µs (отделни до 34–39 µs, в първите 10 s до 91 µs).
  Това е разсейването на самия TSF на станцията (корекциите по beacon-ите и стъпката от 1 µs), не на сдвояването:
  rms е същото и когато всички двойки са 8,75 µs.

### Изводи за картата

- Отговор на 0.8: от задача 37 µs и през mutex на Wi-Fi API; от прекъсване 7,5 µs, без lock, и работи в критична секция
  (`portENTER_CRITICAL_ISR`). **Точка 1.4 трябва да сдвоява в прекъсване** (например в alarm-а на GPTimer между импулсите).
  „Критична секция в задача“, както пише в картата, не става.
- Бързият път зависи от недокументирано разклонение в затворената библиотека (`_is_from_isr`). При нова IDF трябва да се провери отново.
- Една проба TSF носи около 5 µs rms. Правата по 64 проби (8 s) дава грешка от порядъка на 5/√64 ≈ 0,6 µs. Целта
  „TSF ↔ местно време под 1 µs“ е постижима, но не с голям запас. Проверява се във фаза 3.

### Без недокументирания път: сравнение прекъсване ↔ задача (2026-11-12)

- Хардуерен път няма: `soc/esp32c6/include/soc/soc_etm_source.h` няма ETM събития от Wi-Fi/MAC/TSF
  (само ADC, GDMA, GPIO, I2S, LEDC, MCPWM, PCNT, PMU, RMT, SYSTIMER, TIMER, UART и др.).
- Проба `c6_tsf` v3: едновременно, на всеки 125 ms, (а) в прекъсване, както преди, и (б) от задача с висок
  приоритет (`configMAX_PRIORITIES - 3`), т.е. документираният път през Wi-Fi задачата. И двете правят 8 ×
  „таймер → TSF → таймер“ и пазят двойката с най-малка разлика. 13 прозореца по 10 s:

| Път | Най-малка разлика в двойката | Най-голяма от 8 | Наклон TSF/C6 | rms от правата | max |
|---|---|---|---|---|---|
| прекъсване (недокументиран бърз път) | 8,775 µs (понякога 3,65) | до 9,9 µs | -25,48…-25,90 ppm | 4,83–6,31 µs | 10,9–20,0 µs |
| **задача (документиран)** | **37,025 µs винаги** | до 82–574 µs | -25,47…-25,88 ppm | **4,87–6,28 µs** | 10,6–20,7 µs |

- В един и същи прозорец rms на двата пътя се различава с под 0,1 µs. Наклонът съвпада до 0,04 ppm.
  Грешката идва от самия TSF (около 5,5 µs), не от начина на четене.
- Най-малката двойка от задача е винаги точно 37,025 µs, т.е. пътят през Wi-Fi задачата е еднакъв при най-бързото
  извикване. Мястото на четенето в този прозорец е постоянно. То дава само постоянно отместване (до около ±18 µs
  спрямо средата на прозореца), еднакво за всички колони. При S3 Gateway-а днес е същото, защото там също се вика от задача.
- Цена: 8 извиквания по около 37 µs на всеки 125 ms, около 0,24 % от процесора на C6, плюс кратко натоварване на
  Wi-Fi задачата. Понякога едно извикване отнема до 0,57 ms. Най-бързото от 8 винаги е 37,025 µs, така че може и с по-малко опити.

**Решение (предложение):** точка 1.4 ползва **документирания път от задача** с най-малката от N двойки. Така няма
зависимост от недокументираното разклонение `_is_from_isr` в затворената библиотека. Пътят от прекъсване
не дава по-добра права.

## 0.9 Точки в копроцесора на ESP-Hosted за PTP (2026-11-12)

### Приемане (въздух → P4): публичен API, работи

`coprocessor/eh_cp_core/include/eh_cp.h` (esp_hosted 3.1.0):
```c
/* Traffic per interface. rx: from the host over the bus; tx: from the air (or the
 * CP) to the host. eb: the Wi-Fi RX buffer, NULL otherwise. NULL cb clears. */
esp_err_t eh_cp_set_traffic_tx_cb(eh_if_type_t if_type, eh_cp_traffic_cb_t cb);
eh_cp_traffic_cb_t eh_cp_get_traffic_tx_cb(eh_if_type_t if_type);
```
- Пътят: Wi-Fi драйвер → `sta_rx()` → `wifi_rx()` (`eh_cp_feat_wifi_event_publisher.c`), който **за всеки кадър**
  взима `eh_cp_get_traffic_tx_cb(ESP_STA_IF)`. По подразбиране това е `default_wlan_sta_rx_callback` (`eh_cp_core.c:970`)
  към host-а. Network split я сменя със своя (`eh_cp_feat_nw_split.c:1108`). Това е **общата точка**: наш callback,
  който чете таймера, разпознава PTP и вика предишния. Без промяна в esp_hosted.
- Кадърът там е Ethernet II (вече разкриптиран), IPv4/UDP/PTP се чете директно.
- Внимание за фаза 1: `eh_cp_deinit()` нулира callback-а (`eh_cp_core.c:1232`). Нашият трябва да се слага отново след ново init.

### Подаване (P4 → въздух): публичен API, работи

`eh_cp_set_traffic_rx_cb(ESP_STA_IF, cb)`: `host_rx()` (`eh_cp_core.c`) вика callback-а за всеки кадър от host-а.
`ESP_ERR_NOT_FOUND` („nobody claimed it“) пуска кадъра към въздуха (`wifi_tx_with_retry` → `esp_wifi_internal_tx`).

### Край на изпращането: `esp_wifi_set_tx_done_cb()` (частен header `esp_private/wifi.h`)

`typedef void (*wifi_tx_done_cb_t)(uint8_t ifidx, uint8_t *data, uint16_t *data_len, bool txStatus);`
Не се ползва никъде в IDF 6.1 и в esp_hosted 3.1.0, свободен е. Регистрира се (`ESP_OK`) и идва за всеки кадър от host-а
(20 633 подадени / 20 633 TX done).

**`data` е 802.11 кадърът, вече криптиран:** frame control `0x88` (QoS Data) с флаговете `0x41` / `0xc9` / `0xc1`
(ToDS + Protected). IP/UDP/PTP не може да се прочете. **Callback-ът не дава пряка връзка с PTP пакета** (точка 1.6 на картата).
- Свързване по ред (n-ият кадър от host-а ↔ n-ият TX done): броят съвпада, но не е доказано, че е същият кадър.
  Разликата в дължината не е постоянна (14 B само при около 13 % от кадрите). При различни WMM/QoS опашки редът може да се размести.

### Измервания (платка 2, P4 → Mac `Delay_Req` на 100 ms, Mac → P4 Sync/Announce multicast и Delay_Resp unicast на 100 ms, плюс около 2 Mbit/s UDP от P4)

| Път | Резултат |
|---|---|
| въздух → наш callback на C6 → сокет на P4 (`recvmsg` + `esp_timer`) | всички PTP пакети съвпадат по номер; разсейване (след правата) **sd 75–175 µs, до +1,46 ms** |
| P4 `sendto` → callback на C6 при подаване | sd 42–110 µs, до +1,16 ms (в началото до 115 ms, ARP) |
| C6: подаване → TX done (един часовник) | **мин. 264 µs, средно около 3 ms, макс. 63 ms**; 53 % под 0,5 ms, 7 % 0,5–1, 9 % 1–2, 10 % 2–4, 8 % 4–8, 9 % 8–16, 3 % 16–32, 1 % над 32 ms |

**Изводи:**
- Засичането при приемане на C6 премахва разсейване от порядъка на 0,1–1,5 ms по пътя до P4. Това е основната печалба за `apple_clock`.
- При изпращане засичане при подаването не помага: кадърът чака в C6 от 0,26 до 63 ms. Нужен е TX done, но той не казва кой е кадърът.
  **Трябва решение** (точка 1.6: „спира и пита“).

**Решение на Кристиан (2026-11-12, точка 1.6):** `Delay_Req` **не се засича на C6**. Изпращането остава засечено на P4 (`s.t3` в
`apple_clock`), както днес на S3. Засичането на C6 е само при приемане (точка 1.5). `esp_wifi_set_tx_done_cb` не се ползва.
Последици за картата: в 4.1–4.2 отпада частта „при изпращане на Delay_Req“ (отложеното изчисление на двойката и таймаутът).
В 4.4 `CONFIG_AIRPLAY_APPLE_CLOCK_ASYM_US` няма да е близо до 0, а трябва да се измери. В 1.7 PTP записите имат само `dir` = RX.

## 0.10 MAC на P4 (2026-11-12)

### В IDF v6.1 (`components/esp_hw_support/mac_addr.c`)

- `s_mac_table[]` съдържа `ESP_MAC_WIFI_STA` и `ESP_MAC_WIFI_SOFTAP` **само при `SOC_WIFI_SUPPORTED`**. На P4 са само
  `ETH`, `BASE`, `EFUSE_FACTORY`, `EFUSE_CUSTOM`. `esp_read_mac(…, ESP_MAC_WIFI_STA)` → `get_idx()` → „0 mac type is incorrect
  (not found)“ → `ESP_ERR_NOT_SUPPORTED`. `esp_iface_mac_addr_set(…, ESP_MAC_WIFI_STA)` също не може.
- При чипове с Wi-Fi `generate_mac()`: `case ESP_MAC_WIFI_STA: memcpy(mac, base_mac_addr, 6)`, т.е. **STA = BASE** (на S3 също).
- P4 конфигурация по подразбиране: `CONFIG_ESP32P4_UNIVERSAL_MAC_ADDRESSES_ONE=y`, `ESP_MAC_ADDR_UNIVERSE_ETH=y`.

### Мрежовият интерфейс на P4 (`components/esp_wifi/src/wifi_default.c`)

`wifi_start()` при `WIFI_EVENT_STA_START`: `esp_wifi_get_if_mac()` → `esp_wifi_get_mac()` (на P4 RPC към C6) →
`esp_netif_set_mac()`. Значи MAC-ът на интерфейса идва от C6 при старта. `esp_wifi_set_mac()` трябва да е **между
`esp_wifi_init()` и `esp_wifi_start()`**. `esp_wifi.h`: „This API can only be called when the interface is disabled“,
„do not set them to be the same“ (STA и AP), „The bit 0 of the first byte of MAC address can not be 1“.

### Проба `p4_mac` (платка 2)

```
esp_read_mac(WIFI_STA): ESP_ERR_NOT_SUPPORTED      (E system_api: 0 mac type is incorrect (not found))
esp_read_mac(WIFI_SOFTAP): ESP_ERR_NOT_SUPPORTED
esp_read_mac(BASE) = ETH = EFUSE_FACTORY: 30:ed:a0:ea:1a:5e
esp_derive_local_mac(BASE): 32:ed:a0:ea:1a:5e
C6 after init: STA 58:e6:c5:cc:21:48, AP 58:e6:c5:cc:21:49          (собствените MAC-ове на C6)
set_mac(STA, BASE) after init, before set_mode: ESP_OK  → C6 STA 30:ed:a0:ea:1a:5e
set_mac(AP, derived) after set_mode(APSTA): ESP_OK      → C6 AP 32:ed:a0:ea:1a:5e
STA_START: netif MAC 30:ed:a0:ea:1a:5e ; got ip ; netif MAC = P4 base MAC
```

- MAC-ът се задава на C6 при всяко пускане: C6 започва със своите след всеки рестарт (P4 го рестартира през GPIO54).
- DHCP минава с MAC-а на P4.

### Места в Gateway-а, които четат MAC с `ESP_MAC_WIFI_STA` (на P4 всичките връщат грешка)

| Място | Употреба |
|---|---|
| `main/airplay_identity.c:13` | `ap2_port_device_mac()` → deviceid (`rtsp_server.c`, `hap.c` в ap2_core) |
| `main/airplay_identity.c:21` | pairing id |
| `main/airplay_identity.c:47` | сериен номер |
| `components/wifi/src/wifi.c:649` | `wifi_get_mac_str()` |
| `components/wifi/src/wifi.c:799` | име по подразбиране (последните байтове) |
| `components/settings/src/settings.c:279` | (настройки, MAC) |
| `apple_clock.c:395` (esp-airplay2-core v1.0.0) | `apple_clock_start()`: при грешка `return ESP_FAIL`, т.е. **часовникът към Apple не тръгва** |

**Решение 6 от картата („нито едно от 7-те места не се пипа“) не е възможно.** Трябва решение (вижте отговора).
`speaker/main/spk_link.c:172` е само за колоните (S3) и не е засегнат.

**Решение на Кристиан (2026-11-12, решение 6):** в 7-те места `ESP_MAC_WIFI_STA` става **`ESP_MAC_BASE`**, без `#if`.
На S3 стойността е същата (IDF копира BASE в STA), на P4 е eFuse MAC-ът. Прави се във фаза 2 (2.3).
- В `esp-airplay2-core` (`apple_clock.c`) е нужна същата смяна и нов таг. Без нея `apple_clock_start()` на P4 връща `ESP_FAIL`
  още във фаза 2. Решава се при фаза 2: отделен `v1.0.1` или направо в `v1.1.0` от фаза 4.
- 2.3: `esp_wifi_set_mac(WIFI_IF_STA, BASE)` в `components/wifi` между `esp_wifi_init()` и `esp_wifi_start()` (само при
  `!SOC_WIFI_SUPPORTED`). За setup AP-то: `esp_wifi_set_mac(WIFI_IF_AP, esp_derive_local_mac(BASE))` (на пробата `32:ed:…`),
  защото P4 има само един универсален адрес, а STA и AP не бива да са еднакви.

## 0.11 Пинове (2026-11-12)

### Изведени на гнездата на DFR1172 (On-board Function Diagram и снимката с тестовите точки)

- лявата страна: VCC, 3V3, GND, **32, 33, 51, 23, 22, 21, 20, 36, 35, 34, 31**;
- дясната страна: RST, 3V3, NC, GND, NC, **28** (SCK), **29** (MO), **30** (MI), **8** (SCL), **7** (SDA), **48, 49, 50, 52, 4, 5,
  37** (T), **38** (R).
- Общо 24 GPIO (wiki: „Digital I/O x24“).

### Заети или с ограничения

| GPIO | Защо | Източник |
|---|---|---|
| 6 | WAKEUP (C6 GPIO2) | wiki, 0.6 |
| 14–19, 54 | SDIO към C6, reset на C6 | wiki, esp_hosted, 0.3 |
| 39–45 | TF карта; захранване на групата от VDDPST_5 (SD_VREF), напрежението зависи от платката | wiki, `sdmmc_host.rst` |
| 9, 12 | вграденият PDM микрофон (DATA, CLK) | wiki |
| 3 | вграденият LED (едноцветен, **не е WS2812**) | wiki, Getting Started |
| 35 | бутон BOOT, strapping | wiki, `gpio/esp32p4.inc` |
| 34, 36, 37, 38 | strapping (37/38 и UART0 на конзолата) | `gpio/esp32p4.inc`: „GPIO34, GPIO35, GPIO36, GPIO37, and GPIO38 are strapping pins“ |
| 24, 25 | USB-JTAG (не са изведени) | `gpio/esp32p4.inc` |
| 7, 8 | I2C на гнездото (SDA/SCL); свободни, ако няма I2C | wiki |
| 28, 29, 30 | означени SCK/MO/MI (SPI); свободни, ако няма SPI | платката |

**Свободни на гнездата:** 4, 5, 20, 21, 22, 23, 31, 32, 33, 48, 49, 50, 51, 52 (+ 7, 8, 28, 29, 30, ако не се ползват I2C/SPI).

### Какво ползва Gateway-ът сега (`components/airplay_options/Kconfig.projbuild`)

| Опция | S3 по подразбиране | Проблем на P4 |
|---|---|---|
| `AIRPLAY_SURROUND_CAL_MIC_SCK_GPIO` | 5 | няма |
| `AIRPLAY_SURROUND_CAL_MIC_WS_GPIO` | 6 | **6 е WAKEUP** |
| `AIRPLAY_SURROUND_CAL_MIC_SD_GPIO` | 4 | няма |
| `RGB_AUDIO_LED_GPIO` (WS2812) | 38 | **38 е strapping и UART0 RX**; вграденият LED на 3 не е WS2812 |
| `AIRPLAY_SURROUND_SYNC_PULSE_GPIO` (тест, по подразбиране изкл.) | 9 | **9 е вграденият микрофон** |
| `AIRPLAY_BUTTON_*_GPIO` | -1 (изкл.) | няма |
| `range 0 48` на всички | | P4 има GPIO до 54; `led.c` има и `#error` за > 48 |

`AIRPLAY_AMP_CONTROL` е изключен за Gateway-а (`sdkconfig.defaults.gateway`).

### Предложение за FireBeetle 2 ESP32-P4

| Сигнал | GPIO | Защо |
|---|---|---|
| INMP441 SCK | **5** | както на S3; LP домейн (3,3 V) |
| INMP441 SD | **4** | както на S3; LP домейн (3,3 V) |
| INMP441 WS | **52** | съседен на 4 и 5 на гнездото (… 50, 52, 4, 5); не е strapping; групата му включва GPIO54 (reset на C6, работи на 3,3 V) |
| WS2812 (външен) | **33** | извън strapping, USB и SD групата (39–48) |
| тестов импулс за синхрон | **50** | вместо 9 (микрофона) |

Трите пина на INMP441 са един до друг на дясното гнездо (52, 4, 5).
**Не е проверено:** схемата на DFRobot (PDF) и захранващите домейни в datasheet-а на P4 (PDF-ът не може да се
отвори тук без poppler). Затова се избягват 39–48. Пиновете се проверяват на хардуера с INMP441 във фаза 2 (2.7).
Решение за 2.1: дали за FireBeetle LED-ът е изключен по подразбиране, на външен WS2812 (33) или с поддръжка на
едноцветния LED на 3. Последното е нова функция.

---

## Край на фаза 0 (2026-11-12)

| Точка | Резултат |
|---|---|
| 0.1 | P4 v1.3 (`SELECTS_REV_LESS_V3`, 360 MHz); ESP-IDF **v6.1** (commit `d782e0c`); esp_hosted **3.1.0**; esp_wifi_remote вграден в IDF 6.1 |
| 0.2 | заводски C6: ESP-Hosted slave 0.0.6 (комп. 0.0.22), два OTA дяла по 1,5 MB; **OTA от P4 работи**; USB-TTL само за възстановяване |
| 0.3 | SDIO 4 бита, 40 MHz (таванът на P4); TCP 12–16 Mbit/s, около 1,5–2 × S3 на същото място |
| 0.4 | `components/wifi` на Gateway-а работи през C6 без промяна; setup AP + captive DNS (нужен PSRAM); TSF не се поддържа |
| 0.5 | mDNS и PTP multicast стигат до P4 |
| 0.6 | WAKEUP C6 GPIO2 → P4 GPIO6 работи; esp_hosted не ползва линията |
| 0.7 | **ETM в двата края: sd около 20 ns, разлика 75 ns**; alarm-ът се включва отново с ETM `EN_ALARM` |
| 0.8 | TSF от задача 37 µs (документиран път), без разлика в точността спрямо прекъсване; TSF около 5,5 µs rms на проба |
| 0.9 | PTP при приемане: публичен API на копроцесора (`eh_cp.h`), маха 0,1–1,5 ms разсейване; **Delay_Req остава на P4** (решение) |
| 0.10 | `ESP_MAC_WIFI_STA` го няма на P4 → **`ESP_MAC_BASE` в 7-те места** (решение), включително `apple_clock` в общия модул; `esp_wifi_set_mac` между init и start |
| 0.11 | предложение: INMP441 5/4/52, WS2812 33, sync pulse 50 |

**Промени в картата от фаза 0:** 1.3 (ETM `EN_ALARM`), 1.4 (TSF от задача), 1.6 и 4.1–4.2 (без TX засичане), 4.4 (ASYM се мери),
решение 6 (ESP_MAC_BASE и смяна в `esp-airplay2-core`), 1.1 и 5.2 (`app_desc` на C6 не стига до P4 с 3.1.0, нужен е друг път),
фаза 5 (P4 се рестартира след OTA на C6), firmware-ът на C6 никога не спира при грешка (урокът от платка 1).

**Платки:** платка 2 е с тестовите образи (`p4_mac` на P4, `c6_ptp` на C6, OTA работи). Платка 1: C6 чака USB-TTL.

---

## Фаза 1: firmware за C6

### 1.1 Основа (2026-11-12)

`coprocessor/c6/`: отделен IDF проект `airplay-gw-c6`, със собствен `VERSION` (`v0.1.0`) за `esp_app_desc_t`.
ESP-IDF v6.1, `espressif/esp_hosted` `==3.1.0` (същата като на P4), `dependencies.lock` в папката.
- Стандартният копроцесор (`CP_FOR_MCU`, `CP_RPC_V2`, `CP_FEAT_WIFI`, SDIO); `CONFIG_ESP_HOSTED_CP_FEAT_HOST_PS=n`; BT изключен.
- Таблицата на дяловете е като заводската (`ota_0`/`ota_1` по 1536 KB на `0x10000`/`0x190000`). Образът е 1 150 288 байта (27 % свободно).
- `main.c`: само NVS и event loop; **никога не спира** (без `ESP_ERROR_CHECK`), заради урока от 0.7.

Проверка на платка 2: OTA от P4 (`begin`/`write`/`end`/`activate`), C6 се рестартира, `host=3.1.0 coprocessor=3.1.0 (match)`,
`capabilities: 0x01` (само WLAN over SDIO), STA взима IP за около 1,8 s. S3 Gateway-ът се компилира както преди (`0x14ee60`, 0 предупреждения).
Името `airplay-gw-c6` и версията не могат да се прочетат от P4 през esp_hosted 3.1.0 (`app_desc` се попълва на host-а, 0.2).
Как P4 ще разпознава нашия firmware, се решава в 1.7 (съобщение през `peer_data`) и 5.2.

### 1.2 Часовник на C6 (2026-11-12)

`coprocessor/c6/main/c6_clock.{h,c}`: един GPTimer, пуснат в `app_main()`, брои нагоре и никога не се презарежда.
`GPTIMER_CLK_SRC_PLL_F80M` (PLL-ът е заключен към кристала на C6), `resolution_hz` = 40 MHz (25 ns), брояч от 54 бита
(`TIMER_LL_COUNTER_BIT_WIDTH 54`, около 14 години). API: `c6_clock_start()` (при грешка само лог), `c6_clock_now()`
(0, ако не е пуснат), `c6_clock_timer()` (за ETM в 1.3). С `XTAL` резолюцията би била най-много 20 MHz.

Проверка на платка 2 (тестов отчет през `peer_data` само в копие в scratchpad):
`c6_ticks` срещу `esp_timer_get_time()` на C6 за 60 s: разликата е постоянна, +2…+3 µs (редът на четенията), т.е. 0 ppm
(един кристал). 6000 поредни четения през 10 ms без нито едно връщане назад. Образът е 1 157 024 байта.
S3 Gateway-ът се компилира както преди.

### 1.3 Импулс (2026-11-12)

**Ограничение:** C6 има само два GPTimer-а (`soc_etm_source.h`: `TIMER0_EVT_CNT_CMP_TIMER0`, `TIMER1_EVT_CNT_CMP_TIMER0`, по един в
група). Единият е часовникът от 1.2, който не се презарежда. Импулс от 20 µs с два alarm-а на отделни таймери не може да се направи.
**Решение на Кристиан:** правоъгълен сигнал 8 Hz.

`coprocessor/c6/main/c6_pulse.{h,c}`, пин `CONFIG_GW_C6_WAKEUP_GPIO` (`main/Kconfig.projbuild`, по подразбиране 2), период
`SURROUND_TIME_INTERVAL_MS` от `common/surround/surround_proto.h` (същата константа като в Gateway-а):
- вторият GPTimer (`PLL_F80M`, 40 MHz), alarm на всеки половин период (62,5 ms), auto reload:
  ETM → `GPIO_ETM_TASK_ACTION_TOG` на GPIO2 и ETM → `GPTIMER_ETM_TASK_EN_ALARM` на същия таймер;
- GPIO2 е `GPIO_MODE_INPUT_OUTPUT`: нарастващият фронт на самия изход → `gpio_new_etm_event(POS)` → ETM →
  `GPTIMER_ETM_TASK_CAPTURE` на часовника. **`c6_ticks` при всеки нарастващ фронт се записва от хардуера.**
- Процесорът не участва нито в импулса, нито в засичането. `c6_pulse_last_edge()` само чете записаната стойност
  (`gptimer_get_captured_count()`) и номера на фронта.
- Нарастващ фронт на всеки 125 ms, високо ниво 62,5 ms (вместо 20 µs).

**Периодът по часовника на C6 е 5 000 004 тика, а не 5 000 000:** auto reload добавя по 2 тика на всеки alarm. Стойността е
постоянна (во всеки интервал). Затова номерът на фронта се смята по разликата от предишния, не от старта (иначе би
сбъркал с едно след около 22 h).

Проверка на платка 2 (OTA на C6, тестов отчет през `peer_data` само в копие в scratchpad; P4 = GPIO6 POS → ETM → capture):
- C6: 360 фронта, `seq` без пропуски, всеки интервал точно 5 000 004 тика;
- P4: 39 интервала за всеки 5 s прозорец, **sd 7–13 ns, разлика 25 ns** (1 тик); за 60 s sd 22 ns, разлика 75 ns (бавен дрейф
  на кристалите); среден интервал 125 003 1xx ns (+24,8 ppm, кристалите + 0,8 ppm от презареждането).
- C6 образ 1 171 536 байта (26 % свободно). S3 Gateway-ът се компилира както преди.

**Поправка на 1.3, намерена в 1.4:** `gptimer_get_raw_count()` = `timer_hal_capture_and_get_counter_value()` →
`timer_ll_trigger_soft_capture()` (бит `tx_update`) + четене на `hw_timer[].lo/hi`. `gptimer_get_captured_count()` чете
**същите** `lo/hi`, в които пише и ETM `CAPTURE` (`esp_hal_timg/esp32c6/include/hal/timer_ll.h`, `gptimer.c`). Всяко софтуерно
четене на часовника презаписва стойността от фронта. С TSF сдвояването (16 четения на 125 ms) веднъж за 10 s се получи фронт,
сбъркан с около 60 ms. Сега `c6_pulse` хваща само **фронтове 0, 1, 2 при старта** (преди някой друг да чете часовника) и
проверява, че двата интервала са еднакви. После маха ETM канала за capture и смята фронт n = `edge0 + n · period`. И двата
таймера са от един PLL, а периодът е постоянен в хардуера (5 000 004 тика), така че стойността при фронта е точно
изчислима, както иска картата („стойността на таймера при фронта е точно стойността на alarm-а“). `c6_pulse_start()` отнема
около 320 ms и трябва да е преди всяко друго четене на часовника.

### 1.4 TSF ↔ часовник на C6 (2026-11-12)

`coprocessor/c6/main/c6_tsf.{h,c}`:
- задача (`configMAX_PRIORITIES - 3`) на всеки `SURROUND_TIME_INTERVAL_MS`: 8 × (`c6_clock_now()`, `esp_wifi_get_tsf_time(WIFI_IF_STA)`,
  `c6_clock_now()`) по **документирания път от задача** (решението от 0.8). Пази се двойката с най-малка разлика;
- изхвърля се, ако TSF = 0 (няма връзка или beacon) или ако разликата е > най-малката от последните 80 (10 s) + 4 µs.
  Прагът се нагажда сам, не зависи от точните 37 µs;
- права (най-малки квадрати) по последните 16 двойки (2 s), с повторно пресмятане без двойките, отдалечени на > 30 µs.
  Сумите са спрямо най-новата двойка, както часовника, така и TSF: TSF в ns е около 1e15 и в `double` би загубил точност;
- `c6_tsf_at(c6_ticks)` → TSF (ns), най-малката разлика (качество) и флагове `CONNECTED`, `VALID`, `RESTART`;
- `RESTART` (правата започва отначало): при всяко `WIFI_EVENT_STA_CONNECTED` (нов BSSID или ново свързване),
  `STA_DISCONNECTED`, и при 3 поредни двойки на > 500 µs от правата (TSF е скочил). Отделни такива се броят като `outliers`;
- mutex, не критична секция: C6 няма FPU, а правата се смята в `double` за десетки µs. Всички извикващи са задачи;
- броячи за 1.8: двойки, нули, изхвърлени по разлика, outliers, рестарти.

Проверка на платка 2 (P4 свързан към AP; тестов отчет през `peer_data` само в копие в scratchpad): TSF при всеки фронт от `c6_tsf_at()`,
права по 80 фронта (10 s), 16 прозореца, около 165 s:

| | |
|---|---|
| TSF/фронт | 124 996 836…124 996 899 ns (-24,8…-25,3 ppm спрямо 125 ms: AP-то спрямо C6) |
| rms от правата по номера на фронта | **2,0–2,4 µs** (първият прозорец с рестарта: 3,1 µs) |
| max | 4,5–6,2 µs |
| най-малка разлика при сдвояването | 35,25 µs |
| изхвърлени | 22 нули (преди връзката), 0 по разлика, 0 outliers; 1 рестарт (свързването) |

Сравнение с една двойка (0.8): около 5,5 µs rms. Правата по 16 двойки смъква грешката при фронта до около 2 µs. Правата на P4 (`tsf_fit`, 64 проби) ще я смъкне още.
C6 образ 1 175 056 байта (25 % свободно). S3 Gateway-ът се компилира както преди.

### 1.5 PTP при приемане (2026-11-12)

`coprocessor/c6/main/c6_ptp.{h,c}`:
- `air_rx()` (IRAM) е пред callback-а на ESP-Hosted за кадрите от въздуха към host-а (`eh_cp_set_traffic_tx_cb(ESP_STA_IF)`, публичен API).
  Първо чете часовника (`c6_clock_now()`, IRAM; `CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM=y`), после само за Ethernet II / IPv4 (без фрагменти) /
  UDP към 319 или 320 / PTPv2 взима `messageType`, `sequenceId`, `sourcePortIdentity` (10 B). Записът отива в кръгов буфер (32 записа,
  `portENTER_CRITICAL_SAFE`). **Кадърът винаги продължава към P4 непроменен** (вика се предишният callback).
- Наблюдател (задача, 1 s): ако callback-ът на ESP-Hosted отново е стандартният (ново init след `eh_cp_deinit`), слага `air_rx` отново
  пред него. Чужд callback (например Network split) не се обвива, защото може да вика предишния и да се получи безкраен цикъл. Само се записва предупреждение.
- `c6_ptp_take()` за 1.7; броячи: кадри, записи, изпуснати (буферът пълен).

Проверка на платка 2 (Mac: Sync multicast 319, Announce multicast 320, Delay_Resp unicast 320, по 600 на 100 ms; P4 = сокетите
на `apple_clock` + `Delay_Req` и около 2 Mbit/s натоварване; тестов отчет само в копие в scratchpad):
- 1783 записа на C6, **0 изпуснати**. Всеки PTP пакет, получен от P4 (752 за 10 s), има запис на C6 със същия тип и номер.
  `sourcePortIdentity` е прочетен правилно (`112233fffe445566-0001`). Пакетите стигат до P4 и сокетите на P4 ги четат както преди.
- Разсейване от callback-а на C6 до сокета на P4 (след правата): **sd 79–102 µs, до +1,04 ms**. Това е разсейването, което
  засичането на C6 маха.
- Времето, което callback-ът добавя към всеки кадър, не е мерено поотделно.

C6 образ 1 176 000 байта (25 % свободно). S3 Gateway-ът се компилира както преди.

### 1.6 PTP при изпращане (2026-11-12)

Не се прави код. Решено в 0.9: `esp_wifi_set_tx_done_cb` дава вече шифрования 802.11 кадър, затова не може да се свърже с
PTP съобщение. По решение на Кристиан `Delay_Req` остава засечен на P4 (`s.t3` в `apple_clock`), както днес на S3. На C6 се
засича само приемането (1.5). Решението е записано в `coprocessor/c6/main/c6_ptp.h`.

Последствия за картата: във фаза 4 (4.1–4.2) няма запис „време на излизане“ и няма отлагане на двойката t3/t4 с таймаут.
Асиметрията от различните точки на засичане (C6 при приемане, P4 при изпращане) се мери в 4.4.

Компилиране: C6 `0x11f1c0`, S3 Gateway `0x14ee60`, без предупреждения (непроменени).

### 1.7 Съобщения към P4 (2026-11-12)

Формат: `common/gw_c6/gw_c6_proto.h` (общ за C6 и за Gateway-а на P4 във фаза 3). `msg_id` на peer data е типът:
`HELLO` 0x47430001, `TICK` 0x47430002, `PTP` 0x47430003 (C6 → P4), `HELLO_REQ` 0x47430081 (P4 → C6). Всеки payload почва с
`proto` (= 1). `gw_c6_tick_t` е точно структурата от картата (25 B + записи), записът е 22 B, `dir` е само RX (1.6).
`HELLO` носи името, версията и IDF от `esp_app_desc` (решава въпроса от 1.1: P4 не може да ги вземе през `app_desc`),
кои части работят (`features`), честотата на часовника и измерения период.

Проверено в esp_hosted 3.1.0:
- C6: `eh_cp_feat_peer_data_send()` → `eh_cp_rpc_send_event(RPC_ID__Event_CustomRpc)`, до 16 KiB; нужно е
  `CONFIG_ESP_HOSTED_CP_FEAT_PEER_DATA=y` (добавено в `sdkconfig.defaults`). Callback-ът за входящи съобщения е в RPC
  контекста, затова само събужда задачата.
- P4: събитието се копира (`malloc`) и се праща през `esp_event_post(..., 0)` в default event loop-а; callback-ът е там.
  Ако опашката е пълна, съобщението се губи (и копието не се освобождава: бъг в esp_hosted, при 25 съобщения/s не се стига).

`coprocessor/c6/main/c6_link.{h,c}`: една задача (`configMAX_PRIORITIES - 4`) праща всичко, по реда на събитията:
- `HELLO` веднъж преди първия `TICK` и след всеки `HELLO_REQ`;
- `TICK` около 1 ms след всеки фронт (спи до следващия), с TSF от линията от 1.4 и чакащите PTP записи;
- `PTP` веднага: `c6_ptp` събужда задачата при всеки запис (`c6_ptp_set_notify`). **Решение:** записите не чакат импулса.

**Поправка на 1.3, намерена тук.** Засичането на фронта на C6 трепти с ±1 тик (GPIO входът се синхронизира с друг такт):
интервалите са P−1, P, P+1, сборът се запазва. Старото калибриране (2 интервала, трябва да са равни) понякога изключваше
импулса (видяно: `period 0` в 1 от 18 рестарта) или можеше да вземе грешен период (грешката расте с времето). Сега:
9 фронта, период = закръгленото средно, всеки интервал в ±2 тика, `edge0` = средното на `e[i] − i·P`, до 3 опита (~1,2 s).
Проверено с тестов образ, който пази засичането включено: при 11 рестарта и 3 минути (1400 фронта) отклонението на всеки
истински фронт от `edge0 + n·P` е 0 или +1 тик, без натрупване. Периодът е цяло число тикове, но различен при различните
рестарти: **5 000 006** (повечето) или **5 000 004**; P4 мери интервал с 48 ns по-къс при 5 000 004, значи разликата е истинска.

Тест на платка 2 (`p4_test17`, P4 засича фронта с ETM, Mac праща PTP, фоново натоварване):
- `HELLO`: `airplay-gw-c6` / `v0.1.0` / `v6.1`, features 0x0f. Първото `HELLO` на C6 понякога се губи (пратено преди P4
  да е готов) → **Gateway-ът трябва да пита с `HELLO_REQ`**; отговорът идва за ~6 ms.
- `TICK`: 8/s, без пропуски, стъпката на `edge_c6_ticks` е точно n·P. Фронтът на C6 спрямо засичането на P4: sd 8–40 ns
  за 10 s. За минути наклонът се мени (+25,1 → +25,7 ppm: кристалите на двете платки загряват; P4 мери това и сам).
  `TICK` идва 2–4 ms след фронта (рядко до 80 ms).
- TSF при фронта спрямо C6: sd ~2,2 µs, наклон около −25 ppm, като в 1.4.
- PTP: 1600+ пакета, всички с запис, `sourcePortIdentity` верен; 98 % в `PTP`, останалите в `TICK`. Записът идва
  средно 1,1 ms **след** пакета (от −0,8 до +4 ms) → във фаза 4 `apple_clock` чака записа няколко ms.

**Отворено: закъсняло и изгубено съобщение.** В 5 от 35 рестарта без трафик и във всичките 8 рестарта с ping от Mac-а
(2/s): за десетки поредни `TICK` всеки идва ~2 ms след **следващия** фронт (цял период късно), после един `TICK` липсва и
всичко се оправя. Прилича на съобщение, което стои в SDIO пътя, докато следващото не го избута. В теста с PTP и фоново
натоварване не се появи. Ping RTT през това време: медиана 12–21 ms, 5–14 от 50 над 50 ms, до 973 ms (няма сравнение със
S3 при същите условия). Причината (C6 или host частта на esp_hosted) не е намерена. Последствие за фаза 3: P4 трябва да свързва `TICK` със своето засичане по `seq` и
предвиждане от правата, а не по времето на пристигане.

Компилиране: C6 `0x120b70`, без предупреждения. S3 Gateway `0x14ee60`, без предупреждения (непроменен).

## Фаза 2: P4 в Gateway-а

### 2.0 Компилиране според платката (2026-11-12)

Проверено в ESP-IDF v6.1:
- `tools/cmake/targets.cmake`: `CONFIG_IDF_TARGET` от `sdkconfig.defaults` се ползва само ако target не е зададен.
  Затова `"esp32s3"` там остава и обикновеното `idf.py build` си е S3, а `idf.py set-target esp32p4` работи.
- `tools/cmake/kconfig.cmake`: за всеки файл в `SDKCONFIG_DEFAULTS` IDF добавя `<файл>.<target>` веднага след него.
  Редът за P4: `sdkconfig.defaults` → `sdkconfig.defaults.esp32p4` → `sdkconfig.defaults.gateway` →
  `sdkconfig.defaults.gateway.esp32p4`. Колоните (`speaker/`) са S3 и не четат файловете за P4.
- `docs/en/api-guides/tools/idf-component-manager.rst`: lock файл по target с
  `idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock.${IDF_TARGET})` преди `project()`.

Промени:
1. `sdkconfig.defaults.esp32p4` (нов): ревизия v1.x (`SELECTS_REV_LESS_V3`, `REV_MIN_100`), 360 MHz, 16 MB flash,
   `partitions_esp32p4.csv`, PSRAM 200 MHz (общият файл иска 80 MHz за S3). ESP-Hosted и Wi-Fi идват в 2.2.
2. `partitions_esp32p4.csv` (нов): като `partitions.csv` до `ota_0`, после `ota_0`/`ota_1` по 6 MB и `storage` 3,9 MB
   (образът на колона е 1,82 MB, а на S3 `storage` е 1,9 MB).
3. `CMakeLists.txt`: lock файл `dependencies.lock.<target>` за всеки чип освен S3, който си остава с `dependencies.lock`.
   `main/idf_component.yml`: `espressif/esp_hosted ==3.1.0` само за P4. С v6.1 `esp_wifi_remote` е вграден в IDF.
   **Намерено:** `espressif/esp_audio_codec` 2.6.2 (манифестът иска `^2.5.0`) спира build-а на P4 с ревизия под v3.0
   (`esp_audio_codec_p4_rev_check` в неговия `CMakeLists.txt`). Gateway-ът не го ползва: нито един файл не го включва,
   нито един компонент не го изисква (декодерите са на колоните). Затова е изключен само за P4 (`target != esp32p4`).
5. Правилото важи за следващите точки: зависимото от платката е зад `SOC_*`, Kconfig или избраната платка.
6. `coprocessor/c6/prebuilt/airplay-gw-c6_v0.1.0.bin` и `coprocessor/c6/update_prebuilt.sh` (компилира C6 и слага образа
   с версията в името). `main/CMakeLists.txt` го вгражда при `CONFIG_ESP_HOSTED_HOST` (символи `_binary_gw_c6_fw_bin_start/_end`).
   Докато никой не го вика (фаза 5), linker-ът го маха от крайния образ.
   Точка 4 (платка в Kconfig) е 2.1.

Резултат:
- **S3 Gateway: байт по байт същият образ** (`0x14ee60`) и същият `sdkconfig`. В `dependencies.lock` се сменя само
  `manifest_hash`. Колоната се компилира (`0x1bc760`).
- **P4 Gateway (`-B build_p4 -D SDKCONFIG=sdkconfig.p4`) се компилира:** `0x12c110` (80 % свободно), едно предупреждение
  в самия esp_hosted (`start_req` не се ползва). `CONFIG_ESP_HOSTED_HOST=y` по подразбиране.
- **Внимание:** bootloader-ът на P4 е `0x5eb0` от `0x6000` (остават 0x150 байта; на S3 остават 31 %). Всяка настройка,
  която го уголеми, ще го счупи. Тогава трябва да се мести таблицата на дяловете.

### 2.1 Платка в Kconfig (2026-11-12)

`components/airplay_options/Kconfig.projbuild` (общ за Gateway-а и колоните; краищата на редовете са запазени, файлът е
със смесени CRLF/LF):
- `choice AIRPLAY_GATEWAY_BOARD` (само за Gateway-а): `AIRPLAY_GW_BOARD_S3_DEVKIT` (само при esp32s3),
  `AIRPLAY_GW_BOARD_FIREBEETLE_P4` (само при esp32p4), `AIRPLAY_GW_BOARD_CUSTOM`. По подразбиране платката на чипа.
- `AIRPLAY_GW_TIME_PULSE_GPIO` (само Gateway на P4, по подразбиране 6): пинът на P4 за импулса от C6 (решение 4).
  Пинът на C6 е в firmware-а на C6 (`GW_C6_WAKEUP_GPIO`, 2). Ползва се във фаза 3.
- Обхватът на всички GPIO опции е по чипа: `range 0 SOC_GPIO_OUT_RANGE_MAX` за изходи, `SOC_GPIO_IN_RANGE_MAX` за входове
  (микрофон SD, бутоните, импулсът от C6). Проверено в `soc/<чип>/include/soc/Kconfig.soc_caps.in`: 48 на S3, 54 на P4.
  `components/led/src/led.c`: `#error` за пин > 48 → > `SOC_GPIO_OUT_RANGE_MAX`.
- Стойности по подразбиране на P4 (от 0.11): микрофон WS **52** (6 е импулсът), SCK 5, SD 4; тестов импулс **50**
  (9 е вграденият микрофон); WS2812 на **33**.

**Решение на Кристиан (2026-11-12, LED на FireBeetle):** едноцветният LED на платката (GPIO3). Това е нова функция
(само състояние, без VU метър) и ще се направи отделно, след като Gateway-ът тръгне на P4. Дотогава на FireBeetle WS2812 е
**изключен** по подразбиране (`default n if AIRPLAY_GW_BOARD_FIREBEETLE_P4`). Може да се включи на 33 в menuconfig.

Резултат:
- S3 Gateway и колона: в образите се различават само датата и часът на компилиране (`__DATE__`/`__TIME__`, `esp_app_desc`)
  и SHA на ELF. Кодът е същият. В `sdkconfig` на S3 Gateway-а се появява само `CONFIG_AIRPLAY_GW_BOARD_S3_DEVKIT=y`,
  а `sdkconfig` на колоната е същият.
- P4: `CONFIG_AIRPLAY_GW_BOARD_FIREBEETLE_P4=y`, микрофон 5/52/4, импулс 6, LED изключен; образ `0x124ea0`.
- **Внимание:** съществуващ `sdkconfig` пази старите пинове (новите стойности по подразбиране не се прилагат върху вече
  записани). При смяна на target `idf.py set-target` го прави наново, така че за P4 е наред.

### 2.2 Конфигурация на P4: ESP-Hosted и Wi-Fi (2026-11-12)

`sdkconfig.defaults.esp32p4` получава:
- **ESP-Hosted host по SDIO:** `ESP_HOSTED_HOST`, `HOST_TRANSPORT_BUS_SDIO`, 4 бита, `FEAT_RPC`, `RPC_EXT_V2`, `SYSTEM`, `WIFI`.
  `ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD` (в `host/Kconfig.ext` това е стойността по подразбиране за P4 + C6) фиксира пиновете
  на ESP32-P4-Function-EV-Board: CLK 18, CMD 19, D0–D3 14–17, reset 54. FireBeetle 2 е свързана по същия начин (0.3).
  За Custom платка: „No specific PCB board“ в менюто на ESP-Hosted и свои пинове. `SDIO_CLK_KHZ` остава 40000 (таванът, 0.3).
- **Енергоспестяването на host-а:** `ESP_HOSTED_HOST_FEAT_POWER_SAVE=n` изрично (решения 4 и 5; и без това е `default n`).
- **Wi-Fi буфери:** на P4 `CONFIG_ESP_WIFI_*` нямат prompt и само копират `WIFI_RMT_*`
  (`components/esp_wifi/remote/Kconfig.wifi_is_remote.in`), затова редовете `ESP_WIFI_*` от общия файл и от
  `sdkconfig.defaults.gateway` не правят нищо. Задават се `WIFI_RMT_*`, които отиват на C6 с `esp_wifi_init()` (0.3).
  Взети са стойностите на Espressif за P4 + C6 (static RX 16, dynamic RX 64, BA TX/RX 32), с които изпращането беше най-бързо
  в 0.3 (15,1–15,8 Mbit/s). TX буферите са статични: при `SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` (общия файл) IDF не позволява
  динамични (`Kconfig.wifi.in`: `depends on !(SPIRAM_TRY_ALLOCATE_WIFI_LWIP && ...)`). 32 броя, колкото S3 Gateway-ът.
  **Тази комбинация не е мерена** (в 0.3 TX буферите бяха динамични, 64). Мери се в 2.7.
- `espressif/esp_wifi_remote` не се добавя: в IDF v6.1 е вграден (0.1). lwIP остава като в общия файл (32768/65535, mbox 64).
  Стойностите на Espressif (65534/65534, `TCP_SACK_OUT`) са кандидат, ако в 2.7 изпращането не стига.

Резултат: P4 build-ът от нулева конфигурация (`set-target`) взема всичко (проверено в `sdkconfig.p4`), образ `0x124eb0`,
едно предупреждение в esp_hosted. Непознати за P4 са само три S3 реда в `sdkconfig.defaults`
(`ESP32S3_INSTRUCTION_CACHE_32KB`, `_LINE_32B`, `ESP_DEFAULT_CPU_FREQ_MHZ_240`), безвредни. S3 Gateway: `0x14ee60`, `sdkconfig`
без промяна освен `AIRPLAY_GW_BOARD_S3_DEVKIT` от 2.1.

**Още не се прошива на P4:** без 2.3 `esp_read_mac(ESP_MAC_WIFI_STA)` връща грешка (0.4, 0.10), а идентичността на
HomeKit и deviceid идват от MAC-а и се пазят в NVS.

### 2.3 MAC (2026-11-12)

По решението от 0.10:
- `components/wifi/src/wifi.c`, само при `!SOC_WIFI_SUPPORTED` (радиото е копроцесор):
  `esp_wifi_set_mac(WIFI_IF_STA, ESP_MAC_BASE)` веднага след `esp_wifi_init()` (преди `esp_wifi_start()`, който дава MAC-а на
  интерфейса), после `esp_wifi_get_mac()` от C6 и сравнение. Различие или грешка → `ESP_LOGE`. AP:
  `esp_derive_local_mac(BASE)` след `set_mode(APSTA)`, със същата проверка. Лог: `station MAC 30:ed:a0:ea:1a:5e (this chip's)`.
- `ESP_MAC_WIFI_STA` → `ESP_MAC_BASE` без `#if`: `main/airplay_identity.c` (3), `components/wifi/src/wifi.c` (2),
  `components/settings/src/settings.c` (1).
- **`esp-airplay2-core` v1.0.1** (решение на Кристиан, 2026-11-12: отделен таг сега): commit `c42f516` „apple_clock: PTP clock
  identity from ESP_MAC_BASE“ (един ред + README), таг `v1.0.1` (`428aae7`), push в GitHub. Gateway-ът минава на `v1.0.1` за
  `ap2_core` и `apple_clock`. Колоните остават на `v1.0.0` (не ползват MAC-а на `apple_clock` и не са засегнати).
  CI на модула не е проверен оттук (`gh` не е влязъл).

**Намерено при първото пускане на P4 (платка 2, flash изтрит, без запазена мрежа):** рестарт в цикъл.
`apple_clock_start()` → `PTP port 319 unavailable: errno 125` (`EADDRNOTAVAIL`: `IP_ADD_MEMBERSHIP` без интерфейс с адрес) →
`ESP_ERROR_CHECK` в `main.c` → `abort()`. В режим за настройка `main.c` не чака Wi-Fi, а точката за достъп на C6 тръгва по
RPC около 60 ms по-късно. **Поправка в `main.c`:** `apple_clock_start_when_ready()` опитва до 10 s на 250 ms (неуспешният опит
освобождава всичко), после `ESP_ERROR_CHECK` както преди. След нея: `access point on` (3442 ms), `apple_clock` от втория опит
(`identity 30eda0fffeea1a5e`, т.е. от MAC-а на P4), `captive DNS on`, режимът за настройка работи; 60 s без рестарт.

**Внимание за lock файловете (намерено тук):**
- component manager-ът не преизчислява git зависимост при съществуващ lock, дори ако версията в манифеста е сменена.
- `idf.py update-dependencies` трие **`dependencies.lock`** (по подразбиране) и при `-B build_p4`, т.е. трие lock-а на S3.
  Пресмятането наново вдигна и `espressif/led_strip` 3.0.3 → 3.1.1 на S3. Затова S3 lock-ът е върнат от git и в него са
  сменени само `ap2_core`/`apple_clock` и `manifest_hash`. P4 lock-ът е създаден наново (`led_strip` 3.1.1 там).
- Начин за смяна на git таг: смяна в манифеста, изтриване само на lock-а на P4 + `reconfigure`; на S3 ръчна смяна на двата записа.

Компилиране: S3 Gateway `0x14ee90` (различен от преди: 7 места с друга константа и `main.c`), P4 `0x1251d0`, колона без промяна.
**На S3 не е проверено на хардуер**, че името, deviceid и HomeKit идентичността са същите (по кода на IDF са, 0.10).

**Тест на Кристиан (2026-11-12, платка 2, P4):** настройка през `ESP32-AirPlay-Setup` → рестарт → `UniFi` (IP 192.168.1.169,
RSSI -39…-60), mDNS „ESP32 AirPlay A5E“, `apple_clock` идентичност `30eda0fffeea1a5e`. **Три колони се свързват** към P4
(FL, 240, 484, fw v4.5.68). Видяно в лога: AirPlay сесия, `apple_clock ready` (PTP от Apple през C6: `Delay_Req 40 answered 40`,
floor RTT 3,69 ms), `gw_relay` получава звука (4578 kbit/s) и го праща на трите колони. Колоните не свирят: няма `TIME`.

**Намерено:** `W esp_wifi_remote_weak: esp_wifi_remote_get_tsf_time unsupported` на всеки 125 ms (`time_tick()` →
`tsf_fit_read_sample()` → `esp_wifi_get_tsf_time()`; ESP-Hosted не носи TSF, 0.4). **Поправка в `main/gw_speakers.c`:** при
`!SOC_WIFI_SUPPORTED` `time_tick()` не чете TSF и казва веднъж `no TSF on this board yet: no TIME to the speakers, so they
connect but do not play`. `tsf_fit` не е пипан. Уеб страницата вече показва това за всяка колона (`no sender time`,
`speakers.html`). Във фаза 3 тук идва TSF-ът от C6. S3: `0x14ee90` (кодът е зад `#if`).

**Тест на Кристиан (2026-11-12, S3 Gateway с промените от 2.0–2.3):** работи. Името, Home и колоните са както преди,
т.е. `ESP_MAC_BASE` дава на S3 същата идентичност като `ESP_MAC_WIFI_STA`.

### 2.4 OTA (2026-11-12)

- **Собственото OTA (`components/ota`):** `ota_image_check_head()` получава очаквания чип като параметър (`ota_image.c`
  остава без ESP-IDF headers); `ota.c` подава `CONFIG_IDF_FIRMWARE_CHIP_ID` (S3 0x0009, P4 0x0012; проверено в двата
  `sdkconfig`). Махнати са `OTA_IMAGE_CHIP_ID_ESP32S3` и неговият `_Static_assert`. Текстът на грешката: „not an application
  image for this chip“. README: таблицата и пример 5. Колоните ползват същия компонент и проверяват своя чип (S3), както преди.
- **Образите за колоните (`main/gw_fw.c`)** се проверяват отделно (име на проекта `airplay-speaker`, магически числа) и не
  ползват `components/ota`. Не са пипани. Двете проверки са отделни, както иска картата.

Тест на платка 2 (P4 Gateway, `POST /api/ota/update`):
| Образ | Резултат |
|---|---|
| S3 Gateway `0x14ee90` | `ESP_ERR_IMAGE_INVALID` след 1440 байта, „not an application image for this chip“, нищо не е записано |
| колона (S3) | същото |
| P4 Gateway | 1 197 840 байта в `ota_1` (0x620000), 10 %…90 %, `ota_1 boots next`, рестарт, `Surround Gateway v4.5.68 ready` |

**Не е тествано:** качване на образ за колона в `storage` на P4 (`/api/speakers/firmware`). Gateway-ът сам разпраща образа
на свързаните колони с друг build, така че тестът би обновил колоните на Кристиан. Кодът (`gw_fw.c`) не е променян, а
`storage` на P4 е 3,9 MB (`gw_fw: ... available 3968 KiB` в лога на 2.3).

Компилиране: S3 Gateway `0x14ee90`, колона `0x1bc770`, P4 `0x124710`; нови предупреждения няма.

### 2.5 Код, който предполага S3 (2026-11-12)

Търсене за `CONFIG_IDF_TARGET_`, `__XTENSA__`, `xtensa`, `ESP32S3` в `main/`, `common/`, `components/`, `ap2_core`, `apple_clock`:
- **`main/playback_buttons.c`** (бутони, по подразбиране изключени): списъкът със заети пинове познава само S3
  (`#if defined(CONFIG_IDF_TARGET_ESP32S3)`: flash/PSRAM 26–37, UART 43/44, USB 19/20). **Добавено** (без `CONFIG_IDF_TARGET_*`):
  при `CONFIG_ESP_HOSTED_HOST` SDIO пиновете и reset на копроцесора от опциите на ESP-Hosted (CLK, CMD, D0–D3, reset), и
  `CONFIG_AIRPLAY_GW_TIME_PULSE_GPIO`. Блокът за S3 не е пипан. USB-JTAG на P4 (24/25) не е в списъка: в `soc` няма макрос
  за тях, а на FireBeetle не са изведени. Проверено с временен P4 build с `CONFIG_AIRPLAY_HARDWARE_BUTTONS=y` (компилира се).
- `common/surround/surround_proto.h`: коментар „both ends are ESP32-S3“ (колоните). Протоколът към колоните не се пипа;
  P4 Gateway-ът е little endian като S3, а структурите са packed, така че форматът е същият.
- Останалите попадения са в Kconfig (2.1) и в `components/ota` (2.4).

Проверено, че няма друго за P4:
- **FDK:** няма го в Gateway-а (`main/`, `common/`, Gateway CMake); само в колоните. `FDK_USE_BUILTIN_CLZ` не е нужен.
- **Микрофонът (`gw_mic.c`):** `I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, ...)`, стандартният I2S драйвер, без тактов
  източник по чип. Ще се провери с INMP441 в 2.7.
- **Wi-Fi API в `main/`:** само `esp_wifi_sta_get_ap_info` (работи през C6, 0.4) и `esp_wifi_get_tsf_time` (вижте 2.3).
- Няма твърди 240 MHz, cycle count или Xtensa код.

Намерени при пускането на P4 и поправени още в 2.3 (същият вид проблем): стартът на `apple_clock` преди точката за достъп
(`main.c`) и TSF на всеки 125 ms (`gw_speakers.c`).

Компилиране: S3 Gateway `0x14ee90` (бутоните са изключени, кодът е същият), P4 `0x124710`, P4 с бутони `0x125180`.

### 2.6 Буфер за звука от Apple (2026-11-12)

- `components/airplay_options/Kconfig.projbuild`: две скрити опции (без prompt, само за Gateway-а), по чип:
  `AIRPLAY_GW_RELAY_RING_MIB` (8 на P4, иначе 4) и `AIRPLAY_GW_ADVERTISED_BUFFER_MIB` (8 на P4, иначе 6).
- `main/gw_relay.c`: `GW_RELAY_RING_BYTES` = `CONFIG_AIRPLAY_GW_RELAY_RING_MIB` MiB, с `_Static_assert` за степен на 2
  (маската). Логиката на релето не е пипана. Индексът (65 536 кадъра) стига: при AAC 256 kbit/s кадърът е ~743 B,
  8 MiB ≈ 11 000 кадъра.
- `common/audio/sound_engine.h`: `AP2_BUFFERED_AUDIO_ADVERTISED_BYTES` = `CONFIG_AIRPLAY_GW_ADVERTISED_BUFFER_MIB` MiB, когато
  опцията я има (Gateway), иначе 6 MiB (колоните, без промяна). `ap2_core` я взема от този header (`rtsp_core.c:866`),
  константата остава в проекта.

Проверено: `sdkconfig` на S3 Gateway 4/6, на P4 8/8; колоната няма опциите (`sdkconfig` същият). Две A/B проби на колоната
(със стария `sound_engine.h` и без новия блок в Kconfig) дават същия образ освен часа на компилиране, т.е. 2.6 не я
засяга. Размерите: S3 Gateway `0x14ee90`, колона `0x1bc780`, P4 `0x124710`. P4 тръгва, PSRAM 32 595 KiB свободни след
старта; буферът се заделя при първия поток.

**Не е тествано:** AutoMix от iPhone и бързото изчистване при смяна на песента. На P4 може да се види в лога
(`gw_relay: RELAY ... buffered N KiB` над 4096 и `flushed`), но колоните още не свирят; пълният тест е след фаза 3.

## Фаза 3: хардуерното засичане на времето на P4

Кристиан пропусна 2.7 засега и поиска фаза 3, за да тръгне звукът. 3.1–3.5 са направени заедно (поотделно не дават звук).

### 3.1–3.5 (2026-11-12)

`main/gw_c6_time.{h,c}` (компилира се при `CONFIG_ESP_HOSTED_HOST`, стартира се първо в `app_main`):
- **3.1:** GPIO `AIRPLAY_GW_TIME_PULSE_GPIO` (6) → `gpio_new_etm_event(POS)` → ETM → `GPTIMER_ETM_TASK_CAPTURE` на GPTimer
  (`PLL_F80M`, 40 MHz). IRAM прекъсване на същия фронт само чете `gptimer_get_captured_count()` (първо) и записва
  (`CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM=y`, в `linker.lf` двете функции са `noflash`).
- **3.2:** в същото прекъсване, след capture: `esp_timer_get_time()`, `gptimer_get_raw_count()`, `esp_timer_get_time()`.
  GPTimer и `esp_timer` са от един кристал, затова се търси само отместване: средното на последните 64
  `(t0+t1)/2 − raw·25 ns` (двойки с разлика над 20 µs се пропускат). Местното време на фронта = отместване + capture·25 ns.
  Четенето на GPTimer след capture не може да развали capture-а (проблемът от 1.3), защото стойността вече е взета.
- **3.3:** фронтовете се номерират като на C6: първият след reset на C6 е 0. P4 започва да засича преди C6 да е тръгнал
  (reset при старта на ESP-Hosted, първият фронт на C6 е след ~0,5 s). Интервал, който не е n·период (±0,5 %), почва номерацията
  отначало (смущение при старта на C6). TICK се сдвоява с фронта **по номер**, затова закъснелите TICK-ове от 1.7 не пречат.
  Проверка: интервалът на P4 спрямо този на C6 трябва да е в 200 ppm + 1 µs. 4 отказа подред почват сдвояването отначало.
  16 TICK-а подред без фронт → номерацията се изравнява по последния фронт преди TICK-а (с предупреждение).
- **3.4:** линия „часовник на C6 → местно време“ по последните 64 двойки (8 s), отхвърляне при отклонение над 20 µs;
  `gw_c6_time_c6_to_local()` за фаза 4.
- **3.5:** `common/surround/tsf_fit.c`: при `!SOC_WIFI_SUPPORTED` `tsf_fit_read_sample()` взема пробата от
  `gw_c6_time_sample()`: (местно време на фронта, `edge_tsf_ns`), по една на извикване през опашка от 4, за да не се губи `TIME`
  при разминаване на фазите. Логиката на `tsf_fit` не е пипана; на S3 и в колоните четенето е както преди.
  **`main/gw_speakers.c` е върнат в оригиналния си вид** (махнато е временното „no TSF“ от 2.3).
- `sdkconfig.defaults.esp32p4`: `ESP_HOSTED_HOST_FEAT_PEER_DATA=y`, `GPTIMER_CTRL_FUNC_IN_IRAM=y`.
  `main/CMakeLists.txt`: `esp_driver_gptimer` винаги, `espressif__esp_hosted` по target (изискванията се четат преди
  `sdkconfig`, затова условие по `CONFIG_*` не работи там).
- Колоната ще приеме пробата за момента на фронта: проверява, че `TIME` е за момент до 2 s назад (`spk_time.c`).

**Измерено на платка 2** (C6 с `v0.1.0`, 70 s, колоните свързани, без AirPlay):
- HELLO: `airplay-gw-c6 v0.1.0`, период 5 000 006, features 0x0f;
- фронтове 498, пропуснати 0; TICK 488: сдвоени 488, несдвоени 0, отхвърлени 0, рестарти 0; първите 39 TICK са без TSF
  (TSF линията на C6 още се строи);
- **линия C6 → P4: rms 7–21 ns** (цел < 0,5 µs), +25,6 ppm; TICK идва 2,3–2,8 ms след фронта;
- **`tsf_fit` на Gateway-а: 64 проби, rms 2,1–2,6 µs**, −50,4…−50,9 ppm. Целта (< 1 µs) **не е изпълнена**: това е шумът на
  TSF при сдвояването на C6 (1.4: 2,0–2,4 µs при фронта). S3 сега: няколко µs.
- `gw_speakers: TIME ... (TSF)` се праща; `apple_clock ready=0`, защото няма AirPlay сесия.

Компилиране: P4 `0x129ec0`, S3 Gateway `0x14eea0`, колона `0x1bc780`; нови предупреждения няма.

**Тест на Кристиан (2026-11-12): колоните свирят от P4 Gateway-а** (AirPlay от iPhone, TIME от TSF на C6). Синхронът и
калибрацията (3.7) още не са мерени.

### 3.6 Уеб страница и лог (2026-11-12)

- `/api/speakers` получава обект `time`: `tsf_samples`, `tsf_rms_us`, `tsf_ppm`, `tsf_restarts` (от `tsf_fit` на
  Gateway-а, нов getter `gw_speakers_tsf_status()`, само за показване), а на P4 и `c6`: `hello`, `name`, `version`,
  `features`, фронтове (и пропуснати), TICK (сдвоени, несдвоени, отхвърлени, без TSF), рестарти на номерацията,
  `line_rms_ns`, `line_ppm`, `latency_ms`.
- `speakers.html`: нов сгъваем панел **Gateway time** (като Room calibration и Speaker firmware). Чипът показва
  `TSF ±x µs` (зелено под 5 µs) или „no TIME from the C6“ (червено), ако C6 не отговаря с нашия firmware или му липсва
  импулс или TSF. На S3: „The TSF is read from this chip's own Wi-Fi“ и редът на `tsf_fit`. Бутонът за обновяване на C6
  идва с фаза 5; засега страницата казва, че ще дойде.
- Лог: `gw_c6_time` на всеки 10 s (фронтове, сдвояване, линия C6 → P4, закъснение); `gw_speakers: TIME ... TSF ... rms`
  както преди; предупреждение след 15 s, ако C6 не отговаря на `HELLO_REQ`.

Проверено на платка 2: API-то връща стойностите (линия C6 → P4 rms 10–14 ns, `tsf_fit` rms 2,0–2,5 µs, 4892 от 4892
TICK сдвоени); панелът се показва и на ширина на телефон (375 px). На S3 страницата не е гледана на хардуер.
Компилиране: S3 Gateway `0x14f830` (страницата и API-то), колона `0x1bc780`, P4 `0x12aac0`; нови предупреждения няма.

### 3.7 Тест (2026-11-12)

**Синхрон, P4 Gateway** (AirPlay от iPhone, 3 колони, `/api/speakers` на всеки 2 s за 120 s, 57 четения). `sync` е грешката
на пускането на всяка колона спрямо графика ѝ (`sound_engine.c`), не мерене между колони:

| Колона | свири / `TIME` | sync средно | sd | макс. | TSF rms на колоната |
|---|---|---|---|---|---|
| FL (ch0) | 57/57, 57/57 | +0,003 ms | 0,011 | 0,029 ms | 8,9 µs |
| 240 (ch1) | 57/57, 57/57 | +0,012 ms | 0,013 | 0,042 ms | 5,6 µs |
| 484 (ch3) | 57/57, 57/57 | +0,004 ms | 0,007 | 0,027 ms | 6,9 µs |

`tsf_fit` на Gateway-а: rms средно 2,49 µs, макс. 3,17 µs, т.е. по-добре от TSF на самите колони (S3, 5,6–8,9 µs).

**Синхрон, S3 Gateway за сравнение** (192.168.1.98, firmware от 2.3, без полето `time` в API-то; 120 s, 58 четения):

| Колона | P4: sync средно / sd / макс. | S3: sync средно / sd / макс. | TSF rms на колоната P4 / S3 |
|---|---|---|---|
| FL (ch0) | +0,003 / 0,011 / 0,029 ms | −0,001 / 0,002 / 0,007 ms | 8,9 / 8,7 µs |
| 240 (ch1) | +0,012 / 0,013 / 0,042 ms | +0,026 / 0,025 / 0,103 ms | 5,6 / 4,8 µs |
| 484 (ch3) | +0,004 / 0,007 / 0,027 ms | −0,001 / 0,003 / 0,009 ms | 6,9 / 7,4 µs |

Двата Gateway-а са в един порядък (всичко под 0,11 ms). С P4 FL и 484 трептят малко повече (sd 7–11 µs срещу 2–3 µs), а
240 по-малко. Вероятна причина: шумът на TSF от C6 (2,5 µs rms в `tsf_fit`). Не е доказано: rms на S3 не е в старото API.
Истинската разлика между колоните се мери с осцилоскоп (`AIRPLAY_SURROUND_SYNC_PULSE`) или с калибрацията.

**Решение на Кристиан (2026-11-12, 3.6):** панелът „Gateway time“ **не е на страницата**, данните остават само в лога
(`gw_c6_time` на всеки 10 s, `gw_speakers: TIME ... TSF ... rms`). Страницата, полето `time` в API-то и getter-ът са
върнати към commit-а `27671e8`.

### Вграденият микрофон на FireBeetle 2 (2026-11-12)

**Решение на Кристиан:** вграденият микрофон да се ползва и да е по подразбиране.
- `Kconfig.projbuild`: `choice AIRPLAY_SURROUND_CAL_MIC_TYPE`: `AIRPLAY_SURROUND_CAL_MIC_INMP441` (I2S, SCK/WS/SD, както
  досега) или `AIRPLAY_SURROUND_CAL_MIC_PDM` (`depends on SOC_I2S_SUPPORTS_PDM_RX`), с пинове
  `AIRPLAY_SURROUND_CAL_MIC_PDM_CLK_GPIO` (12) и `_DATA_GPIO` (9) по wiki-то на DFRobot (0.11). По подразбиране PDM на
  FireBeetle 2 P4, иначе INMP441. На S3 нищо не се сменя (`sdkconfig`: само новият избор `INMP441=y`; образ `0x14eea0`).
- `main/gw_mic.c`: при PDM `I2S_NUM_0` (PDM RX е само на I2S0, `SOC_I2S_PDM_MAX_RX_LINES` „On I2S0“), `i2s_channel_init_pdm_rx_mode`,
  16-битов PCM от хардуерния PDM→PCM филтър (`I2S_PDM_DATA_FMT_PCM`, high-pass 35,5 Hz по подразбиране), 48 kHz, DMA по 256
  кадъра (подравняване на 64 байта). Времената на пробите са от DMA, както при INMP441.
- **Намерено:** на P4 под v3.0 I2S тактът по подразбиране е XTAL (`hal/esp32p4/include/hal/i2s_ll.h`: `PLL_160M` само за v3),
  а PDM при 48 kHz иска MCLK 24,576 MHz от източник поне 2× по-бърз → `sample rate is too large`. Решено с
  `I2S_CLK_SRC_APLL` при `SOC_I2S_SUPPORTS_APLL` (на S3 няма APLL, там е по подразбиране).
- Тест на платка 2 („Test mic“, `/api/calibration` `test`): 48 128 проби, 189 DMA времена, **20,833 µs/проба**,
  `Microphone works; room noise -61…-64 dBFS`, пик −54 dBFS. Пълна калибрация с вградения микрофон още не е правена.

**Калибрация с вградения микрофон, P4 (Кристиан, 2026-11-12):** 42 s, и трите колони измерени.

| Колона | разстояние | време | SNR | разсейване | закъснение | ниво |
|---|---|---|---|---|---|---|
| 484 (ch3, sub) | 1,81 m | 5,26 ms | 7,0 dB | 0,11 ms | 1,6 ms | по ухо |
| 240 (ch1) | 1,56 m | 4,54 ms | 11,4 dB | 0,03 ms | 2,3 ms | 0 dB |
| FL (ch0) | 2,34 m | 6,84 ms | 15,4 dB | 0,06 ms | 0 ms | −4 dB |

Шум −60,9 dBFS, пик −49 dBFS. **Отворено:** FL и 240 излизат `inverted` (подсказка за обърната полярност). Ако със S3 и
INMP441 не излизат, PDM микрофонът дава обърнат знак и пробите трябва да се обръщат. Сравнение със S3 още няма.
Кристиан: със S3 и INMP441 FL и 240 също излизат `inverted`. Значи PDM микрофонът е със същия знак като INMP441 и
подсказката е за самите колони. Промяна не трябва. Закъсненията от двете калибрации не са сравнени число по число.

## Фаза 4: PTP от Apple със засичане на C6 (2026-11-12)

По решението от 1.6: само **приемането**. `Delay_Req` остава засечен на P4 (`s.t3`), затова няма отложена двойка и таймаут.

**4.1–4.2, `esp-airplay2-core` (локално, без commit до теста):**
- `apple_clock_set_rx_time_hook()` и `apple_clock_rx_time_hook_t` (`include/apple_clock.h`): по подразбиране няма кука.
- `apple_clock.c`: при Sync пази `sequenceId` и `sourcePortIdentity` (байтове 20–29). `t2` влиза в сметката чак при
  `Delay_Resp` (`acc_add_exchange`), около RTT след Sync. Затова куката се пита **тогава** и не чака: записът от C6 е дошъл
  (в 1.7 той идва средно 1,1 ms, най-много около 4 ms след пакета). Приема се само време ≤ времето от сокета и не по-рано от
  100 ms. Иначе се ползва времето от сокета, както без кука.
- Логът на `apple_clock` получава `| Sync time from hook N of M` **само когато има кука**; S3 и колоните пишат както преди.

**4.3, Gateway (P4):** `gw_c6_time.c` пази PTP записите от `TICK` и `PTP` (64 записа, до 2 s). `gw_c6_time_ptp_rx()` търси по
тип, номер и източник и превръща `c6_ticks` с линията от 3.4. `main.c` регистрира куката след `gw_c6_time_start()`.

**Пробата с локалното ядро:** `override_path` в `main/idf_component.yml` не се прилага към git зависимост (component
manager-ът решава пак от git, и със съществуващ, и без lock). Работи `-D EXTRA_COMPONENT_DIRS=<локално apple_clock>`:
компонент с това име извън `managed_components` е с предимство. Той обаче пренаписва lock файловете, затова те се пазят
и се връщат след пробата.

**Тест на платка 2** (AirPlay от iPhone, 95 s):
- **Sync с време от C6: 573 от 573 (100 %)**, `missed 0`; PTP записи 2693;
- **floor RTT 2,03 ms** (във фаза 3 без кука: 2,81–4,05 ms): махнато е закъснението C6 → P4 на Sync;
- **`estimate` (отклонение на линията): от −0,073 до +0,044 ms, sd ≈ 0,03 ms** (фаза 3 без кука, началото на сесия:
  от −0,340 до +0,105 ms, sd ≈ 0,18 ms); наклонът се успокоява до около ±1 ppm след около 50 s. Сравнението не е при
  еднакви условия (различни сесии), но разликата е около 5 пъти.
- линия C6 → P4 rms 8–18 ns; всички TICK сдвоени.

S3 Gateway с локалното ядро (без кука): компилира се, `0x14ef50` (+176 байта за куката). Колоната е на `v1.0.0`.

**Не е направено:** 4.4 (`CONFIG_AIRPLAY_APPLE_CLOCK_ASYM_US` спрямо HomePod) и 4.6 (синхрон с HomePod/Apple TV,
пълно натоварване). Таг `v1.1.0` (4.5) чака одобрение.

**Решение на Кристиан (2026-11-12, 4.5):** първо съраундът остана на `v1.0.1`, после Кристиан одобри: commit, таг
`v1.1.0` и push в `esp-airplay2-core`, и съраундът минава на `v1.1.0`.
- `esp-airplay2-core`: commit `f32eaf8` „apple_clock: optional receive-time hook for Sync“ (куката + README: куката и
  локален build с `EXTRA_COMPONENT_DIRS`, защото `override_path` не се прилага към тези git зависимости), таг `v1.1.0`
  (`32c22d9`), push в GitHub.
- Съраундът: `ap2_core` и `apple_clock` на `v1.1.0`; в S3 lock-а са сменени само двата записа и `manifest_hash`
  (`led_strip` остава 3.0.3), P4 lock-ът е решен наново; `main.c` регистрира куката. Колоната остава на `v1.0.0`.
- Проверено: S3 Gateway `0x14ef50`, колона без промяна, P4 `0x129b20`, взема `apple_clock` от `managed_components`;
  на платка 2 `Sync time from hook 9 of 9`.

## Фаза 5: управление на firmware-а на C6 (2026-11-12)

- **5.1:** образът е вграден от 2.0 (`coprocessor/c6/prebuilt/`, `target_add_binary_data`); сега кодът го ползва, затова
  остава в образа на P4: `0x24b580` (2,35 MB, 62 % свободно в 6 MB дял).
- **5.2 (`main/gw_c6_fw.c`):** `esp_hosted_get_coprocessor_app_desc()` не пита C6 в 3.1.0 (0.2), затова C6 се познава по
  `HELLO` (име и версия от неговия `esp_app_desc`); заводският не отговаря. Името и версията на вградения образ се четат от
  `esp_app_desc_t` в самия образ. Ако до 20 s няма `HELLO` или името/версията не съвпадат: OTA за копроцесор
  (`begin`, `write` по 1400 байта, `end`, `activate` при C6 ≥ 2.6), после **P4 се рестартира сам** след 300 ms. Без това
  esp_hosted рестартира P4 с `abort()` при падналия SDIO (`TRANSPORT_FAILURE: forcing host reset`), което изглежда като срив.
  Опитите се броят в NVS (`gw_c6`: `crc` на образа, `tries`), най-много 3 за един образ; при съвпадение се нулират.
  **Решение на Кристиан:** не се отказва завинаги. Включване на тока (`ESP_RST_POWERON`) или reset пинът (`ESP_RST_EXT`)
  нулира броя, така че при всяко включване пак има 3 опита; собствените рестарти на Gateway-а след OTA (`ESP_RST_SW`) и
  сривове не го нулират, за да няма безкраен цикъл. Ръчното обновяване остава за краен случай.
  `main.c` стартира проверката веднага след `nvs_flash_init()`.

**Тестове на платка 2:**
| Случай | Резултат |
|---|---|
| C6 = вграденият (`v0.1.0`) | `the C6 runs airplay-gw-c6 v0.1.0, as the Gateway carries`, без обновяване |
| вграден `v0.1.1` (временно), C6 `v0.1.0` | OTA 9,3 s, рестарт, `v0.1.1` |
| обратно: вграден `v0.1.0`, C6 `v0.1.1` | OTA 9,0 s, чист рестарт (`restarting the Gateway with the C6`), `v0.1.0` |
| вграден заводският app (`network_adapter 83efce6-dirty`, изваден от образа на DFRobot от `0x10000`) | инсталиран; после без `HELLO` още веднъж; на 2-рия опит: `2 updates … did not take: not trying again` (тогава с най-много 2 и без нулиране) |
| **C6 със заводски firmware, вграден `v0.1.0`** | Gateway-ът тръгва и със заводския C6 (Wi-Fi работи); 20 s без `HELLO` → OTA 10,4 s → рестарт → `as the Gateway carries`, импулс и TICK сдвоени |

Тестовите образи (`v0.1.1`, заводският) бяха само временно в `prebuilt/`; `coprocessor/c6` е както в git, C6 на платка 2 е
с `v0.1.0`. S3 Gateway `0x14ef50` (без промяна), колоната без промяна.

- **5.4:** README: C6 firmware-ът се слага от Gateway-а; USB-TTL остава само за възстановяване (не е пробван).
- **5.3 (решение на Кристиан: в картата System):** `GET /api/c6` (състояние, версия на C6 и на вградения образ, процент) и
  `POST /api/c6 {"action":"update"}` (с админ проверката), само при `CONFIG_ESP_HOSTED_HOST` (`main/web_gateway.c`). На
  главната страница (`data/www/index.html`, обща) ред „Wi-Fi chip (ESP32-C6)“ с бутон **Update**; показва се само ако
  `/api/c6` отговори, т.е. не на S3 Gateway-а и не на колоните. Проверено на платка 2: показва `v0.1.0 · up to date`; POST →
  `updating … 70 %` → OTA 8,2 s → рестарт → `as the Gateway carries`. S3 Gateway `0x14f560` и колона `0x1bcd80`
  (по-големи само заради общата страница).
- **5.5 прекъсване на захранването (тест на Кристиан, платка 2):** по време на музика (3 колони) **Update** от страницата
  → `installing airplay-gw-c6 v0.1.0` → спрян ток при `C6 update 60 %`. След включване: SDIO и `chip id 0x0d`, `HELLO`
  от C6 (`airplay-gw-c6 v0.1.0`, период `5000006`), `the C6 runs airplay-gw-c6 v0.1.0, as the Gateway carries`, Wi-Fi,
  трите колони, музика, `paired` всички TICK, `Sync time from hook` всички. Обяснение: OTA пише в другия OTA дял на C6, а
  активният се сменя чак в `end`/`activate`; прекъснат запис оставя C6 с досегашния firmware. Ръчното обновяване не брои
  опити, затова нулирането при включване (`power-on: the C6 update tries start again`) не се вижда тук; то остава
  непроверено на платка с C6, който отказва образа.

### Компилиране по обикновения начин (решение на Кристиан)

README описва само `idf.py set-target esp32p4` / `idf.py set-target esp32s3` в главната папка и после `idf.py build`;
отделната папка `-B build_p4 -D SDKCONFIG=sdkconfig.p4` е махната от README. Проверено от нулата (изтрити `build/`):
P4 `0x24bfa0` (`sdkconfig` същият като `sdkconfig.p4`), обратно на S3 `0x14f560`, колона `0x1bcd80`; `dependencies.lock`
и `dependencies.lock.esp32p4` не се променят. Новият S3 `sdkconfig` се различава от стария само с
`CONFIG_ESP_INT_WDT_TIMEOUT_MS=800` (стандартното на IDF 6.1 при PSRAM; старият беше създаден с 6.0.2 и имаше 300) — колоната
също е с 800.
