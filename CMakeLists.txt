cmake_minimum_required(VERSION 3.16)
project(testhub VERSION 1.0.0 LANGUAGES CXX)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 设置编译选项
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
elseif(MSVC)
    add_compile_options(/W4)
endif()

# 查找依赖
find_package(Threads REQUIRED)

# 定义源文件
set(TESTHUB_SOURCES
    src/main.cpp
    src/testhub.cpp
    src/server/http_server.cpp
    src/runner/runner_bridge.cpp
)

# 复用 Gauge 解析器
set(GAUGE_PARSER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../gauge-cpp/src/parser/lexer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../gauge-cpp/src/parser/spec_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../gauge-cpp/src/parser/concept_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../gauge-cpp/src/parser/step_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../gauge-cpp/src/gauge/specification.cpp
)

# 创建库
add_library(testhub_core STATIC ${TESTHUB_SOURCES})
target_include_directories(testhub_core PUBLIC src)
target_include_directories(testhub_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../gauge-cpp/src)
target_link_libraries(testhub_core PUBLIC Threads::Threads)

# 创建可执行文件
add_executable(testhub ${GAUGE_PARSER_SOURCES})
target_include_directories(testhub PRIVATE src)
target_include_directories(testhub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../gauge-cpp/src)
target_link_libraries(testhub PRIVATE testhub_core Threads::Threads)

# Windows 特定链接
if(WIN32)
    target_link_libraries(testhub PRIVATE ws2_32)
endif()

# 安装
install(TARGETS testhub DESTINATION bin)
