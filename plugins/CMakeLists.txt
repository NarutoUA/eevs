add_library(eevs_plugin_common STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/common/audio_io.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/audio_io.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/containers.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/json_response.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/spsc_ring_buffer.h"
)
target_include_directories(eevs_plugin_common PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/common
)
target_link_libraries(eevs_plugin_common PUBLIC
    glaze::glaze
    Opus::opus
)
set_target_properties(eevs_plugin_common PROPERTIES FOLDER "plugins")

function(eevs_add_plugin TARGET_NAME SOURCE_FILE)
    add_library(${TARGET_NAME} SHARED ${SOURCE_FILE})
    target_include_directories(${TARGET_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/common
    )
    target_link_libraries(${TARGET_NAME} PRIVATE
        eevs_plugin_common
        glaze::glaze
    )
    set_target_properties(${TARGET_NAME} PROPERTIES
        FOLDER "plugins"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/$<CONFIG>/plugins"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/$<CONFIG>/plugins"
    )
    if(WIN32)
        target_compile_definitions(${TARGET_NAME} PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
        )
    endif()

    install(TARGETS ${TARGET_NAME}
        RUNTIME DESTINATION plugins
        LIBRARY DESTINATION plugins
    )
endfunction()

add_subdirectory(voice-recorder)
add_subdirectory(audio-cpp-client)
add_subdirectory(voice-effects)
