# userspace/CMakeLists.txt - safety_userspace 頂層 CMake
#
# 本檔為整個 C++17 userspace 的建置入口。被以下指令驅動：
#   cmake -S userspace -B build/userspace -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build build/userspace -j3
#
# 產物固定落在 build/userspace/bin/{safety-supervisord,safetyctl,pty_bridge}。

cmake_minimum_required(VERSION 3.16)
project(safety_userspace CXX)

# ---- C++17、嚴格警告、優化 + debug 資訊、pthread ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 若未指定 build type，預設 RelWithDebInfo（保留 -g 供 valgrind 用）。
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -O2 -g)
add_compile_options(-pthread)
add_link_options(-pthread)

# ---- 所有可執行檔輸出至 build/userspace/bin ----
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---- 共用 include 路徑：協定標頭 + kernel ioctl 標頭 ----
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/../linux/drivers/safety_copro
)

# ---- 子專案 ----
add_subdirectory(safety-supervisord)
add_subdirectory(safety-linkd)
add_subdirectory(safetyctl)
# bridge 在 source tree 之外（../bridge），需指定獨立 binary dir。
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../bridge ${CMAKE_BINARY_DIR}/bridge)
