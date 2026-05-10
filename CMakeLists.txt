cmake_minimum_required(VERSION 3.20)
project(baa VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Header-only interface library
add_library(baa INTERFACE)
add_library(baa::baa ALIAS baa)

target_include_directories(baa INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(baa INTERFACE cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(baa INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()

# Build options
option(BAA_BUILD_TESTS       "Build tests"      ON)
option(BAA_BUILD_BENCHMARKS  "Build benchmarks" ON)

if(BAA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(BAA_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# Install rules
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS baa
    EXPORT baaTargets
)
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(EXPORT baaTargets
    FILE baaTargets.cmake
    NAMESPACE baa::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/baa
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/baaConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/baaConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/baa"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/baaConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/baaConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/baaConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/baa"
)
