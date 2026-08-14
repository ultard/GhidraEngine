# Биндинги: плоский C ABI

`include/ghidraengine/ghidraengine_c.h` — граница для любого языка, умеющего звать C.

Правила, на которые можно опираться:

- через границу не проходит ни одно исключение; каждый способный упасть вызов возвращает
  `ghidraengine_status`;
- все строки — UTF-8 с нулём на конце, память принадлежит библиотеке;
- указатели, полученные из аксессоров, живут, пока жив владеющий хендл;
- хендлы не потокобезопасны — по одному на поток (исключение: `ghidraengine_scanner_cancel`
  можно звать из другого потока во время скана).

## Жизненный цикл

```c
#include <ghidraengine/ghidraengine_c.h>

ghidraengine_config config;
ghidraengine_config_init(&config);       /* всегда: поля новых версий получат значения */
config.phash_threshold = 8;
config.cache_enabled = 1;

ghidraengine_scanner* scanner = NULL;
if (ghidraengine_scanner_create(&config, &scanner) != GHIDRAENGINE_OK) {
    return 1;
}

const char* roots[] = {"/home/user/Photos"};
ghidraengine_report* report = NULL;
ghidraengine_status status = ghidraengine_scanner_scan(scanner, roots, 1, &report);
if (status != GHIDRAENGINE_OK) {
    fprintf(stderr, "%s: %s\n", ghidraengine_status_message(status),
            ghidraengine_scanner_last_error(scanner));
    ghidraengine_scanner_free(scanner);
    return 1;
}

for (size_t c = 0; c < ghidraengine_report_cluster_count(report); ++c) {
    const uint32_t keeper = ghidraengine_report_cluster_keeper(report, c);
    printf("keep %s\n", ghidraengine_report_file_path(report, keeper));

    for (size_t m = 0; m < ghidraengine_report_cluster_member_count(report, c); ++m) {
        const uint32_t index = ghidraengine_report_cluster_member(report, c, m);
        if (index != keeper) {
            printf("  dup %s (distance %u)\n",
                   ghidraengine_report_file_path(report, index),
                   ghidraengine_report_cluster_member_distance(report, c, m));
        }
    }
}

ghidraengine_report_free(report);
ghidraengine_scanner_free(scanner);
```

`ghidraengine_config_init` обязателен: он заполняет структуру значениями по умолчанию, и
поля, добавленные в следующих версиях, не окажутся мусором.

Индексы членов кластера — это индексы в списке файлов отчёта, поэтому путь берётся через
`ghidraengine_report_file_path(report, index)`, а не через отдельный вызов у кластера.

## Прогресс и отмена

```c
static int on_progress(int phase, uint64_t processed, uint64_t total, void* user) {
    (void)phase; (void)total; (void)user;
    return processed > 100000 ? 1 : 0;   /* ненулевой ответ = отменить скан */
}

ghidraengine_scanner_set_progress(scanner, on_progress, NULL);
```

Колбэк вызывается из рабочих потоков. Второй способ — `ghidraengine_scanner_cancel(scanner)`
из другого потока. Отменённый скан возвращает `GHIDRAENGINE_OK` и отчёт, у которого
`ghidraengine_report_was_cancelled()` даёт 1: результаты частичные, но валидные.

## Что доступно

| Группа | Функции |
|---|---|
| Информация | `ghidraengine_version_string`, `ghidraengine_simd_backend`, `ghidraengine_status_message` |
| Конфигурация | `ghidraengine_config_init` |
| Сканер | `ghidraengine_scanner_create/free/scan/cancel/set_progress/last_error` |
| Файлы | `ghidraengine_report_file_count/path/size/mtime_ns/media_kind` |
| Кластеры | `ghidraengine_report_cluster_count/match_kind/media_kind/member_count/member/member_distance/keeper/reclaimable` |
| Ошибки | `ghidraengine_report_error_count/path/message/code` |
| Итоги | `ghidraengine_report_total_reclaimable`, `ghidraengine_report_stats`, `ghidraengine_report_was_cancelled` |
| Превью | `ghidraengine_video_preview`, `ghidraengine_preview_pixels/width/height/free` |
| Освобождение | `ghidraengine_report_free`, `ghidraengine_scanner_free` |

`ghidraengine_report_stats` принимает восемь выходных указателей, любой из них можно
передать как `NULL`.

Чего в C ABI нет: `exclude_patterns`, `probe_unknown_extensions` и колбэк ошибок по файлам.
Ошибки читаются из отчёта после скана; фильтрацию путей проще сделать на стороне
вызывающего языка.

## Превью видео

```c
ghidraengine_preview* preview = NULL;
/* 320 — предел длинной стороны, 0.25 — позиция в долях длительности */
if (ghidraengine_video_preview("/movies/a.mkv", 320, 0.25, &preview) == GHIDRAENGINE_OK) {
    const uint8_t* rgb = ghidraengine_preview_pixels(preview);   /* RGB24, stride = width * 3 */
    const uint32_t w = ghidraengine_preview_width(preview);
    const uint32_t h = ghidraengine_preview_height(preview);
    /* ... отдать в свой тулкит ... */
    ghidraengine_preview_free(preview);
}
```

Кадр берётся ближайший ключевой, изображение только уменьшается: для видео меньше
`max_size` вернётся исходный размер. Пропорции учитывают неквадратный пиксель, так что
`width/height` может не совпадать с `codecpar` анаморфного файла. Хендл не зависит от
сканера и живёт до `ghidraengine_preview_free`.

## Python (ctypes)

```python
import ctypes as C

lib = C.CDLL("./libghidraengine.so")   # ghidraengine.dll на Windows

class Config(C.Structure):
    _fields_ = [
        ("detect_exact", C.c_int), ("detect_similar", C.c_int),
        ("scan_images", C.c_int), ("scan_videos", C.c_int),
        ("min_file_size", C.c_uint64), ("max_file_size", C.c_uint64),
        ("follow_symlinks", C.c_int), ("skip_hidden", C.c_int), ("max_depth", C.c_uint32),
        ("phash_threshold", C.c_uint32), ("phash256_threshold", C.c_uint32),
        ("dhash_threshold", C.c_uint32), ("color_threshold", C.c_uint32),
        ("dihedral_invariant", C.c_int), ("min_dimension", C.c_uint32),
        ("video_frame_samples", C.c_uint32), ("video_edge_skip_fraction", C.c_double),
        ("video_duration_tolerance", C.c_double), ("video_frame_threshold", C.c_uint32),
        ("video_min_frame_match_ratio", C.c_double), ("video_min_frame_variance", C.c_double),
        ("video_subclip_detection", C.c_int),
        ("verify_bytes", C.c_int), ("cluster_mode", C.c_int), ("keeper_policy", C.c_int),
        ("cpu_threads", C.c_uint32), ("io_threads", C.c_uint32),
        ("cache_enabled", C.c_int), ("cache_path", C.c_char_p),
        ("cache_prune_after_days", C.c_uint32),
    ]

lib.ghidraengine_report_file_path.restype = C.c_char_p
lib.ghidraengine_scanner_last_error.restype = C.c_char_p

config = Config()
lib.ghidraengine_config_init(C.byref(config))
config.phash_threshold = 8

scanner = C.c_void_p()
if lib.ghidraengine_scanner_create(C.byref(config), C.byref(scanner)) != 0:
    raise RuntimeError("scanner_create failed")

roots = (C.c_char_p * 1)(b"/home/user/Photos")
report = C.c_void_p()
if lib.ghidraengine_scanner_scan(scanner, roots, 1, C.byref(report)) != 0:
    raise RuntimeError(lib.ghidraengine_scanner_last_error(scanner).decode())

for c in range(lib.ghidraengine_report_cluster_count(report)):
    keeper = lib.ghidraengine_report_cluster_keeper(report, c)
    print("keep", lib.ghidraengine_report_file_path(report, keeper).decode())

lib.ghidraengine_report_free(report)
lib.ghidraengine_scanner_free(scanner)
```

Порядок полей `Config` обязан совпадать с `ghidraengine_config` из заголовка — выравнивание
ctypes воспроизводит компиляторное. Если задаёте `cache_path`, держите `bytes`-объект живым,
пока жив сканер: структура хранит указатель, а не копию.

Функции, возвращающие указатель или 64-битное целое, требуют явного `restype` — иначе
ctypes усечёт результат до `int`.
