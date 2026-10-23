# Third-party sources

Everything here is built by the top-level `CMakeLists.txt`; nothing is downloaded at build time.

| Directory | Version | Source |
|---|---|---|
| `dng_sdk_1_7_1/` | Adobe DNG SDK 1.7.1 | https://download.adobe.com/pub/adobe/dng/dng_sdk_1_7_1.zip (SHA-256 `5366d9abd0e623858573bfaf0291b5e352d704f51b322753a22f82af3ae6e0c2`) |
| `expat-2.9.0/` | libexpat 2.9.0 | https://github.com/libexpat/libexpat/releases/download/R_2_9_0/expat-2.9.0.tar.xz (SHA-256 `1e6371862cc31999b368c3b89b49994f0677e1bab5f1b2b85ae3741f5d803051`) |
| `zlib-1.3.2/` | zlib 1.3.2 | https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz (SHA-256 `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16`) |
| `SDL3-3.4.18/` | SDL 3.4.18 | https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz (SHA-256 `9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3`) |
| `imgui-1.93.0-docking/` | Dear ImGui 1.93.0 (docking branch) | https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0-docking.tar.gz (SHA-256 `746c265540bd3aa109570f29492ef4d8f5b421dd9fd0049c35ca35f21496f1a5`) |
| `nlohmann_json-3.11.3/` | JSON for Modern C++ 3.11.3 (single header, MIT) | https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp (SHA-256 `9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6`) |

The DNG SDK zip also contains libjpeg, libjxl (with brotli and highway) and the
Adobe XMP Toolkit; those are used as shipped.

## Changes from the original DNG SDK zip

- Removed `sample_files/` (66 MB of example DNGs, not needed to build).
- Removed `xmp/toolkit/XMPCore/third-party/expat/public/` (bundled expat 2.2.9 and
  older tarballs) and `xmp/toolkit/third-party/zlib/` (zlib 1.2.11). Both versions
  have known security vulnerabilities; the newer releases above replace them.

No Adobe source files have been modified.

## Changes from the original SDL and Dear ImGui archives

- SDL is unmodified.
- Removed `examples/` from Dear ImGui (sample apps for every backend, not needed to build).
