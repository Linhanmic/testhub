# TestHub 测试：单元测试 + HTTP/WebSocket 集成测试（零依赖，自带微型断言框架）

add_executable(testhub_unit_tests
    test_main.cpp
    test_json.cpp
    test_spec_parser.cpp
    test_tag_filter.cpp
    test_test_queue.cpp
    test_event_bus.cpp
    test_http_server.cpp
    test_websocket.cpp
    test_execution_engine.cpp
    test_report_writer.cpp
    test_http_client.cpp
    test_spec_watcher.cpp
    test_scheduler.cpp
    test_trends.cpp
    test_projects.cpp
    test_runner_bridge.cpp)
target_link_libraries(testhub_unit_tests PRIVATE testhub_core)
target_compile_definitions(testhub_unit_tests PRIVATE
    TESTHUB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME unit_tests COMMAND testhub_unit_tests)

if(NOT WIN32)
    add_executable(testhub_integration_tests
        test_main.cpp
        test_integration.cpp)
    target_link_libraries(testhub_integration_tests PRIVATE testhub_core)
    target_compile_definitions(testhub_integration_tests PRIVATE
        TESTHUB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME integration_tests COMMAND testhub_integration_tests)
    set_tests_properties(integration_tests PROPERTIES TIMEOUT 180)
endif()

find_program(PYTHON3_EXECUTABLE NAMES python3 python)
if(PYTHON3_EXECUTABLE)
    add_test(NAME python_runner_protocol
        COMMAND "${PYTHON3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/runners/python/test_runner_protocol.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/runners/python")
    set_tests_properties(python_runner_protocol PROPERTIES TIMEOUT 60)
endif()

find_program(NODE_EXECUTABLE NAMES node nodejs)
if(NODE_EXECUTABLE)
    add_test(NAME node_runner_protocol
        COMMAND "${NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/runners/node/test_runner_protocol.js"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/runners/node")
    set_tests_properties(node_runner_protocol PROPERTIES TIMEOUT 60)
    add_test(NAME ui_i18n
        COMMAND "${NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_i18n.js"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

if(PYTHON3_EXECUTABLE AND NOT WIN32)
    add_test(NAME http_stress
        COMMAND "${PYTHON3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/stress.py"
                --binary "$<TARGET_FILE:testhub>"
                --specs "${CMAKE_SOURCE_DIR}/specs"
                --connections 16
                --scenarios 800
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(http_stress PROPERTIES TIMEOUT 90)
endif()
