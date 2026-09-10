cmake_minimum_required(VERSION 3.16)
project(TestHub VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(TESTHUB_BUILD_TESTS "Build unit and integration tests" ON)
option(TESTHUB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

find_package(Threads REQUIRED)

# ------------------------------------------------------------
# 编译选项
# ------------------------------------------------------------
add_library(testhub_options INTERFACE)
if(MSVC)
    target_compile_options(testhub_options INTERFACE /W4 /utf-8 /EHsc)
    target_compile_definitions(testhub_options INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    if(TESTHUB_WARNINGS_AS_ERRORS)
        target_compile_options(testhub_options INTERFACE /WX)
    endif()
else()
    target_compile_options(testhub_options INTERFACE -Wall -Wextra -Wpedantic)
    if(TESTHUB_WARNINGS_AS_ERRORS)
        target_compile_options(testhub_options INTERFACE -Werror)
    endif()
endif()

# ------------------------------------------------------------
# 内嵌 Web UI 资源
# ------------------------------------------------------------
set(TESTHUB_WEB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/web")
file(GLOB_RECURSE TESTHUB_WEB_FILES CONFIGURE_DEPENDS
    "${TESTHUB_WEB_DIR}/*.html"
    "${TESTHUB_WEB_DIR}/*.css"
    "${TESTHUB_WEB_DIR}/*.js"
    "${TESTHUB_WEB_DIR}/*.svg"
    "${TESTHUB_WEB_DIR}/*.json"
    "${TESTHUB_WEB_DIR}/*.png"
    "${TESTHUB_WEB_DIR}/*.ico")
list(SORT TESTHUB_WEB_FILES)

set(TESTHUB_WEB_ASSETS_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/web_assets.cpp")
add_custom_command(
    OUTPUT "${TESTHUB_WEB_ASSETS_CPP}"
    COMMAND "${CMAKE_COMMAND}"
            "-DOUTPUT=${TESTHUB_WEB_ASSETS_CPP}"
            "-DBASE_DIR=${TESTHUB_WEB_DIR}"
            "-DINPUTS=${TESTHUB_WEB_FILES}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedResources.cmake"
    DEPENDS ${TESTHUB_WEB_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedResources.cmake"
    COMMENT "Embedding Web UI assets"
    VERBATIM)

# ------------------------------------------------------------
# 核心库
# ------------------------------------------------------------
set(TESTHUB_CORE_SOURCES
    src/testhub.cpp
    src/engine/execution_engine.cpp
    src/engine/result_store.cpp
    src/event/event_bus.cpp
    src/runner/mock_runner.cpp
    src/runner/process_runner.cpp
    src/runner/runner_bridge.cpp
    src/server/api_routes.cpp
    src/server/http_server.cpp
    src/server/web_ui.cpp
    src/server/websocket_server.cpp
    src/spec/spec_parser.cpp
    src/spec/spec_repository.cpp
    "${TESTHUB_WEB_ASSETS_CPP}")

add_library(testhub_core STATIC ${TESTHUB_CORE_SOURCES})
target_include_directories(testhub_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(testhub_core PUBLIC Threads::Threads testhub_options)
target_compile_definitions(testhub_core PUBLIC TESTHUB_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(testhub_core PUBLIC ws2_32)
endif()

# ------------------------------------------------------------
# 可执行文件
# ------------------------------------------------------------
add_executable(testhub src/main.cpp)
target_link_libraries(testhub PRIVATE testhub_core)

install(TARGETS testhub RUNTIME DESTINATION bin)
install(DIRECTORY specs/ DESTINATION share/testhub/specs)
install(DIRECTORY runners/ DESTINATION share/testhub/runners)

# ------------------------------------------------------------
# 测试
# ------------------------------------------------------------
if(TESTHUB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "TestHub ${PROJECT_VERSION}: build type=${CMAKE_BUILD_TYPE}, tests=${TESTHUB_BUILD_TESTS}")
