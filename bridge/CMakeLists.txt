# bridge/CMakeLists.txt
#
# 由 userspace 頂層 CMake 以 add_subdirectory(../bridge ...) 納入。
# 亦可獨立建置（cmake -S bridge -B build/bridge）。

cmake_minimum_required(VERSION 3.16)
project(safety_bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 獨立建置時也需要這些旗標與 include 路徑（被頂層納入時頂層已設定，
# 此處重複設定無害且確保獨立建置可用）。
add_compile_options(-Wall -Wextra -O2 -g)

add_executable(pty_bridge
    pty_bridge.cpp
    fault_injector.cpp
)

# 需要共用協定標頭 (safety_protocol.h)。
target_include_directories(pty_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../userspace/common
)

# 確保產物落在與其他工具相同的 bin 目錄（頂層已設 CMAKE_RUNTIME_OUTPUT_DIRECTORY；
# 獨立建置時 fallback 至本 build 的 bin）。
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set_target_properties(pty_bridge PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
