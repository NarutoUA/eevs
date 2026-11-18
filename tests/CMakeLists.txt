add_executable(eevs_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp
)

target_include_directories(eevs_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/plugins/common
)

target_link_libraries(eevs_tests PRIVATE
    eevs_core
    eevs_plugin_common
    doctest::doctest
)

set_target_properties(eevs_tests PROPERTIES
    FOLDER "tests"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/$<CONFIG>"
)

add_test(NAME eevs_tests COMMAND eevs_tests)
