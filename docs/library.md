# Библиотека C++

Требуется C++20. Публичный заголовок один: `<ghidraengine/ghidraengine.hpp>`.

## Подключение

```cmake
find_package(ghidraengine CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE ghidraengine::ghidraengine)
```

Либо как подкаталог:

```cmake
add_subdirectory(third_party/ghidraengine)
target_link_libraries(my_app PRIVATE ghidraengine::ghidraengine)
```

Опции сборки:

| Опция | По умолчанию | Смысл |
|---|---|---|
| `GHIDRAENGINE_BUILD_CLI` | ON | собирать `ghidraengine` |
| `GHIDRAENGINE_BUILD_TESTS` | ON | собирать тесты |
| `GHIDRAENGINE_BUILD_BENCHMARKS` | OFF | собирать `ghidraengine_bench` |
| `GHIDRAENGINE_ENABLE_LTO` | ON | LTO в оптимизированных конфигурациях |
| `GHIDRAENGINE_ENABLE_SIMD` | ON | SIMD-ядра с рантайм-диспетчеризацией |
| `BUILD_SHARED_LIBS` | ON | .dll/.so; `OFF` даёт статическую библиотеку |

## Минимальный пример

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
    const auto& keeper = report->files[cluster.keeper];
    for (std::uint32_t member : cluster.members) {
        if (member == cluster.keeper) continue;
        // report->files[member] — кандидат на удаление, решение за вами
    }
}
```

Библиотека ничего не удаляет и не изменяет на диске: она возвращает отчёт.

## Ошибки

`Result<T>` — это `T` либо `Error{ErrorCode, message}`, без исключений в публичном API:

```cpp
auto hash = ghidraengine::hash_file("a.jpg");
if (!hash) {
    std::println("{}", hash.error().message);
}
```

Разделение важное: ошибка отдельного файла попадает в `Report::errors` и в колбэк
`on_error`, но скан продолжается. `Result<Report>` возвращает ошибку, только если скан не
смог начаться — некорректная конфигурация (`ScanConfig::validate()`) или недоступные корни.

## Отмена и прогресс

```cpp
std::stop_source source;

cfg.on_progress = [](const ghidraengine::Progress& p) {
    // вызывается из рабочих потоков: должен быть потокобезопасным и не блокировать
    std::print(stderr, "\r{} {}/{}", to_string(p.phase), p.processed, p.total);
};

auto report = scanner.scan(roots, source.get_token());
// source.request_stop() из другого потока → report->cancelled == true, отчёт частичный
```

Фазы: `Enumerating`, `Hashing`, `Decoding`, `Indexing`, `Clustering`, `Done`.
`total == 0` означает, что общее число ещё неизвестно.

## Потокобезопасность

- `Scanner` не потокобезопасен; `scan()` синхронен и распараллеливается внутри.
- Один `Scanner` можно переиспользовать последовательно, меняя конфигурацию через
  `set_config()`.
- `on_progress` и `on_error` вызываются из рабочих потоков.
- Свободные функции (`hash_file`, `compute_image_signature`, `images_match`, …)
  потокобезопасны.

## ScanConfig

### Что искать

| Поле | По умолчанию | Смысл |
|---|---|---|
| `detect_exact` | true | искать побайтовые дубликаты |
| `detect_similar` | true | искать перцептивно похожие |
| `scan_images` | true | обрабатывать изображения |
| `scan_videos` | true | обрабатывать видео |

### Обход

| Поле | По умолчанию | Смысл |
|---|---|---|
| `min_file_size` | 4096 | нижняя граница размера |
| `max_file_size` | 0 | верхняя граница, 0 — без ограничения |
| `follow_symlinks` | false | ходить по символическим ссылкам |
| `skip_hidden` | true | пропускать скрытые файлы |
| `max_depth` | 0 | глубина рекурсии, 0 — без ограничения |
| `exclude_patterns` | пусто | глобы по полному пути |
| `probe_unknown_extensions` | false | открывать файлы с любым расширением |

### `image` — `ImageMatchConfig`

| Поле | По умолчанию | Смысл |
|---|---|---|
| `phash_threshold` | 10 | расстояние Хэмминга из 64 |
| `phash256_threshold` | 40 | подтверждение на 256 битах |
| `dhash_threshold` | 16 | подтверждение градиентным хешем; 64 отключает |
| `color_threshold` | 24 | разница цветности 0–255; 255 отключает |
| `dihedral_invariant` | false | совпадение при поворотах и отражениях |
| `min_dimension` | 32 | меньшие изображения пропускаются |

Пара считается похожей, только если проходит все пороги сразу.

### `video` — `VideoMatchConfig`

| Поле | По умолчанию | Смысл |
|---|---|---|
| `frame_samples` | 16 | отсчётов на файл, ограничено `kMaxVideoFrames` = 32 |
| `edge_skip_fraction` | 0.05 | доля таймлайна, пропускаемая с каждого края |
| `duration_tolerance` | 0.02 | префильтр по длительности |
| `frame_threshold` | 8 | бит расхождения, при котором кадры ещё «те же» |
| `min_frame_match_ratio` | 0.65 | доля совпавших кадров |
| `min_frame_variance` | 12.0 | ниже — кадр считается пустым и пересемплируется |
| `subclip_detection` | false | искать ролик внутри более длинного |

### Результат и производительность

| Поле | По умолчанию | Смысл |
|---|---|---|
| `verify_bytes` | false | побайтовое подтверждение точных дубликатов |
| `cluster_mode` | `Strict` | `Transitive` даёт полноту ценой цепного склеивания |
| `keeper_policy` | `HighestResolution` | также `LargestFile`, `OldestModified`, `NewestModified`, `ShortestPath` |
| `concurrency.cpu_threads` | 0 | 0 — по `hardware_concurrency()` |
| `concurrency.io_threads` | 0 | 0 — по типу носителя первого корня |
| `cache.enabled` | false | кеш подписей в SQLite |
| `cache.path` | пусто | пусто — `ghidraengine-cache.db` в первом корне |
| `cache.prune_after_days` | 90 | удалять записи, не встреченные так долго |

## Report

```cpp
struct Report {
    std::vector<FileEntry> files;      // все файлы, дошедшие до сравнения
    std::vector<Cluster>   clusters;   // группы дубликатов, минимум 2 файла в каждой
    std::vector<FileError> errors;     // файлы, которые не удалось прочитать
    ScanStats              stats;
    bool                   cancelled;
    std::uint64_t total_reclaimable_bytes() const noexcept;
};
```

`Cluster::members` — индексы в `Report::files`; `keeper` — тоже индекс, а не позиция в
`members`. `distances` параллелен `members`: биты Хэмминга до keeper для похожих кластеров,
нули для точных. `total_reclaimable_bytes()` считает каждый файл один раз, даже если он
попал в несколько кластеров, — это не сумма `reclaimable_bytes` по кластерам.

`ScanStats` содержит `files_seen`, `files_considered`, `files_hashed`, `images_decoded`,
`videos_probed`, `cache_hits`, `bytes_read`, `hardlinks_collapsed`, `elapsed_seconds`.

## Отдельные операции

Если полный скан не нужен, доступны кирпичики:

```cpp
Result<Hash128> hash_file(const std::filesystem::path&);
Result<Hash128> hash_file_partial(const std::filesystem::path&, std::uint64_t size);

Result<ImageSignature> compute_image_signature(const std::filesystem::path&,
                                               const ImageMatchConfig& = {});
Result<VideoSignature> compute_video_signature(const std::filesystem::path&,
                                               const VideoMatchConfig& = {});
ImageSignature signature_from_thumbnail(std::span<const std::uint8_t> pixels,
                                        const ImageMatchConfig& = {});

std::uint32_t hamming_distance(std::uint64_t, std::uint64_t) noexcept;
std::uint32_t hamming_distance(const Hash256&, const Hash256&) noexcept;
bool   images_match(const ImageSignature&, const ImageSignature&, const ImageMatchConfig&) noexcept;
double video_similarity(const VideoSignature&, const VideoSignature&, const VideoMatchConfig&) noexcept;

MediaKind probe_media_kind(const std::filesystem::path&);
MediaKind probe_media_kind(std::span<const std::uint8_t> header) noexcept;

const char* version_string() noexcept;
const char* active_simd_backend() noexcept;
```

`signature_from_thumbnail` ожидает ровно `kThumbSize * kThumbSize` = 1024 байта в
построчном порядке — это точка входа, если декодированием занимается ваш код.

Пример — хранить подписи у себя и сравнивать при добавлении новой картинки:

```cpp
auto sig = ghidraengine::compute_image_signature(path);
if (sig) {
    for (const auto& known : my_library) {
        if (ghidraengine::images_match(*sig, known.signature, cfg.image)) { /* дубликат */ }
    }
}
```
