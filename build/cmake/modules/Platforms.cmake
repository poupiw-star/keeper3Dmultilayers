# Platforms.cmake - platform detection + directory-scope compile definitions.

if(WIN32)
    kfx_status("PLATFORM" "Windows / MinGW-w64 (i686)")
elseif(UNIX AND NOT APPLE)
    kfx_status("PLATFORM" "Linux (x86_64)")
elseif(APPLE)
    kfx_status("PLATFORM" "macOS (${CMAKE_SYSTEM_PROCESSOR})")
    # Third-party libraries come from Homebrew (see docs/macos.md). Apple clang
    # does not search the Homebrew prefix by default, and several formulas we
    # need are keg-only (not linked into the prefix), so point pkg-config and
    # the compiler/linker at them explicitly.
    find_program(KFX_BREW brew HINTS /opt/homebrew/bin /usr/local/bin)
    if(KFX_BREW)
        execute_process(COMMAND "${KFX_BREW}" --prefix
            OUTPUT_VARIABLE KFX_BREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
    elseif(EXISTS "/opt/homebrew")
        set(KFX_BREW_PREFIX "/opt/homebrew")
    else()
        message(FATAL_ERROR "Homebrew not found. Install it from https://brew.sh, then the packages listed in docs/macos.md")
    endif()
    kfx_status("HOMEBREW" "${KFX_BREW_PREFIX}")
    option(KFX_MACOS_WERROR "macOS: treat compiler warnings as errors" OFF)
    set(_kfx_pc "$ENV{PKG_CONFIG_PATH}")
    foreach(_keg openal-soft curl zlib)
        if(EXISTS "${KFX_BREW_PREFIX}/opt/${_keg}/lib/pkgconfig")
            set(_kfx_pc "${KFX_BREW_PREFIX}/opt/${_keg}/lib/pkgconfig:${_kfx_pc}")
        endif()
    endforeach()
    set(ENV{PKG_CONFIG_PATH} "${_kfx_pc}")
    include_directories(SYSTEM "${KFX_BREW_PREFIX}/include")
    link_directories("${KFX_BREW_PREFIX}/lib")
else()
    message(FATAL_ERROR "Unsupported platform (only Windows/MinGW, Linux and macOS are supported)")
endif()

add_compile_definitions("DEBUG=$<IF:$<CONFIG:Debug>,1,0>")

option(KEEPERFX_TRACY "Enable Tracy profiler instrumentation" OFF)

# Static-linkage defines for the prebuilt Windows dependencies.
if(WIN32)
    add_compile_definitions(_CRT_NONSTDC_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)
    # AL_LIBTYPE_STATIC tells <AL/al.h> not to mark its functions dllimport.
    # Needed when linking the kfx-deps static prebuilt (mirrors the Makefile);
    # not needed -- and not defined -- when vcpkg's openal-soft is in play,
    # which is linked dynamically. See Dependencies.cmake for why the two
    # differ.
    if(NOT VCPKG_TOOLCHAIN)
        add_compile_definitions(AL_LIBTYPE_STATIC)
    endif()
    add_compile_definitions(SPNG_STATIC=1)
    # MSVC's <math.h> only defines M_PI/M_E/etc. when this is set before the
    # first include; MinGW's <math.h> defines them unconditionally regardless,
    # so this is a no-op there. Directory-scope since math.h can be pulled in
    # transitively in an unpredictable order across source files.
    add_compile_definitions(_USE_MATH_DEFINES)
endif()
