cmake_minimum_required(VERSION 3.20)
project(webengine CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
file(CREATE_LINK
    ${CMAKE_BINARY_DIR}/compile_commands.json
    ${CMAKE_SOURCE_DIR}/compile_commands.json
    SYMBOLIC
)

find_package(Boost 1.74 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

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

# ── Example application that consumes the library ─────────────────────────────
# Named `backend` and emitted at the build root so the existing Docker/systemd
# deployment (/usr/local/bin/backend) keeps working unchanged.
add_executable(backend example/main.cpp)
set_target_properties(backend PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(backend PRIVATE webengine)
