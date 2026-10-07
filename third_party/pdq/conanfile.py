import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain
from conan.tools.files import copy, download


class PdqReference(ConanFile):
    name = "pdq"
    version = "cci.20261001"
    license = "BSD-3-Clause"
    url = "https://github.com/facebook/ThreatExchange"
    description = "Unmodified reference PDQ hashing core, without decoding or indexes"
    package_type = "static-library"
    settings = "os", "compiler", "build_type", "arch"
    options = {"fPIC": [True, False]}
    default_options = {"fPIC": True}
    exports_sources = "CMakeLists.txt"

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def source(self):
        source = self.conan_data["sources"][str(self.version)]
        for path, checksum in source["files"].items():
            destination = os.path.join(self.source_folder, path)
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            download(self, f"https://raw.githubusercontent.com/facebook/ThreatExchange/"
                     f"{source['commit']}/{path}", destination, sha256=checksum)

    def generate(self):
        CMakeToolchain(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        CMake(self).install()
        copy(self, "LICENSE", self.source_folder, os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        self.cpp_info.libs = ["pdq"]
        self.cpp_info.set_property("cmake_file_name", "pdq")
        self.cpp_info.set_property("cmake_target_name", "PDQ::pdq")
