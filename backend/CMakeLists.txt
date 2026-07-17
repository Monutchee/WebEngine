cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(webengine LANGUAGES CXX)
endif()

set(_webengine_build_example_default OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_webengine_build_example_default ON)
endif()
option(WEBENGINE_BUILD_EXAMPLE "Build the WebEngine example backend"
       ${_webengine_build_example_default})
option(WEBENGINE_FETCH_DEPS
       "Fetch Glaze when a parent project or package manager did not provide it"
       ON)

find_package(Boost 1.74 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Prefer a target supplied by the parent project, then an installed package.
# FetchContent remains an explicit convenience fallback for standalone builds.
if(NOT TARGET glaze::glaze)
    find_package(glaze 7.9.0 CONFIG QUIET)
endif()
if(NOT TARGET glaze::glaze AND WEBENGINE_FETCH_DEPS)
    include(FetchContent)
    message(STATUS "Glaze 7.9.0 was not found; fetching it into the build directory")
    set(glaze_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(glaze_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG v7.9.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glaze)
endif()
if(NOT TARGET glaze::glaze)
    message(FATAL_ERROR
        "WebEngine requires glaze::glaze (Glaze 7.9.0 or newer). "
        "Provide the target or enable WEBENGINE_FETCH_DEPS.")
endif()

# ── The reusable web-engine library ───────────────────────────────────────────
add_library(webengine STATIC
    src/WebEngine.cpp
    src/Router.cpp
    src/AuthHandler.cpp
    src/Session.cpp
    src/Listener.cpp
    src/ProcessUtil.cpp
    src/NginxController.cpp
    src/LighttpdController.cpp
    src/WebServerFactory.cpp
)
add_library(webengine::webengine ALIAS webengine)

# Public headers (include/webengine/*) are visible to consumers; src/ is private.
target_include_directories(webengine
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(webengine PUBLIC
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)
# glaze is used only inside the library's .cpp files (no public header exposes it),
# so it's a private build-time dependency.
target_link_libraries(webengine PRIVATE glaze::glaze)
target_compile_features(webengine PUBLIC cxx_std_23)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(webengine PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Example application that consumes the library ─────────────────────────────
# Named `backend` and emitted at the build root so the existing Docker/systemd
# deployment (/usr/local/bin/backend) keeps working unchanged.
if(WEBENGINE_BUILD_EXAMPLE)
    add_executable(backend example/main.cpp)
    set_target_properties(backend PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    # The example calls Glaze directly; WebEngine itself keeps Glaze private.
    target_link_libraries(backend PRIVATE webengine::webengine glaze::glaze)
    target_compile_features(backend PRIVATE cxx_std_23)
endif()
