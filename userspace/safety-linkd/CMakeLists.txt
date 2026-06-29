# safety-linkd/CMakeLists.txt

add_executable(safety-linkd
    main.cpp
)

target_include_directories(safety-linkd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
