# safety-supervisord/CMakeLists.txt

add_executable(safety-supervisord
    main.cpp
    device_client.cpp
    protocol.cpp
    event_loop.cpp
    recovery_worker.cpp
    logger_worker.cpp
    health_state_machine.cpp
    replay_mode.cpp
)

target_include_directories(safety-supervisord PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
