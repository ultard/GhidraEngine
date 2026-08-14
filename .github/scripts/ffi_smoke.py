#!/usr/bin/env python3
"""Load the installed shared library the way a binding would and call the C ABI.

Proves the artifact is loadable (all transitive imports resolve) and that its
flat C symbols are exported. Behaviour is covered by the C++ test suite; this
only guards the packaging and the export surface.
"""

import ctypes
import sys
from pathlib import Path

prefix = Path(sys.argv[1] if len(sys.argv) > 1 else "dist")

candidates = [p for pattern in ("bin/ghidraengine.dll", "lib/libghidraengine.so*",
                                "lib/libghidraengine*.dylib")
              for p in prefix.glob(pattern) if not p.is_symlink()]
if not candidates:
    sys.exit(f"no shared library under {prefix}: {sorted(p.name for p in prefix.rglob('*'))}")

lib = ctypes.CDLL(str(candidates[0]))
print(f"loaded {candidates[0]}")

lib.ghidraengine_version_string.restype = ctypes.c_char_p
lib.ghidraengine_simd_backend.restype = ctypes.c_char_p
lib.ghidraengine_status_message.restype = ctypes.c_char_p
lib.ghidraengine_status_message.argtypes = [ctypes.c_int]

version = lib.ghidraengine_version_string().decode()
print(f"version {version}, simd {lib.ghidraengine_simd_backend().decode()}")
assert version.count(".") == 2, version

# Oversized so the layout of ghidraengine_config never has to be mirrored here.
config = ctypes.create_string_buffer(4096)
lib.ghidraengine_config_init(config)
assert config.raw != bytes(4096), "config_init left the struct untouched"

scanner = ctypes.c_void_p()
status = lib.ghidraengine_scanner_create(config, ctypes.byref(scanner))
assert status == 0, lib.ghidraengine_status_message(status).decode()
assert scanner.value, "scanner_create returned OK but no handle"
lib.ghidraengine_scanner_free(scanner)

print("C ABI smoke test passed")
