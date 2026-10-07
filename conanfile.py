from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain


class GhidraEngineDependencies(ConanFile):
    settings = "os", "compiler", "build_type", "arch"

    default_options = {
        # BLAKE3
        "blake3/*:shared": False,
        "blake3/*:with_tbb": False,

        # libvips
        "libvips/*:shared": True,
        "libvips/*:cpp": True,
        "libvips/*:deprecated": False,
        "libvips/*:with_exif": True,
        "libvips/*:with_fftw": False,
        "libvips/*:with_jpeg": "libjpeg-turbo",
        "libvips/*:with_png": "libpng",
        "libvips/*:with_analyse": False,
        "libvips/*:with_radiance": True,

        # GLib
        "glib/*:shared": True,
        "glib/*:with_elf": False,
        "glib/*:with_mount": False,
        "glib/*:with_selinux": False,

        # libtiff
        "libtiff/*:jpeg": "libjpeg-turbo",

        # FFmpeg
        "ffmpeg/*:shared": True,
        "ffmpeg/*:disable_all_encoders": True,
        "ffmpeg/*:disable_all_muxers": True,
        "ffmpeg/*:disable_all_hardware_accelerators": True,
        "ffmpeg/*:disable_all_protocols": True,
        "ffmpeg/*:enable_protocols": "file",

        **{
            f"ffmpeg/*:{option}": False
            for option in (
                "avdevice",
                "avfilter",
                "swresample",
                "with_programs",
                "with_bzip2",
                "with_lzma",
                "with_libiconv",
                "with_freetype",
                "with_openjpeg",
                "with_openh264",
                "with_opus",
                "with_vorbis",
                "with_libx264",
                "with_libx265",
                "with_libvpx",
                "with_libmp3lame",
                "with_libfdk_aac",
                "with_libwebp",
                "with_ssl",
                "with_libalsa",
                "with_pulse",
                "with_vaapi",
                "with_vdpau",
                "with_xcb",
                "with_xlib",
                "with_appkit",
                "with_avfoundation",
                "with_coreimage",
                "with_audiotoolbox",
                "with_videotoolbox",
                "with_libsvtav1",
                "with_libaom",
                "with_libdav1d",
            )
        },
    }

    def requirements(self):
        self.requires("blake3/1.8.5")
        self.requires("pdq/cci.20261001")
        self.requires("libvips/8.16.0")
        self.requires("ffmpeg/8.0")

    def build_requirements(self):
        if self.conf.get("user.ghidraengine:tests", default=False, check_type=bool):
            self.test_requires("gtest/1.18.0")

        if self.conf.get("user.ghidraengine:benchmarks", default=False, check_type=bool):
            self.test_requires("benchmark/1.9.5")

    def generate(self):
        CMakeDeps(self).generate()

        tc = CMakeToolchain(self)
        tc.user_presets_path = False

        tc.cache_variables["BUILD_TESTING"] = self.conf.get(
            "user.ghidraengine:tests",
            default=False,
            check_type=bool,
        )

        tc.cache_variables["GHIDRAENGINE_BUILD_BENCHMARKS"] = self.conf.get(
            "user.ghidraengine:benchmarks",
            default=False,
            check_type=bool,
        )

        tc.generate()
