cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg toolchain file")
    elseif(WIN32)
        find_program(VSWHERE_EXECUTABLE vswhere
            PATHS "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer"
            NO_DEFAULT_PATH
        )
        if(VSWHERE_EXECUTABLE)
            execute_process(
                COMMAND "${VSWHERE_EXECUTABLE}" -latest -property installationPath
                OUTPUT_VARIABLE VS_INSTALL_PATH
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            if(EXISTS "${VS_INSTALL_PATH}/VC/vcpkg/scripts/buildsystems/vcpkg.cmake")
                set(CMAKE_TOOLCHAIN_FILE "${VS_INSTALL_PATH}/VC/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg toolchain file")
            endif()
        endif()
    endif()
endif()

project(eevs VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(EEVS_BUILD_PLUGINS "Build built-in EEVS shared-library plugins" ON)
option(EEVS_BUILD_BENCHMARK "Build EEVS benchmark tool (eevs_bench)" ON)
option(EEVS_BUILD_TESTS "Build EEVS unit and integration test suite (eevs_tests)" ON)
option(EEVS_BUILD_DOCS "Configure Doxygen documentation target (docs)" ON)
option(EEVS_ENABLE_AVX2 "Enable AVX2 + FMA3 SIMD and fast-math optimizations" ON)
option(EEVS_ENABLE_TRACY "Enable Tracy real-time profiler instrumentation (TRACY_ON_DEMAND)" OFF)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

if(EEVS_ENABLE_TRACY)
    find_package(Tracy CONFIG QUIET)
    add_compile_definitions(EEVS_ENABLE_TRACY=1 TRACY_ENABLE=1 TRACY_ON_DEMAND=1)
endif()

if(EEVS_ENABLE_AVX2)
    if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # clang-cl on Windows
        add_compile_options(
            /arch:AVX2 /fp:fast /Oi /Ot
            /clang:-march=x86-64-v3
            /clang:-fstrict-vtable-pointers
            /clang:-ffp-contract=fast
            /clang:-fno-math-errno
            /clang:-fno-trapping-math
        )
    elseif(MSVC)
        # MSVC cl.exe on Windows
        add_compile_options(/arch:AVX2 /fp:fast /Oi /Ot)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # clang++ on Linux / POSIX
        add_compile_options(
            -march=x86-64-v3
            -mavx2
            -mfma
            -ffast-math
            -ffp-contract=fast
            -fno-math-errno
            -fno-trapping-math
            -fno-plt
            -fstrict-vtable-pointers
            -fno-semantic-interposition
            -fomit-frame-pointer
            -Wno-deprecated-enum-enum-conversion
        )
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # g++ on Linux / POSIX
        add_compile_options(
            -march=x86-64-v3
            -mavx2
            -mfma
            -ffast-math
            -fno-plt
            -fno-semantic-interposition
            -fomit-frame-pointer
        )
    endif()
endif()

find_package(glaze CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Opus CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(quill CONFIG REQUIRED)
find_package(unofficial-uwebsockets CONFIG REQUIRED)

if(EEVS_BUILD_BENCHMARK)
    find_package(CLI11 CONFIG REQUIRED)
    find_package(ftxui CONFIG REQUIRED)
    find_package(hdr_histogram CONFIG REQUIRED)
endif()

if(EEVS_BUILD_TESTS)
    enable_testing()
    find_package(doctest CONFIG REQUIRED)
endif()

add_subdirectory(src)

if(EEVS_BUILD_PLUGINS)
    add_subdirectory(plugins)
endif()

if(EEVS_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(EEVS_BUILD_DOCS)
    find_package(Doxygen QUIET OPTIONAL_COMPONENTS dot)
    if(DOXYGEN_FOUND)
        if(DOXYGEN_DOT_FOUND)
            set(DOXYGEN_DOT_FOUND "YES")
        else()
            set(DOXYGEN_DOT_FOUND "NO")
        endif()

        set(DOXYFILE_IN "${PROJECT_SOURCE_DIR}/cmake/Doxyfile.in")
        set(DOXYFILE_OUT "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile")
        configure_file("${DOXYFILE_IN}" "${DOXYFILE_OUT}" @ONLY)

        add_custom_target(docs
            COMMAND "${DOXYGEN_EXECUTABLE}" "${DOXYFILE_OUT}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            COMMENT "Generating EEVS API & Plugin SDK documentation with Doxygen"
            VERBATIM
        )
    endif()
endif()

# ==============================================================================
# Installation & CPack Packaging (`cmake --install build --prefix dist`)
# ==============================================================================
install(FILES
    "${PROJECT_SOURCE_DIR}/eevs.cfg"
    "${PROJECT_SOURCE_DIR}/README.md"
    "${PROJECT_SOURCE_DIR}/LICENSE"
    "${PROJECT_SOURCE_DIR}/LICENSE.MIT"
    DESTINATION .
)

install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/"
    DESTINATION include
)

install(DIRECTORY "${PROJECT_SOURCE_DIR}/scripts/"
    DESTINATION scripts
)

install(DIRECTORY "${PROJECT_SOURCE_DIR}/demo/"
    DESTINATION demo
)

set(CPACK_PACKAGE_NAME "eevs")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "NarutoUA")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "EEVS - Enhanced External Voice Server for FiveM Enhanced")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)
