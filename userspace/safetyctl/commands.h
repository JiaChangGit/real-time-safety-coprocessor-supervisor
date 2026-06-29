// commands.h - safetyctl 的命令邏輯
//
// safetyctl 是與 /dev/safety_copro 互動的小工具。所有實際邏輯在 commands.cpp，
// main.cpp 只負責分派。輸出一律英文。

#ifndef SAFETYCTL_COMMANDS_H
#define SAFETYCTL_COMMANDS_H

#include <string>

namespace safetyctl {

// 各子命令回傳 process exit code（0 成功，非 0 失敗）。

// 開裝置、ioctl GET_STATS、印出英文計數器。
int cmd_status(const std::string &device);

// inject <kind>：kind ∈ heartbeat-timeout|checksum-error
int cmd_inject(const std::string &device, const std::string &kind);

// recover：ioctl FORCE_RECOVERY（失敗則嘗試送 REQUEST_RECOVERY command）。
int cmd_recover(const std::string &device);

// dump-report：印出 debugfs 內容（若存在），否則指出 reports 路徑。
int cmd_dump_report(const std::string &device);

} // namespace safetyctl

#endif // SAFETYCTL_COMMANDS_H
