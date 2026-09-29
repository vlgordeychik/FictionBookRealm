# FictionBook Realm — исходники прошивки ESP32 (M5Paper S3)

Набор исходников для сборки прошивки FB2-ридера под **ESP32-S3 (M5Paper S3)**.
Никаких привязок к путям конкретной машины: все пути в CMake задаются
относительно корня проекта, единственная внешняя переменная — стандартная
`$ENV{IDF_PATH}`.

**Архив: 10.5 МБ / 491 файл.** Библиотеки M5Stack (M5GFX + M5Unified,
~170 МБ распакованными) в архив **не входят** — они подтягиваются скриптом перед
сборкой, см. «Пререквизиты».

Собран и проверен: `python tools/fetch_deps.py && idf.py set-target esp32s3 && idf.py build`.

## Требования

| Компонент | Версия |
|---|---|
| ESP-IDF | 6.1.x (`release/v6.1`) |
| Цель | `esp32s3` |
| Python | 3.9+ (требования ESP-IDF) |
| Инструменты | CMake, Ninja (ставится вместе с ESP-IDF) |

Сеть нужна **один раз** — на шаг `tools/fetch_deps.py`. После этого дерево
собирается полностью офлайн.

## Быстрый старт

```bash
# Linux / macOS
. $IDF_PATH/export.sh

# Windows (PowerShell)
& "$env:IDF_PATH\export.ps1"

python tools/fetch_deps.py        # ~50 МБ, ~1 мин, один раз
idf.py set-target esp32s3
idf.py build
idf.py -p <COM_PORT> flash monitor
```

## Пререквизиты

`tools/fetch_deps.py` качает с GitHub две библиотеки M5Stack по зафиксированным
тегам, распаковывает их в `components/` и заново накладывает локальные правки,
которые есть в рабочем проекте:

| Компонент | Тег | tarball | Распакованный |
|---|---|---|---|
| `components/M5GFX` | `m5stack/M5GFX@0.2.28` | 49 МБ | ~170 МБ |
| `components/M5Unified` | `m5stack/M5Unified@0.2.21` | 0.6 МБ | ~0.7 МБ |

Из 170 МБ M5GFX около 122 МБ — растровые CJK-шрифты
(`src/lgfx/Fonts/{efont,IPA}/*.c`). Они компилируются в библиотеку, но ридер их
не использует; убрать их — значит патчить upstream, поэтому они приезжают вместе
с библиотекой.

Скачанное дерево побайтово совпадает с рабочим проектом (M5GFX — все 294 файла
идентичны, M5Unified — все 158, кроме намеренно подменённого манифеста).

Ключи скрипта:

```bash
python tools/fetch_deps.py            # дозагрузить недостающее
python tools/fetch_deps.py --check    # только отчёт, ничего не меняет
python tools/fetch_deps.py --force    # перекачать заново
python tools/fetch_deps.py --strict   # падать при несовпадении sha256
```

Скачанные архивы кэшируются в `.deps-cache/` (каталог в `.gitignore`), так что
повторный запуск не требует сети. Если пререквизита нет, конфигурация падает
сразу с понятным сообщением, а не внутри графа компонентов:

```
CMake Error at CMakeLists.txt:21 (message):
  Missing third-party components: M5GFX
  ...
      python tools/fetch_deps.py
```

### Локальные правки, которые скрипт накладывает

Обе обязательны, обе применяются со строгой проверкой якоря — если upstream
изменится, скрипт упадёт с требованием перенести правку вручную, а не молча
её потеряет.

1. `M5GFX/src/lgfx/v1/platforms/esp32/common.cpp` —
   `buscfg.dma_burst_size = 0` под `ESP_IDF_VERSION >= 6.0`: с ESP-IDF 6.1 это
   обязательное поле `spi_bus_config_t`.
2. `M5Unified/src/utility/Power_Class.cpp` — включение питания SD-карты
   (M5PM1 + M5IOE1 gpio6). Без этого карта не получает питание.

Третья правка — служебная: из `M5Unified/idf_component.yml` убирается registry-зависимость
`m5stack/m5gfx`, иначе component manager скачает в `managed_components/` вторую
копию M5GFX (ещё 122 МБ) при каждой конфигурации.

## Что вендорено в архиве

Всё остальное лежит в архиве и сети не требует:

* `bm8563` — драйвер RTC (репозиторий `tuupola/bm8563`, не M5Stack)
* `pugixml` — XML-парсер
* `fatfs` (R0.14) — файловая система SD-карты
* `freetype2` — растеризация шрифтов (только используемые модули)
* `stb` — декодирование изображений
* `src/swofflogo_jpg.h` — логотип выключения, уже встроен в исходник

## Структура

```
CMakeLists.txt              корень сборки ESP-IDF (project fictionbook_realm)
sdkconfig.defaults          настройки платы по умолчанию
partitions.csv              таблица разделов (nvs / phy_init / factory 3 MB)
main/                       точка входа прошивки
bm8563/                     драйвер часов реального времени (EXTRA_COMPONENT_DIRS)
components/
  fb2_core/                 парсер и рендерер FB2
  filesys/                  FatFs + бэкенды дисков (SPI/SDMMC)
  font_renderer/            обёртка FreeType2
  freeimage/                декодер изображений (stb)
  freetype/                 компонент FreeType2 + кастомный ftmodule.h
  platform/                 дисплей, тач, питание, RTC
  pugixml/                  XML-парсер
  scenes/                   UI-сцены и менеджер сцен
  M5GFX/  M5Unified/        ← создаёт tools/fetch_deps.py, в архиве НЕ лежат
src/                        общий код приложения (только ESP32-часть)
include/                    публичные заголовки
tools/fetch_deps.py         загрузка пререквизитов
stb/  fatfs/  freetype2/  pugixml/   сторонние библиотеки
```

## Ресурсы времени выполнения

Шрифты **не встроены** в прошивку — они читаются с SD-карты. Положите
`arial.ttf` (или `verdana.ttf` и т. п.) в каталог `.fonts/` на SD-карте.
Шрифт выбирается в настройках (`font_file`, по умолчанию `arial.ttf`).
Логотип выключения уже встроен (`src/swofflogo_jpg.h`).

## Собранные измеренные значения

`idf.py build` на ESP-IDF 6.1.0 / esp32s3:

```
Bootloader binary size 0x52b0 bytes. 0x2d50 bytes (35%) free.
fictionbook_realm.bin binary size 0x1169e0 bytes (1 141 216).
Smallest app partition is 0x300000 bytes. 0x1e9620 bytes (64%) free.
```

## Отличия от рабочей сборки в исходном проекте

* `CONFIG_HEAP_POISONING_*` — в исходном `sdkconfig` стоял `COMPREHENSIVE`
  (отладочная опция, попала туда через `menuconfig`, в `sdkconfig.defaults` её
  не было). В дистрибутиве остался дефолт `DISABLED`, из-за чего бинарь на
  256 байт меньше оригинального. Если нужно то же поведение — добавьте
  `CONFIG_HEAP_POISONING_COMPREHENSIVE=y` в `sdkconfig.defaults`.
* Настройки консоли (USB Serial/JTAG) в исходном проекте были только в
  локальном `sdkconfig`; они перенесены в `sdkconfig.defaults`, иначе свежая
  сборка пошла бы на UART0, недоступный на M5Paper S3.
* `managed_components/m5stack__m5gfx` (дубликат M5GFX из реестра Espressif) в
  сборке не участвует — вендоренный `components/M5GFX` покрывает все нужные
  символы.

## Правки вендоренного FreeType2

Удалены каталоги `docs/`, `builds/`, `tests/`, `devel/`, `objs/`, `subprojects/`,
корневые build-файлы (autotools/meson/msbuild/vms) и неиспользуемые модули
`src/`: `bdf`, `bzip2`, `cid`, `dlg`, `gxvalid`, `lzw`, `otvalid`, `pcf`, `pfr`,
`sdf`, `svg`, `tools`, `type1`, `type42`, `winfonts`. Собираются только модули,
перечисленные в `components/freetype/CMakeLists.txt`; каждый umbrella-файл
подключает только соседние файлы своего каталога, так что links не рвётся.
Из `bm8563/`, `M5GFX/`, `M5Unified/` убраны `.github/`, `docs/`, `examples/`,
`tests/` и прочие метафайлы.

## Что НЕ входит (и почему)

* `src/sims3/`, `ugui/`, `win32/`, `CMakeLists_win32.txt` — десктопный
  симулятор (содержит жёсткие `C:\Windows\Fonts\...`), к ESP32 отношения не имеет.
* `sdkconfig` — генерируется ESP-IDF из `sdkconfig.defaults`.
* `build/`, `*.bin`, `*.obj`, `*.zip` — артефакты сборки/прошивки.
* `assets/` — шрифты и иконки читаются с SD-карты, логотип уже скомпилирован
  в `src/swofflogo_jpg.h`.
* `doc/`, `sample/`, `sdimage/` — документация, образцы книг, образ SD-образа.

## Лицензии

Исходники прошивки — **MIT**, см. [LICENSE](LICENSE). Полный перечень авторов,
атрибуции и условий сторонних компонентов (включая шрифты) — в
[AUTHORS](AUTHORS).

Сторонние библиотеки сохраняют собственные условия: `bm8563/LICENSE`,
`freetype2/LICENSE.TXT`, а также заголовки в `fatfs/ff.c`, `pugixml/pugixml.hpp`
и `stb/stb_image.h`. Лицензии M5GFX и M5Unified приезжают вместе с библиотеками
(`LICENSE` в корне каждого компонента) — они не включены в архив исходников.

Обратите внимание: FreeType распространяется под двойной лицензией
(FTL **или** GPL-2.0-or-later), и MIT-обёртка проекта её не отменяет.
Распространение бинарника под GPL требует передачи полного соответствующего
исходника.
