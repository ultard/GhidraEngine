# ghidraengine

Поиск дубликатов фото и видео — точных и перцептивно похожих. Библиотека C++20, CLI и
плоский C ABI для биндингов.

- **Быстро.** JPEG декодируется сразу в 1/8 масштаба, полноразмерный битмап не создаётся.
  Поиск похожих идёт через Multi-Index Hashing: на миллионе файлов запрос в 37 раз дешевле
  перебора.
- **Точно.** Четыре подписи с одного декода: две перцептивные, градиентная и цветовая.
  Кандидат обязан пройти все пороги, поэтому разные снимки, совпадающие в градациях серого,
  отсеиваются.
- **Осторожно.** Библиотека ничего не удаляет и не изменяет на диске — она возвращает отчёт.
  CLI удаляет только по явному `--confirm`.

Зависимости: FFmpeg, libjpeg-turbo, xxHash, SQLite.

## Сборка

Нужны CMake ≥ 3.25, компилятор с C++20 и [vcpkg](https://github.com/microsoft/vcpkg) с
переменной окружения `VCPKG_ROOT`. Имя пресета одно и то же для конфигурации, сборки и тестов:

```powershell
$env:VCPKG_ROOT = "C:\dev\vcpkg"       # Linux/macOS: export VCPKG_ROOT=~/vcpkg
cmake --preset windows-release         # linux-release, macos-release
cmake --build --preset windows-release
ctest --preset windows-release
```

## CLI

```bash
ghidraengine D:/Photos                          # найти всё
ghidraengine --exact-only -f json D:/Photos     # только точные копии, машиночитаемо
ghidraengine -t 6 --rotations --cache D:/Photos # строже, с поворотами, с кешем подписей
ghidraengine --delete --confirm D:/Photos       # удалить всё, кроме основного файла кластера
```

Полный список флагов, форматы вывода и коды возврата — [docs/cli.md](docs/cli.md).

## Библиотека

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

```cmake
find_package(ghidraengine CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE ghidraengine::ghidraengine)
```

Подробности — [docs/library.md](docs/library.md), для других языков —
[docs/bindings.md](docs/bindings.md).

## Документация

| Документ | О чём |
|---|---|
| [Как это работает](docs/architecture.md) | Конвейер сканирования, хеши, индекс, кластеризация, кеш |
| [CLI](docs/cli.md) | Все флаги, форматы вывода, коды возврата |
| [Библиотека C++](docs/library.md) | CMake, `Scanner`, `ScanConfig`, `Report` |
| [Биндинги](docs/bindings.md) | C ABI, примеры на C и Python |
| [Бенчмарки](docs/benchmarks.md) | Что измеряется, как запустить, результаты |

## Настройка точности

| Параметр | По умолчанию | Смысл |
|---|---|---|
| `image.phash_threshold` | 10 | расстояние Хэмминга из 64; 6 — строже, 14 тянет чужое |
| `image.color_threshold` | 24 | разница цветности 0–255; 255 отключает проверку |
| `image.dihedral_invariant` | false | совпадение при поворотах и отражениях |
| `video.frame_samples` | 16 | ключевых кадров на файл |
| `video.min_frame_match_ratio` | 0.65 | доля совпавших кадров |
| `cluster_mode` | `Strict` | `Transitive` даёт полноту ценой цепного склеивания |
| `verify_bytes` | false | побайтовое подтверждение точных дубликатов |
