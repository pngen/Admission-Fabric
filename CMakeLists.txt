cmake_minimum_required(VERSION 3.22)

project(AdmissionFabric
    VERSION 1.0.0
    DESCRIPTION "Vendor-neutral admission runtime for expensive accelerator infrastructure"
    HOMEPAGE_URL "https://github.com/pngen/Admission-Fabric"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(ADMISSION_FABRIC_BUILD_TESTS "Build CTest coverage" ON)
option(ADMISSION_FABRIC_BUILD_EXAMPLES "Build runnable examples" ON)
option(ADMISSION_FABRIC_BUILD_BENCHMARKS "Build completed-work benchmarks" ON)
option(ADMISSION_FABRIC_BUILD_CLI "Build the admission-fabricctl CLI and daemon apps" ON)
option(ADMISSION_FABRIC_ENABLE_CUDA "Build the real CUDA backend (requires a CUDA toolkit)" OFF)
option(ADMISSION_FABRIC_ENABLE_TESTING "Enable testing infrastructure" ON)
option(ADMISSION_FABRIC_INSTALL "Install targets and export the package" ON)

# ---------------------------------------------------------------------------
# Compiler configuration.
# ---------------------------------------------------------------------------
if(MSVC)
    # Zero-warning gate: /W4 and treat-all-warnings-as-errors. Applied only to
    # C++ (not CUDA) so the flags never reach nvcc.
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/W4>)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/WX>)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Core library.
# ---------------------------------------------------------------------------
add_library(admission_fabric STATIC)
add_library(AdmissionFabric::admission_fabric ALIAS admission_fabric)

file(GLOB_RECURSE AF_CORE_GLOB_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
target_sources(admission_fabric PRIVATE ${AF_CORE_GLOB_SOURCES})

target_include_directories(admission_fabric PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>")
target_compile_features(admission_fabric PUBLIC cxx_std_20)
set_target_properties(admission_fabric PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
if(WIN32)
    # Winsock is used by the net/protocol layer; static-library consumers must
    # link it too, so expose it publicly.
    target_link_libraries(admission_fabric PUBLIC ws2_32)
endif()

# CUDA backend lives in its own library so that the core library never
# requires a CUDA toolkit. The core runtime is CUDA-optional.
if(ADMISSION_FABRIC_ENABLE_CUDA)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    add_library(admission_fabric_cuda STATIC src/backends/cuda/cuda_backend.cu)
    add_library(AdmissionFabric::cuda_backend ALIAS admission_fabric_cuda)
    target_include_directories(admission_fabric_cuda PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>")
    target_link_libraries(admission_fabric_cuda PUBLIC admission_fabric CUDA::cudart)
    target_compile_features(admission_fabric_cuda PUBLIC cxx_std_20)
    set_target_properties(admission_fabric_cuda PROPERTIES
        CUDA_ARCHITECTURES "120"
        POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(admission_fabric_cuda PUBLIC ADMISSION_FABRIC_HAS_CUDA=1)
    add_compile_definitions(ADMISSION_FABRIC_HAS_CUDA=1)
endif()

if(ADMISSION_FABRIC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ADMISSION_FABRIC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(ADMISSION_FABRIC_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(ADMISSION_FABRIC_BUILD_CLI)
    add_subdirectory(apps)
endif()

# ---------------------------------------------------------------------------
# Install + export.
# ---------------------------------------------------------------------------
if(ADMISSION_FABRIC_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/admission_fabric"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

    install(TARGETS admission_fabric EXPORT AdmissionFabricTargets
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

    if(TARGET admission_fabric_cuda)
        install(TARGETS admission_fabric_cuda EXPORT AdmissionFabricTargets
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()

    install(EXPORT AdmissionFabricTargets
        FILE "AdmissionFabricTargets.cmake"
        NAMESPACE "AdmissionFabric::"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/AdmissionFabric")

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AdmissionFabricConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/AdmissionFabricConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/AdmissionFabric")

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/AdmissionFabricConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/AdmissionFabricConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/AdmissionFabricConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/AdmissionFabric")
endif()
