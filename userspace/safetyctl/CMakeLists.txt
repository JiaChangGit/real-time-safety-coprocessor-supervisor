# safetyctl/CMakeLists.txt

add_executable(safetyctl
    main.cpp
    commands.cpp
)

target_include_directories(safetyctl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
