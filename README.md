# ghidraengine

C++20 библиотека и CLI для поиска дубликатов фото и видео — точных и перцептивно похожих.

Зависимости: FFmpeg, libjpeg-turbo, xxHash, SQLite.

---

## Сборка

Нужны CMake ≥ 3.25, компилятор с C++20 и [vcpkg](https://github.com/microsoft/vcpkg)
с переменной окружения `VCPKG_ROOT`, указывающей на его каталог.

Имя пресета одно и то же для конфигурации, сборки и тестов:

```powershell
# Windows
$env:VCPKG_ROOT = "C:\dev\vcpkg"
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

```bash
# Linux
export VCPKG_ROOT=~/vcpkg
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
```

```bash
# macOS
export VCPKG_ROOT=~/vcpkg
cmake --preset macos-release
cmake --build --preset macos-release
ctest --preset macos-release
```

Первый `cmake --preset` собирает зависимости через vcpkg — это долго (FFmpeg).
Дальше сборка инкрементальная; все пресеты одного триплета делят общий
`build/vcpkg_installed`, так что порты собираются один раз на всю машину-проект.

### Пресеты

| Пресет | Генератор | Тип сборки |
|---|---|---|
| `windows-debug` | Ninja | Debug |
| `windows-release` | Ninja | RelWithDebInfo |
| `windows-bench` | Ninja | Release + бенчмарки |
| `windows-vs` | Visual Studio 17 2022 | мультиконфиг |
| `linux-debug` | Ninja | Debug |
| `linux-release` | Ninja | RelWithDebInfo |
| `linux-bench` | Ninja | Release + бенчмарки |
| `macos-debug` | Ninja | Debug |
| `macos-release` | Ninja | RelWithDebInfo |
| `macos-bench` | Ninja | Release + бенчмарки |

Ninja-пресеты на Windows требуют окружения MSVC и `ninja` в `PATH`: запускать из
**Developer PowerShell for VS 2022** с установленным компонентом VS *«C++ CMake
tools for Windows»* (он и приносит ninja). Если его нет — используйте
`windows-vs`: он работает из любой оболочки и заодно генерирует `.sln`.
В CLion ninja встроенный, там ничего доставлять не нужно.

Готовый бинарник:

| Пресет | Путь |
|---|---|
| `windows-release` | `build/windows-release/cli/ghidraengine.exe` |
| `windows-vs` | `build/windows-vs/cli/RelWithDebInfo/ghidraengine.exe` |
| `linux-release` | `build/linux-release/cli/ghidraengine` |
| `macos-release` | `build/macos-release/cli/ghidraengine` |

Опции CMake: `GHIDRAENGINE_BUILD_CLI`, `GHIDRAENGINE_BUILD_TESTS`,
`GHIDRAENGINE_BUILD_BENCHMARKS`, `GHIDRAENGINE_ENABLE_LTO`, `GHIDRAENGINE_ENABLE_SIMD`.
Бенчмарки выключены по умолчанию — их включает пресет `*-bench`, который заодно
подтягивает фичу vcpkg `benchmarks`. Переключение между `*-bench` и остальными
пресетами доставляет/убирает порт `benchmark` в общем дереве — это секунды из
бинарного кэша vcpkg.

### CLion

Профили подхватываются из `CMakePresets.json` автоматически — ничего настраивать
руками не нужно. Два условия:

- в *Settings | Build, Execution, Deployment | Toolchains* выбран toolchain
  **Visual Studio** — он и даёт Ninja-пресетам окружение MSVC;
- `VCPKG_ROOT` задан системно (в переменных среды Windows), а не только в текущей
  сессии PowerShell — иначе процесс CLion его не увидит.

---

## Использование как библиотеки

```cmake
find_package(ghidraengine CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE ghidraengine::ghidraengine)
```

```cpp
#include <ghidraengine/ghidraengine.hpp>

ghidraengine::ScanConfig cfg;
cfg.image.phash_threshold = 10;

ghidraengine::Scanner scanner(std::move(cfg));
std::vector<std::filesystem::path> roots{"D:/Photos"};

auto report = scanner.scan(roots);
if (!report) {
    return report.error().message;
}
for (const auto& cluster : report->clusters) {
    // cluster.members — индексы в report->files, cluster.keeper — что оставить
}
```

Библиотека ничего не удаляет и не изменяет на диске — она возвращает отчёт.
Ошибки отдельных файлов попадают в `report.errors` и не роняют скан; `Result<T>`
возвращает ошибку только если скан не смог начаться.

Для биндингов есть плоский C ABI — `include/ghidraengine/ghidraengine_c.h`.
Исключения через границу не проходят, строки UTF-8.

---

## Как это работает

Каждая стадия отсеивает кандидатов до того, как следующая потратит на них I/O или CPU:

```
обход ФС → кеш → размер → частичный хеш → полный хеш      точные дубликаты
              ↘ декод 1/8 → pHash/dHash → MIH → кластеры   похожие
```

**Точные дубликаты.** Уникальный размер — мгновенный отсев. Дальше XXH3-128 по первым и
последним 64 KiB, и только для выживших групп — полный потоковый хеш.

**Фото.** libjpeg-turbo декодирует JPEG из DCT-коэффициентов сразу в 1/8 масштаба
(6000×4000 → 750×500), полноразмерный битмап не создаётся. Остальные форматы (PNG,
WebP, AVIF, HEIF, TIFF, BMP, GIF, raw) — через FFmpeg. Из одного буфера 32×32 считаются
четыре хеша: `phash64` (DCT, устойчив к пережатию), `phash256` (режет ложные
срабатывания на больших корпусах), `dhash64` (градиент), `colorMoments` (цветность,
отсекает разные снимки, совпадающие в градациях серого).

**Видео.** `AVDISCARD_NONKEY`, 16 отсчётов равномерно по таймлайну, первые и последние
5% пропускаются. Кадры с низкой дисперсией (чёрные, затемнения) пересемплируются.

**Поиск похожих.** Multi-Index Hashing: 64-битный код режется на 4 полосы по 16 бит,
пара на расстоянии ≤ d обязана совпасть хотя бы в одной полосе. Индекс точен, а не
приблизителен — тест сверяет его с полным перебором.

**Кластеризация.** `Strict` (по умолчанию): элемент входит в кластер только если совпал
с представителем. `Transitive`: union-find, выше полнота, но A~B и B~C склеиваются даже
когда A и C разные.

**SIMD** выбирается в рантайме через cpuid (AVX2 → SSE2 → скаляр, NEON на AArch64).
Тесты проверяют, что все ядра дают идентичный хеш.

---

## Производительность

24 потока @ 3.7 ГГц, MSVC 19.44, RelWithDebInfo. Воспроизводится через
`ghidraengine_bench`.

Декод JPEG, 1/8 против полного разрешения:

| Разрешение | Fast path | Полный декод | Выигрыш |
|---|---|---|---|
| 1920×1080 | 1.11 мс | 3.93 мс | 3.5× |
| 4000×3000 | 5.60 мс | 20.7 мс | 3.7× |
| 6000×4000 | 10.6 мс | 40.2 мс | 3.8× |

Поиск, MIH против линейного скана (порог 10 бит, один запрос):

| Корпус | MIH | Линейный | Выигрыш |
|---|---|---|---|
| 10 000 | 1.5 мкс | 14.7 мкс | 10× |
| 100 000 | 5.5 мкс | 147 мкс | 27× |
| 1 000 000 | 39.8 мкс | 1467 мкс | 37× |

- DCT 32×32→16×16: 557 нс на AVX2 против 2566 нс скалярно — 4.6×.
- Все четыре хеша поверх декода 4000×3000: +30 мкс к 5.60 мс — 0.5%.
- Повторный скан по кешу: 3.6 мс против 37 мс, ноль прочитанных байт.

---

## Настройка точности

| Параметр | По умолчанию | Смысл |
|---|---|---|
| `image.phash_threshold` | 10 | Расстояние Хэмминга из 64. 6 — строже, 14 тянет чужое |
| `image.phash256_threshold` | 40 | Подтверждение на 256 битах |
| `image.color_threshold` | 24 | Разница цветности 0–255; 255 отключает проверку |
| `image.dihedral_invariant` | false | Совпадение при поворотах и отражениях |
| `video.frame_samples` | 16 | Ключевых кадров на файл |
| `video.duration_tolerance` | 0.02 | Допуск по длительности |
| `video.min_frame_match_ratio` | 0.65 | Доля совпавших кадров |
| `cluster_mode` | `Strict` | `Transitive` даёт полноту ценой цепного склеивания |
| `verify_bytes` | false | Побайтовое подтверждение точных дубликатов |
