eevs_add_plugin(audio_cpp_client audio_cpp_client_plugin.cpp)

target_link_libraries(audio_cpp_client PRIVATE OpenSSL::Crypto)
