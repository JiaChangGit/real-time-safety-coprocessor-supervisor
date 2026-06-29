// commands.cpp - safetyctl 命令實作

#include "commands.h"

#include "safety_copro_ioctl.h"
#include "safety_protocol.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace safetyctl {

namespace {

// RAII fd（safetyctl 端輕量版）。
class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

private:
    int fd_;
};

const char *link_state_name(uint32_t s)
{
    switch (s) {
    case 0: return "BOOTING";
    case 1: return "HB_OK";
    case 2: return "HB_TIMEOUT";
    case 3: return "RECOVERING";
    default: return "UNKNOWN";
    }
}

// 將一個 COMMAND frame 寫入裝置 fd。
bool send_command(int fd, uint8_t command, uint8_t arg8, uint16_t arg16,
                  uint32_t arg32)
{
    SafetyFrameHeader hdr;
    SafetyCommandPayload p{};
    p.command = command;
    p.arg8 = arg8;
    p.arg16 = arg16;
    p.arg32 = arg32;
    safety_frame_init(&hdr, SAFETY_FRAME_COMMAND, 0, sizeof(p), 0);

    std::vector<uint8_t> out(SAFETY_HEADER_SIZE + sizeof(p));
    hdr.checksum = safety_frame_compute_checksum(&hdr, &p, sizeof(p));
    std::memcpy(out.data(), &hdr, SAFETY_HEADER_SIZE);
    std::memcpy(out.data() + SAFETY_HEADER_SIZE, &p, sizeof(p));

    ssize_t n = ::write(fd, out.data(), out.size());
    return n == static_cast<ssize_t>(out.size());
}

} // namespace

int cmd_status(const std::string &device)
{
    int fd = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::cerr << "error: cannot open device '" << device
                  << "': " << std::strerror(errno) << "\n";
        return 1;
    }
    Fd guard(fd);

    struct safety_copro_stats st{};
    if (::ioctl(fd, SAFETY_IOC_GET_STATS, &st) != 0) {
        std::cerr << "error: SAFETY_IOC_GET_STATS failed: "
                  << std::strerror(errno) << "\n";
        return 1;
    }

    std::cout << "==== safety_copro status ====\n";
    std::cout << "current_state        : " << link_state_name(st.current_state)
              << " (" << st.current_state << ")\n";
    std::cout << "heartbeat_count      : " << st.heartbeat_count << "\n";
    std::cout << "fault_count          : " << st.fault_count << "\n";
    std::cout << "dropped_frame_count  : " << st.dropped_frame_count << "\n";
    std::cout << "timeout_count        : " << st.timeout_count << "\n";
    std::cout << "retry_count          : " << st.retry_count << "\n";
    std::cout << "protocol_error_count : " << st.protocol_error_count << "\n";
    std::cout << "rx_frames_total      : " << st.rx_frames_total << "\n";
    std::cout << "rx_queue_depth       : " << st.rx_queue_depth << "\n";
    std::cout << "tx_queue_depth       : " << st.tx_queue_depth << "\n";
    std::cout << "=============================\n";
    return 0;
}

int cmd_inject(const std::string &device, const std::string &kind)
{
    // 將 CLI kind 對應到 fault_type / 行為。
    uint32_t fault_type;
    std::string desc;
    if (kind == "heartbeat-timeout") {
        // host 端可由 bridge drop heartbeat 觸發；此處以 INJECT_FAULT task_hang
        // 近似（使 co-processor 停止送 heartbeat）。
        fault_type = SAFETY_FAULT_TASK_HANG;
        desc = "heartbeat-timeout -> INJECT_FAULT task_hang (stops heartbeats)";
    } else if (kind == "checksum-error") {
        fault_type = SAFETY_FAULT_CHECKSUM_ERROR;
        desc = "checksum-error -> INJECT_FAULT checksum_error "
               "(usually driven via pty_bridge --corrupt)";
    } else {
        std::cerr << "error: unknown inject kind '" << kind << "'\n";
        std::cerr << "valid: heartbeat-timeout|checksum-error\n";
        return 2;
    }

    std::cout << desc << "\n";

    int fd = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::cerr << "error: cannot open device '" << device
                  << "': " << std::strerror(errno) << "\n";
        return 1;
    }
    Fd guard(fd);

    // 優先用 ioctl 注入；若 ioctl 失敗（例如 mock 環境），退回送 COMMAND frame。
    if (::ioctl(fd, SAFETY_IOC_INJECT_FAULT, fault_type) == 0) {
        std::cout << "injected via ioctl SAFETY_IOC_INJECT_FAULT (fault_type="
                  << fault_type << ")\n";
        return 0;
    }
    std::cerr << "ioctl SAFETY_IOC_INJECT_FAULT failed (" << std::strerror(errno)
              << "), falling back to COMMAND frame\n";

    if (!send_command(fd, SAFETY_CMD_INJECT_FAULT,
                      static_cast<uint8_t>(fault_type), 0, 0)) {
        std::cerr << "error: failed to write COMMAND frame: "
                  << std::strerror(errno) << "\n";
        return 1;
    }
    std::cout << "injected via COMMAND frame INJECT_FAULT\n";
    return 0;
}

int cmd_recover(const std::string &device)
{
    int fd = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::cerr << "error: cannot open device '" << device
                  << "': " << std::strerror(errno) << "\n";
        return 1;
    }
    Fd guard(fd);

    if (::ioctl(fd, SAFETY_IOC_FORCE_RECOVERY) == 0) {
        std::cout << "recovery forced via ioctl SAFETY_IOC_FORCE_RECOVERY\n";
        return 0;
    }
    std::cerr << "ioctl SAFETY_IOC_FORCE_RECOVERY failed ("
              << std::strerror(errno) << "), falling back to COMMAND frame\n";

    if (!send_command(fd, SAFETY_CMD_REQUEST_RECOVERY, 0, 0, 0)) {
        std::cerr << "error: failed to write COMMAND frame: "
                  << std::strerror(errno) << "\n";
        return 1;
    }
    std::cout << "recovery requested via COMMAND frame REQUEST_RECOVERY\n";
    return 0;
}

int cmd_dump_report(const std::string &device)
{
    (void)device;
    const std::string dbg = "/sys/kernel/debug/safety_copro";

    DIR *d = ::opendir(dbg.c_str());
    if (d) {
        std::cout << "==== debugfs dump (" << dbg << ") ====\n";
        std::vector<std::string> names;
        struct dirent *ent;
        while ((ent = ::readdir(d)) != nullptr) {
            std::string name = ent->d_name;
            if (name == "." || name == "..")
                continue;
            names.push_back(name);
        }
        ::closedir(d);

        for (const auto &name : names) {
            std::string path = dbg + "/" + name;
            std::ifstream f(path);
            std::cout << "---- " << name << " ----\n";
            if (f.is_open()) {
                std::string line;
                while (std::getline(f, line))
                    std::cout << line << "\n";
            } else {
                std::cout << "(cannot read)\n";
            }
        }
        std::cout << "=================================\n";
        return 0;
    }

    std::cout << "debugfs '" << dbg << "' not available.\n";
    std::cout << "Userspace reports are written by safety-supervisord to:\n";
    std::cout << "  reports/events.jsonl        (live/mock event log)\n";
    std::cout << "  reports/replay_events.jsonl (replay state changes)\n";
    return 0;
}

} // namespace safetyctl
