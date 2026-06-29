# CMakeLists.txt - Real-Time Safety Co-Processor Supervisor firmware
#
# 標準 Zephyr application 結構。Board target: qemu_cortex_a53/qemu_cortex_a53/smp
#
# 建置範例：
#   FORMAL: west build -b qemu_cortex_a53/qemu_cortex_a53/smp .
#   DEBUG : west build -b qemu_cortex_a53/qemu_cortex_a53/smp . -- \
#               -DOVERLAY_CONFIG=prj_debug.conf
#   或直接指定 CONF_FILE=prj_debug.conf 取代 prj.conf。

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(safety_copro_firmware C)

target_sources(app PRIVATE
	src/main.c
	src/heartbeat_task.c
	src/telemetry_task.c
	src/fault_monitor_task.c
	src/command_handler_task.c
	src/safety_protocol.c
)

# include/ 內含 LOCKED 的 safety_protocol.h 與 firmware 內部 safety_app.h
target_include_directories(app PRIVATE include)
