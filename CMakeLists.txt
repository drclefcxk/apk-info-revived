cmake_minimum_required(VERSION 3.21)
project(ApkInfo VERSION 1.1.1 LANGUAGES CXX)
enable_language(RC)   # app icon + version info (assets/icons/app.rc)

# ---------------------------------------------------------------------------
# Toolchain notes
#   * UI: Slint C++ 1.13 prebuilt (MSVC). Point at it with -DSlint_DIR=...
#   * zlib:  deflate for zip entries, Alpine v2 gzip streams, Alpine v3 ADBd
#   * libwebp: modern apk launcher icons are WebP, not PNG
#   * zstd:  Alpine v3 containers may use 'z' (zstd) compression
#   zlib/libwebp/zstd come from vcpkg (via the vcpkg toolchain file), the Slint
#   runtime is the prebuilt package, everything else is vendored in third_party.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Slint ships a prebuilt slint-compiler; without this hint CMake would try to
# download it at configure time.
if (NOT SLINT_COMPILER)
    foreach(candidate "D:/SDK/slint/bin/slint-compiler.exe" "$ENV{SLINT_DIR}/bin/slint-compiler.exe")
        if (EXISTS "${candidate}")
            set(SLINT_COMPILER "${candidate}" CACHE STRING "Path to the slint-compiler executable" FORCE)
            break()
        endif()
    endforeach()
endif()

find_package(Slint 1.13 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# clangd / compile_commands.json
#   With -DCMAKE_EXPORT_COMPILE_COMMANDS=ON CMake writes a compilation database
#   (the `ninja` preset does this). clangd can read it, but neither the MSVC STL
#   nor the Windows SDK include directories are target properties: cl.exe knows
#   them only through its INCLUDE environment variable, which clangd does not
#   inherit. Without them clangd reports <string>, <vector>, <windows.h>, ... as
#   missing and the database is useless. Hand them to clangd as external
#   includes (-external:I, which clang reads as -isystem; to cl.exe they are
#   just duplicates of what INCLUDE already provides).
# ---------------------------------------------------------------------------
if (MSVC AND CMAKE_EXPORT_COMPILE_COMMANDS)
    set(APKINFO_SYSTEM_INCLUDE_DIRS "")
    if (DEFINED ENV{INCLUDE} AND NOT "$ENV{INCLUDE}" STREQUAL "")
        # Configured from a Developer Command Prompt or by VS Code's CMake Tools
        # kit, which both export INCLUDE: use it verbatim.
        foreach(_dir IN ITEMS $ENV{INCLUDE})
            if (IS_DIRECTORY "${_dir}")
                list(APPEND APKINFO_SYSTEM_INCLUDE_DIRS "${_dir}")
            endif()
        endforeach()
    else()
        # Configured from a plain shell: rebuild the two roots from what CMake
        # already resolved (VC/Tools/MSVC/<ver>/bin/Host<x>/<arch>/cl.exe and the
        # Windows 10 SDK).
        get_filename_component(_vc "${CMAKE_CXX_COMPILER}" DIRECTORY)   # .../bin/Hostx64/x64
        get_filename_component(_vc "${_vc}" DIRECTORY)                  # .../bin/Hostx64
        get_filename_component(_vc "${_vc}" DIRECTORY)                  # .../bin
        get_filename_component(_vc "${_vc}" DIRECTORY)                  # .../MSVC/<ver>
        if (IS_DIRECTORY "${_vc}/include")
            list(APPEND APKINFO_SYSTEM_INCLUDE_DIRS "${_vc}/include")
        endif()

        set(_sdk_root "")
        if (DEFINED ENV{WindowsSdkDir} AND IS_DIRECTORY "$ENV{WindowsSdkDir}/Include")
            set(_sdk_root "$ENV{WindowsSdkDir}/Include")
        elseif (IS_DIRECTORY "C:/Program Files (x86)/Windows Kits/10/Include")
            set(_sdk_root "C:/Program Files (x86)/Windows Kits/10/Include")
        endif()
        if (_sdk_root)
            file(GLOB _sdk_versions RELATIVE "${_sdk_root}" "${_sdk_root}/10.*")
            if (_sdk_versions)
                list(SORT _sdk_versions COMPARE NATURAL ORDER DESCENDING)
                list(GET _sdk_versions 0 _sdk_version)
                foreach(_sub ucrt um shared winrt cppwinrt)
                    if (IS_DIRECTORY "${_sdk_root}/${_sdk_version}/${_sub}")
                        list(APPEND APKINFO_SYSTEM_INCLUDE_DIRS "${_sdk_root}/${_sdk_version}/${_sub}")
                    endif()
                endforeach()
            endif()
        endif()
    endif()

    if (NOT APKINFO_SYSTEM_INCLUDE_DIRS)
        message(WARNING "clangd will not find the MSVC/Windows SDK headers: no system include "
                        "directories were detected. Reconfigure from a Developer Command Prompt.")
    endif()
    # CXX only: the RC compiler must not see these.
    foreach(_dir IN LISTS APKINFO_SYSTEM_INCLUDE_DIRS)
        add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-external:I${_dir}>")
    endforeach()
    message(STATUS "clangd support: ${CMAKE_EXPORT_COMPILE_COMMANDS} "
                   "(${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json, "
                   "${APKINFO_SYSTEM_INCLUDE_DIRS})")
endif()

set(APKINFO_CORE_SOURCES
    src/container/file_map.cpp
    src/container/zip_file.cpp
    src/model/info_document.cpp
    src/model/tree_builder.cpp
    src/model/report.cpp
    src/detect.cpp
    src/i18n.cpp
    src/app.cpp
    src/util/text.cpp
    src/util/json.cpp
    src/config.cpp
    src/util/clipboard.cpp
    src/util/hash.cpp
    src/util/png_writer.cpp
    src/image/vector.cpp
    src/image/decode.cpp
    src/image/stb_impl.cpp
    src/android/axml.cpp
    src/android/arsc.cpp
    src/android/versions.cpp
    src/android/apk_reader.cpp
    src/android/adb.cpp
    src/install.cpp
    src/util/process.cpp
    src/alpine/alpine.cpp
    src/alpine/alpine_reader.cpp
)

add_library(apkinfo_core STATIC ${APKINFO_CORE_SOURCES})
target_include_directories(apkinfo_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
)
target_compile_definitions(apkinfo_core PRIVATE
    APKINFO_HAVE_ZSTD=1
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
target_link_libraries(apkinfo_core PUBLIC
    ZLIB::ZLIB
    WebP::webp
    zstd::libzstd
    bcrypt
    shell32
    ole32
    uuid
)
if (MSVC)
    target_compile_options(apkinfo_core PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
endif()

# Copy the runtime DLLs (Slint + vcpkg ports) next to whatever we build.
# The prebuilt Slint package puts its runtime in <prefix>/lib/slint_cpp.dll;
# find it by walking up from the CMake package directory instead of assuming a
# fixed depth.
set(SLINT_RUNTIME_DLL "")
set(_slint_probe "${Slint_DIR}")
foreach(_i RANGE 1 5)
    if (EXISTS "${_slint_probe}/lib/slint_cpp.dll")
        set(SLINT_RUNTIME_DLL "${_slint_probe}/lib/slint_cpp.dll")
        break()
    endif()
    get_filename_component(_slint_probe "${_slint_probe}" DIRECTORY)
endforeach()
if (NOT SLINT_RUNTIME_DLL)
    message(WARNING "slint_cpp.dll not found near ${Slint_DIR}; copy it next to the executable manually")
endif()

function(apkinfo_deploy_runtime target)
    if (SLINT_RUNTIME_DLL)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SLINT_RUNTIME_DLL}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${target}> "$<TARGET_FILE_DIR:${target}>"
        COMMAND_EXPAND_LISTS
        VERBATIM)
endfunction()

# --- GUI application --------------------------------------------------------
add_executable(ApkInfo WIN32 src/main.cpp assets/icons/app.rc)
target_compile_definitions(ApkInfo PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if (MSVC)
    target_compile_options(ApkInfo PRIVATE /utf-8)
endif()
slint_target_sources(ApkInfo ui/main.slint)
target_link_libraries(ApkInfo PRIVATE apkinfo_core Slint::Slint)
apkinfo_deploy_runtime(ApkInfo)

# Loose .ico files next to the exe: the shell uses them via DefaultIcon, and the
# file-association script copies them somewhere stable.
add_custom_command(TARGET ApkInfo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/app.ico"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/apk.ico"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/apks.ico"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/android.ico"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/alpine.ico"
            "$<TARGET_FILE_DIR:ApkInfo>"
    VERBATIM)

# --- console test driver ----------------------------------------------------
add_executable(apkinfo-cli tools/cli.cpp)
target_compile_definitions(apkinfo-cli PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if (MSVC)
    target_compile_options(apkinfo-cli PRIVATE /utf-8)
endif()
target_link_libraries(apkinfo-cli PRIVATE apkinfo_core)
apkinfo_deploy_runtime(apkinfo-cli)
