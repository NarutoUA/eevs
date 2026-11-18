set(EEVS_CORE_SOURCES
    ${PROJECT_SOURCE_DIR}/include/eevs/api_version.h
    ${PROJECT_SOURCE_DIR}/include/eevs/host_api.h
    ${PROJECT_SOURCE_DIR}/include/eevs/plugin.h
    ${PROJECT_SOURCE_DIR}/include/eevs/plugin_scope.h
    ${PROJECT_SOURCE_DIR}/include/eevs/types.h
    ${PROJECT_SOURCE_DIR}/include/eevs/interfaces/i_audio_source.h
    ${PROJECT_SOURCE_DIR}/include/eevs/interfaces/i_audio_tap.h
    ${PROJECT_SOURCE_DIR}/include/eevs/interfaces/i_dsp_processor.h
    ${PROJECT_SOURCE_DIR}/include/eevs/interfaces/i_route_provider.h
    ${PROJECT_SOURCE_DIR}/include/eevs/interfaces/i_session_listener.h
    ${PROJECT_SOURCE_DIR}/include/eevs/interfaces/i_voice_packet_filter.h
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/audio_pipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/audio_pipeline.h
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/clip_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/clip_manager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/opus_codec_pool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/opus_codec_pool.h
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/packet_synthesizer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/packet_synthesizer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/containers.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/crypto_utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/crypto_utils.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/logger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/logger.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/profiler.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/routing_matrix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/routing_matrix.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/server_config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/server_config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/session_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/session_registry.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/voice_engine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/voice_engine.h
    ${CMAKE_CURRENT_SOURCE_DIR}/network/control_api_server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/network/control_api_server.h
    ${CMAKE_CURRENT_SOURCE_DIR}/network/enet.h
    ${CMAKE_CURRENT_SOURCE_DIR}/network/enet_impl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/network/fx_gateway_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/network/fx_gateway_client.h
    ${CMAKE_CURRENT_SOURCE_DIR}/network/protocol.h
    ${CMAKE_CURRENT_SOURCE_DIR}/network/voice_udp_server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/network/voice_udp_server.h
    ${CMAKE_CURRENT_SOURCE_DIR}/plugins/plugin_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/plugins/plugin_manager.h
)

add_library(eevs_core STATIC ${EEVS_CORE_SOURCES})

source_group(
    TREE "${PROJECT_SOURCE_DIR}"
    FILES ${EEVS_CORE_SOURCES}
)

target_include_directories(eevs_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/network
    ${CMAKE_CURRENT_SOURCE_DIR}/plugins
)

target_link_libraries(eevs_core PUBLIC
    glaze::glaze
    OpenSSL::Crypto
    OpenSSL::SSL
    Opus::opus
    lz4::lz4
    quill::quill
    unofficial::uwebsockets::uwebsockets
)

if(EEVS_ENABLE_TRACY AND TARGET Tracy::TracyClient)
    target_link_libraries(eevs_core PUBLIC Tracy::TracyClient)
endif()

if(WIN32)
    target_compile_definitions(eevs_core PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_link_libraries(eevs_core PUBLIC
        ws2_32
        winmm
    )
endif()

add_executable(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE eevs_core)

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION .
)

if(WIN32)
    install(FILES $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}>
        DESTINATION .
    )
endif()

if(EEVS_BUILD_BENCHMARK)
    set(EEVS_BENCH_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/bench/main_bench.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/crypto_utils.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/crypto_utils.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/server_config.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/server_config.h
        ${CMAKE_CURRENT_SOURCE_DIR}/network/enet.h
        ${CMAKE_CURRENT_SOURCE_DIR}/network/enet_impl.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/network/protocol.h
    )

    add_executable(eevs_bench ${EEVS_BENCH_SOURCES})

    target_include_directories(eevs_bench PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/network
    )

    target_link_libraries(eevs_bench PRIVATE
        CLI11::CLI11
        ftxui::screen
        ftxui::dom
        ftxui::component
        glaze::glaze
        hdr_histogram::hdr_histogram_static
        OpenSSL::Crypto
        Opus::opus
        lz4::lz4
    )

    if(WIN32)
        target_compile_definitions(eevs_bench PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
        )
        target_link_libraries(eevs_bench PRIVATE
            ws2_32
            winmm
        )
    endif()

    install(TARGETS eevs_bench
        RUNTIME DESTINATION .
    )
endif()

