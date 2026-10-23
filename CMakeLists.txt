cmake_minimum_required(VERSION 4.4)

project(raw2raw2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The DNG SDK and XMP Toolkit only support Microsoft's compiler on Windows
if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR "raw2raw2 must be built with MSVC on Windows. "
                        "Install Visual Studio (or its Build Tools) with the C++ workload.")
endif()

# Compile each target's source files in parallel
if(CMAKE_GENERATOR MATCHES "Visual Studio")
    add_compile_options(/MP)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(DNG_ROOT ${THIRD_PARTY}/dng_sdk_1_7_1)
set(XMP_ROOT ${DNG_ROOT}/xmp/toolkit)

# Adobe tests the DNG SDK and XMP Toolkit with C++14/17, so third-party code is
# built as C++17 and warnings are silenced; our own code stays C++23.
set(THIRD_PARTY_CXX_STANDARD 17)
set(THIRD_PARTY_NO_WARNINGS /W0)

# ============================================================
# zlib
# ============================================================

set(ZLIB_ROOT_DIR ${THIRD_PARTY}/zlib-1.3.2)
add_library(dng_zlib STATIC
    ${ZLIB_ROOT_DIR}/adler32.c
    ${ZLIB_ROOT_DIR}/compress.c
    ${ZLIB_ROOT_DIR}/crc32.c
    ${ZLIB_ROOT_DIR}/deflate.c
    ${ZLIB_ROOT_DIR}/gzclose.c
    ${ZLIB_ROOT_DIR}/gzlib.c
    ${ZLIB_ROOT_DIR}/gzread.c
    ${ZLIB_ROOT_DIR}/gzwrite.c
    ${ZLIB_ROOT_DIR}/infback.c
    ${ZLIB_ROOT_DIR}/inffast.c
    ${ZLIB_ROOT_DIR}/inflate.c
    ${ZLIB_ROOT_DIR}/inftrees.c
    ${ZLIB_ROOT_DIR}/trees.c
    ${ZLIB_ROOT_DIR}/uncompr.c
    ${ZLIB_ROOT_DIR}/zutil.c
)
target_include_directories(dng_zlib PUBLIC ${ZLIB_ROOT_DIR})
target_compile_options(dng_zlib PRIVATE ${THIRD_PARTY_NO_WARNINGS})

# ============================================================
# expat (XML parser used by XMPCore)
# ============================================================

set(EXPAT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_PKGCONFIG OFF CACHE BOOL "" FORCE)
set(EXPAT_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(EXPAT_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY}/expat-2.9.0 ${CMAKE_BINARY_DIR}/third_party/expat EXCLUDE_FROM_ALL)

# XMPCore includes expat as "XMPCore/third-party/expat/public/lib/expat.h". Rather
# than keeping a second copy of the headers in the SDK tree, mirror them into the
# build directory at that path.
set(XMP_EXPAT_SHIM ${CMAKE_BINARY_DIR}/xmp_expat_shim)
foreach(header expat.h expat_external.h)
    configure_file(${THIRD_PARTY}/expat-2.9.0/lib/${header}
                   ${XMP_EXPAT_SHIM}/XMPCore/third-party/expat/public/lib/${header} COPYONLY)
endforeach()

# ============================================================
# libjpeg
# ============================================================

set(JPEG_DIR ${DNG_ROOT}/libjpeg)
add_library(dng_jpeg STATIC
    ${JPEG_DIR}/jaricom.c  ${JPEG_DIR}/jcapimin.c ${JPEG_DIR}/jcapistd.c ${JPEG_DIR}/jcarith.c
    ${JPEG_DIR}/jccoefct.c ${JPEG_DIR}/jccolor.c  ${JPEG_DIR}/jcdctmgr.c ${JPEG_DIR}/jchuff.c
    ${JPEG_DIR}/jcinit.c   ${JPEG_DIR}/jcmainct.c ${JPEG_DIR}/jcmarker.c ${JPEG_DIR}/jcmaster.c
    ${JPEG_DIR}/jcomapi.c  ${JPEG_DIR}/jcparam.c  ${JPEG_DIR}/jcprepct.c ${JPEG_DIR}/jcsample.c
    ${JPEG_DIR}/jctrans.c  ${JPEG_DIR}/jdapimin.c ${JPEG_DIR}/jdapistd.c ${JPEG_DIR}/jdarith.c
    ${JPEG_DIR}/jdatadst.c ${JPEG_DIR}/jdatasrc.c ${JPEG_DIR}/jdcoefct.c ${JPEG_DIR}/jdcolor.c
    ${JPEG_DIR}/jddctmgr.c ${JPEG_DIR}/jdhuff.c   ${JPEG_DIR}/jdinput.c  ${JPEG_DIR}/jdmainct.c
    ${JPEG_DIR}/jdmarker.c ${JPEG_DIR}/jdmaster.c ${JPEG_DIR}/jdmerge.c  ${JPEG_DIR}/jdpostct.c
    ${JPEG_DIR}/jdsample.c ${JPEG_DIR}/jdtrans.c  ${JPEG_DIR}/jerror.c   ${JPEG_DIR}/jfdctflt.c
    ${JPEG_DIR}/jfdctfst.c ${JPEG_DIR}/jfdctint.c ${JPEG_DIR}/jidctflt.c ${JPEG_DIR}/jidctfst.c
    ${JPEG_DIR}/jidctint.c ${JPEG_DIR}/jmemmgr.c  ${JPEG_DIR}/jmemnobs.c ${JPEG_DIR}/jquant1.c
    ${JPEG_DIR}/jquant2.c  ${JPEG_DIR}/jutils.c
)
target_include_directories(dng_jpeg PUBLIC ${JPEG_DIR})
target_compile_options(dng_jpeg PRIVATE ${THIRD_PARTY_NO_WARNINGS})

# ============================================================
# libjxl (JPEG XL, with bundled brotli and highway)
# ============================================================

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_JPEGLI OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_JPEGLI_LIBJPEG OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_DOXYGEN OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_MANPAGES OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_BENCHMARK OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_JNI OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_SJPEG OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_OPENEXR OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_FUZZERS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_TCMALLOC OFF CACHE BOOL "" FORCE)
set(JPEGXL_BUNDLE_LIBPNG OFF CACHE BOOL "" FORCE)
set(JPEGXL_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# The bundled brotli declares cmake_minimum_required(VERSION < 3.5), which CMake 4 rejects
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(${DNG_ROOT}/libjxl/libjxl ${CMAKE_BINARY_DIR}/third_party/libjxl EXCLUDE_FROM_ALL)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# ============================================================
# XMPCore (Adobe XMP Toolkit)
# ============================================================

add_library(dng_xmpcore STATIC
    ${XMP_ROOT}/XMPCore/source/WXMPDocOps.cpp
    ${XMP_ROOT}/XMPCore/source/WXMPIterator.cpp
    ${XMP_ROOT}/XMPCore/source/WXMPMeta.cpp
    ${XMP_ROOT}/XMPCore/source/WXMPUtils.cpp
    ${XMP_ROOT}/XMPCore/source/XMPDocOps-Utils.cpp
    ${XMP_ROOT}/XMPCore/source/XMPDocOps-Utils2.cpp
    ${XMP_ROOT}/XMPCore/source/XMPDocOps.cpp
    ${XMP_ROOT}/XMPCore/source/XMPDocOps2.cpp
    ${XMP_ROOT}/XMPCore/source/XMPIterator.cpp
    ${XMP_ROOT}/XMPCore/source/XMPIterator2.cpp
    ${XMP_ROOT}/XMPCore/source/XMPMeta-GetSet.cpp
    ${XMP_ROOT}/XMPCore/source/XMPMeta-Parse.cpp
    ${XMP_ROOT}/XMPCore/source/XMPMeta-Serialize.cpp
    ${XMP_ROOT}/XMPCore/source/XMPMeta.cpp
    ${XMP_ROOT}/XMPCore/source/XMPMeta2-GetSet.cpp
    ${XMP_ROOT}/XMPCore/source/XMPUtils-FileInfo.cpp
    ${XMP_ROOT}/XMPCore/source/XMPUtils.cpp
    ${XMP_ROOT}/XMPCore/source/XMPUtils2.cpp
    ${XMP_ROOT}/XMPCore/source/ExpatAdapter.cpp
    ${XMP_ROOT}/XMPCore/source/ParseRDF.cpp
    ${XMP_ROOT}/XMPCore/source/XMPCore_Impl.cpp
    ${XMP_ROOT}/XMPCore/source/CoreObjectFactoryImpl.cpp
    ${XMP_ROOT}/third-party/zuid/sources/MD5.cpp
    ${XMP_ROOT}/third-party/zuid/sources/ZUIDSysDep.cpp
    ${XMP_ROOT}/third-party/zuid/sources/ZUIDUUID.cpp
    ${XMP_ROOT}/source/UnicodeConversions.cpp
    ${XMP_ROOT}/source/XML_Node.cpp
    ${XMP_ROOT}/source/XMP_LibUtils.cpp
)
target_include_directories(dng_xmpcore
    PRIVATE
        ${XMP_EXPAT_SHIM}
        ${XMP_ROOT}/XMPCore/resource/win
        ${XMP_ROOT}/third-party/zuid/interfaces
        ${XMP_ROOT}/build
    PUBLIC
        ${XMP_ROOT}
        ${XMP_ROOT}/public/include
)
target_compile_definitions(dng_xmpcore
    PRIVATE
        AdobePrivate=1
        ENABLE_CPP_DOM_MODEL=0
        BUILDING_XMPCORE_LIB=1
        BUILDING_XMPCORE_AS_STATIC=1
        XMP_COMPONENT_INT_NAMESPACE=AdobeXMPCore_Int
        XML_STATIC=1
        NOMINMAX
        UNICODE
        _UNICODE
        _CRT_SECURE_NO_WARNINGS=1
        _SCL_SECURE_NO_WARNINGS=1
    PUBLIC
        WIN_ENV=1
        XMP_StaticBuild=1
        XMP_64=1
)
target_compile_options(dng_xmpcore PRIVATE ${THIRD_PARTY_NO_WARNINGS})
target_link_libraries(dng_xmpcore PRIVATE expat dng_zlib)
set_target_properties(dng_xmpcore PROPERTIES CXX_STANDARD ${THIRD_PARTY_CXX_STANDARD})

# ============================================================
# DNG SDK
# ============================================================

set(DNG_SRC ${DNG_ROOT}/dng_sdk/source)
add_library(dng_sdk STATIC
    ${DNG_SRC}/dng_1d_function.cpp      ${DNG_SRC}/dng_1d_table.cpp
    ${DNG_SRC}/dng_abort_sniffer.cpp    ${DNG_SRC}/dng_area_task.cpp
    ${DNG_SRC}/dng_bad_pixels.cpp       ${DNG_SRC}/dng_big_table.cpp
    ${DNG_SRC}/dng_bottlenecks.cpp      ${DNG_SRC}/dng_camera_profile.cpp
    ${DNG_SRC}/dng_color_space.cpp      ${DNG_SRC}/dng_color_spec.cpp
    ${DNG_SRC}/dng_date_time.cpp        ${DNG_SRC}/dng_exceptions.cpp
    ${DNG_SRC}/dng_exif.cpp             ${DNG_SRC}/dng_file_stream.cpp
    ${DNG_SRC}/dng_filter_task.cpp      ${DNG_SRC}/dng_fingerprint.cpp
    ${DNG_SRC}/dng_gain_map.cpp         ${DNG_SRC}/dng_globals.cpp
    ${DNG_SRC}/dng_host.cpp             ${DNG_SRC}/dng_hue_sat_map.cpp
    ${DNG_SRC}/dng_ifd.cpp              ${DNG_SRC}/dng_image.cpp
    ${DNG_SRC}/dng_image_writer.cpp     ${DNG_SRC}/dng_info.cpp
    ${DNG_SRC}/dng_iptc.cpp             ${DNG_SRC}/dng_jpeg_image.cpp
    ${DNG_SRC}/dng_jpeg_memory_source.cpp ${DNG_SRC}/dng_jxl.cpp
    ${DNG_SRC}/dng_lens_correction.cpp  ${DNG_SRC}/dng_linearization_info.cpp
    ${DNG_SRC}/dng_local_string.cpp     ${DNG_SRC}/dng_lossless_jpeg.cpp
    ${DNG_SRC}/dng_matrix.cpp           ${DNG_SRC}/dng_memory.cpp
    ${DNG_SRC}/dng_memory_stream.cpp    ${DNG_SRC}/dng_misc_opcodes.cpp
    ${DNG_SRC}/dng_mosaic_info.cpp      ${DNG_SRC}/dng_mutex.cpp
    ${DNG_SRC}/dng_negative.cpp         ${DNG_SRC}/dng_opcodes.cpp
    ${DNG_SRC}/dng_opcode_list.cpp      ${DNG_SRC}/dng_orientation.cpp
    ${DNG_SRC}/dng_parse_utils.cpp      ${DNG_SRC}/dng_pixel_buffer.cpp
    ${DNG_SRC}/dng_point.cpp            ${DNG_SRC}/dng_preview.cpp
    ${DNG_SRC}/dng_pthread.cpp          ${DNG_SRC}/dng_rational.cpp
    ${DNG_SRC}/dng_read_image.cpp       ${DNG_SRC}/dng_rect.cpp
    ${DNG_SRC}/dng_reference.cpp        ${DNG_SRC}/dng_ref_counted_block.cpp
    ${DNG_SRC}/dng_render.cpp           ${DNG_SRC}/dng_resample.cpp
    ${DNG_SRC}/dng_safe_arithmetic.cpp  ${DNG_SRC}/dng_shared.cpp
    ${DNG_SRC}/dng_simple_image.cpp     ${DNG_SRC}/dng_spline.cpp
    ${DNG_SRC}/dng_stream.cpp           ${DNG_SRC}/dng_string.cpp
    ${DNG_SRC}/dng_string_list.cpp      ${DNG_SRC}/dng_tag_types.cpp
    ${DNG_SRC}/dng_temperature.cpp      ${DNG_SRC}/dng_tile_iterator.cpp
    ${DNG_SRC}/dng_tone_curve.cpp       ${DNG_SRC}/dng_utils.cpp
    ${DNG_SRC}/dng_xmp.cpp              ${DNG_SRC}/dng_xmp_sdk.cpp
    ${DNG_SRC}/dng_xy_coord.cpp
)
target_include_directories(dng_sdk PUBLIC ${DNG_SRC})
target_compile_definitions(dng_sdk
    PUBLIC
        qWinOS=1
        qMacOS=0
        qDNGValidate=0
        qDNGXMPFiles=0
        qDNGXMPDocOps=0
        qDNGUseLibJPEG=1
        $<IF:$<CONFIG:Debug>,qDNGDebug=1,qDNGDebug=0>
    PRIVATE
        _CRT_SECURE_NO_DEPRECATE
        NOMINMAX
)
target_compile_options(dng_sdk PRIVATE ${THIRD_PARTY_NO_WARNINGS})
target_link_libraries(dng_sdk
    PUBLIC
        dng_xmpcore
        jxl
        jxl_threads
    PRIVATE
        dng_jpeg
        dng_zlib
)
set_target_properties(dng_sdk PROPERTIES CXX_STANDARD ${THIRD_PARTY_CXX_STANDARD})

# ============================================================
# SDL3 (window, input, renderer, native file dialogs)
# ============================================================

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY}/SDL3-3.4.18 ${CMAKE_BINARY_DIR}/third_party/SDL3 EXCLUDE_FROM_ALL)

# ============================================================
# Dear ImGui (docking branch) with the SDL3 + SDL_Renderer backends
# ============================================================

set(IMGUI_DIR ${THIRD_PARTY}/imgui-1.93.0-docking)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)
target_compile_options(imgui PRIVATE ${THIRD_PARTY_NO_WARNINGS})

# ============================================================
# raw2raw2
# ============================================================

# Every .cpp under src/ (except src/tools/, which are separate programs) is part of the
# app, so a new feature (src/features/) only needs its own files. CONFIGURE_DEPENDS makes
# the build re-glob when files are added.
file(GLOB_RECURSE RAW2RAW2_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(FILTER RAW2RAW2_SOURCES EXCLUDE REGEX "/src/tools/")
add_executable(raw2raw2 WIN32 ${RAW2RAW2_SOURCES})
set(JSON_DIR ${THIRD_PARTY}/nlohmann_json-3.11.3)
target_include_directories(raw2raw2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${JSON_DIR})
# The Raw Processor's median keeps every frame in memory; this caps how many it accepts.
set(RAW2RAW2_MAX_STACK_FRAMES 100 CACHE STRING "Most frames the Raw Processor combines at once")
target_compile_definitions(raw2raw2 PRIVATE R2R_MAX_STACK_FRAMES=${RAW2RAW2_MAX_STACK_FRAMES})
# zlib decodes ZIP-compressed TIFFs; Media Foundation (part of Windows) encodes video;
# WinHTTP talks to the export servers.
target_link_libraries(raw2raw2 PRIVATE dng_sdk imgui dng_zlib mfplat mfreadwrite mfuuid ole32 winhttp)
# C4310 (constant truncated by a cast) fires on the DNG SDK's own constants in its headers.
target_compile_options(raw2raw2 PRIVATE /W4 /utf-8 /wd4310)

# ============================================================
# Export tools: console programs in src/tools/<name>/ that drive another application
# (Photoshop, LightCraft) over a local HTTP service. They share src/tools/common/.
# ============================================================

file(GLOB TOOLS_COMMON_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/common/*.cpp)

function(add_export_tool name dir)
    file(GLOB tool_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/${dir}/*.cpp)
    add_executable(${name} ${tool_sources} ${TOOLS_COMMON_SOURCES})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${JSON_DIR})
    target_compile_options(${name} PRIVATE /W4 /utf-8)
    target_compile_definitions(${name} PRIVATE UNICODE _UNICODE)
    target_link_libraries(${name} PRIVATE ws2_32 ${ARGN})
endfunction()

add_export_tool(r2r-photoshop-export photoshop_export ole32 oleaut32)
add_export_tool(r2r-lightcraft-export lightcraft_export)

# The exposure-match server meters raw files itself, so it carries the DNG SDK.
add_export_tool(r2r-exposure-match exposure_match dng_sdk)
target_sources(r2r-exposure-match PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/io/dng_loader.cpp
                                          ${CMAKE_CURRENT_SOURCE_DIR}/src/processing/metering.cpp)
target_compile_options(r2r-exposure-match PRIVATE /wd4310)
