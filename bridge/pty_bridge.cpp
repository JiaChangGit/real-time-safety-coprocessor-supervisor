// pty_bridge.cpp - host 端 UART-like 傳輸橋接器
//
// 在 Linux QEMU guest serial 與 Zephyr QEMU serial 之間轉送位元組。
// 以 posix_openpt/grantpt/unlockpt/ptsname 建立「兩個」PTY master，並印出對應
// 的兩個 slave 路徑（英文），讓 QEMU 以 `-serial /dev/pts/N` 各自接上。
//
// 轉送以 poll() 雙向進行；理解協定到「能切出完整 frame」的程度，以便對整個
// frame 套用 fault 注入（drop/corrupt/delay）。所有輸出英文。

#include "fault_injector.h"

#include "safety_protocol.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::atomic_bool g_run{true};

// 每個方向維護一個 streaming 組裝器，從 byte 流切出完整 frame。
// 與 supervisord 的 FrameAssembler 同樣邏輯，但 bridge 需保留原始位元組以轉發。
class ByteFramer {
public:
    // 餵入位元組，吐出「完整 frame 的原始位元組」序列。對於無法解析為合法
    // header 的雜訊位元組，逐 byte 直通轉發（append 為單 byte「frame」），
    // 避免阻塞非協定資料。
    void feed(const uint8_t *data, size_t len,
              std::vector<std::vector<uint8_t>> &out)
    {
        buf_.insert(buf_.end(), data, data + len);
        size_t pos = 0;
        for (;;) {
            // 對齊 magic。
            size_t hunt = pos;
            bool found = false;
            while (hunt + 2 <= buf_.size()) {
                uint16_t m;
                std::memcpy(&m, buf_.data() + hunt, sizeof(m));
                if (m == SAFETY_PROTO_MAGIC) {
                    found = true;
                    break;
                }
                ++hunt;
            }
            // hunt 之前的位元組是雜訊：直通轉發（保留傳輸透明性）。
            if (hunt > pos) {
                out.push_back(std::vector<uint8_t>(buf_.begin() + pos,
                                                   buf_.begin() + hunt));
                pos = hunt;
            }
            if (!found) {
                // 可能殘留 1 byte（半個 magic）：保留待下次。
                break;
            }
            if (pos + SAFETY_HEADER_SIZE > buf_.size())
                break; // header 未到齊
            SafetyFrameHeader hdr;
            std::memcpy(&hdr, buf_.data() + pos, SAFETY_HEADER_SIZE);
            size_t plen = hdr.payload_length;
            if (plen > SAFETY_MAX_PAYLOAD) {
                // 不合理長度：把這 2 bytes magic 當雜訊直通，前移重試。
                out.push_back(std::vector<uint8_t>(buf_.begin() + pos,
                                                   buf_.begin() + pos + 1));
                ++pos;
                continue;
            }
            size_t total = SAFETY_HEADER_SIZE + plen;
            if (pos + total > buf_.size())
                break; // payload 未到齊
            out.push_back(std::vector<uint8_t>(buf_.begin() + pos,
                                               buf_.begin() + pos + total));
            pos += total;
        }
        if (pos > 0)
            buf_.erase(buf_.begin(), buf_.begin() + pos);
    }

private:
    std::vector<uint8_t> buf_;
};

void print_help(const char *prog)
{
    std::cout <<
        "Usage: " << prog << " [options]\n"
        "\n"
        "Host-side UART-like bridge between two QEMU serial endpoints.\n"
        "Creates two PTYs and forwards frames between them, applying optional\n"
        "fault injection. Attach each QEMU with -serial /dev/pts/N.\n"
        "\n"
        "Options:\n"
        "  --left PATH       open an existing LEFT PTY path\n"
        "  --right PATH      open an existing RIGHT PTY path\n"
        "  --left-name FILE   write the LEFT slave pts path to FILE (for scripts)\n"
        "  --right-name FILE  write the RIGHT slave pts path to FILE\n"
        "  --drop N           drop every Nth frame\n"
        "  --drop-type TYPE   only drop frames of TYPE (1=HEARTBEAT 2=TELEMETRY\n"
        "                       3=FAULT_EVENT 4=COMMAND 5=ACK 6=NACK 7=RECOVERY)\n"
        "  --delay-ms N       delay each forwarded frame by N ms\n"
        "  --corrupt N        corrupt checksum of every Nth frame\n"
        "  --log FILE         log transported frames (ts,dir,type,seq,len,action)\n"
        "  --help             show this help and exit\n";
}

// 建立一個 PTY master，回傳 master fd 並由 out_slave 取得 slave 路徑。
int make_pty(std::string &out_slave)
{
    int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0)
        return -1;
    if (::grantpt(master) != 0 || ::unlockpt(master) != 0) {
        ::close(master);
        return -1;
    }
    const char *name = ::ptsname(master);
    if (!name) {
        ::close(master);
        return -1;
    }
    out_slave = name;
    // 設為非阻塞，配合 poll()。
    int fl = ::fcntl(master, F_GETFL, 0);
    ::fcntl(master, F_SETFL, fl | O_NONBLOCK);
    return master;
}

void write_name_file(const std::string &path, const std::string &value)
{
    if (path.empty())
        return;
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (f.is_open())
        f << value << "\n";
}

int open_endpoint(const std::string &path)
{
    int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    return fd;
}

static void on_signal(int) { g_run.store(false); }

} // namespace

int main(int argc, char **argv)
{
    bridge::InjectorConfig cfg;
    std::string left_path, right_path;
    std::string left_name_file, right_name_file;

    for (int i = 1; i < argc; ++i) {
        std::string opt = argv[i];
        auto value = [&](const char *o) -> const char * {
            if (i + 1 >= argc) {
                std::cerr << "error: option " << o << " requires a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (opt == "--help" || opt == "-h") {
            print_help(argv[0]);
            return 0;
        } else if (opt == "--left") {
            left_path = value("--left");
        } else if (opt == "--right") {
            right_path = value("--right");
        } else if (opt == "--left-name") {
            left_name_file = value("--left-name");
        } else if (opt == "--right-name") {
            right_name_file = value("--right-name");
        } else if (opt == "--drop") {
            cfg.drop_every = std::atoi(value("--drop"));
        } else if (opt == "--drop-type") {
            cfg.drop_type = std::atoi(value("--drop-type"));
        } else if (opt == "--delay-ms") {
            cfg.delay_ms = std::atoi(value("--delay-ms"));
        } else if (opt == "--corrupt") {
            cfg.corrupt_every = std::atoi(value("--corrupt"));
        } else if (opt == "--log") {
            cfg.log_path = value("--log");
        } else {
            std::cerr << "error: unknown option '" << opt << "'\n";
            print_help(argv[0]);
            return 2;
        }
    }

    std::string left_slave, right_slave;
    int left = -1;
    int right = -1;
    if (!left_path.empty() || !right_path.empty()) {
        if (left_path.empty() || right_path.empty()) {
            std::cerr << "error: --left and --right must be used together\n";
            return 2;
        }
        left_slave = left_path;
        right_slave = right_path;
        left = open_endpoint(left_slave);
        if (left < 0) {
            std::cerr << "error: failed to open LEFT endpoint '" << left_slave
                      << "': " << std::strerror(errno) << "\n";
            return 1;
        }
        right = open_endpoint(right_slave);
        if (right < 0) {
            std::cerr << "error: failed to open RIGHT endpoint '" << right_slave
                      << "': " << std::strerror(errno) << "\n";
            ::close(left);
            return 1;
        }
    } else {
        left = make_pty(left_slave);
    }
    if (left < 0) {
        std::cerr << "error: failed to create LEFT pty: " << std::strerror(errno)
                  << "\n";
        return 1;
    }
    if (right < 0) {
        right = make_pty(right_slave);
        if (right < 0) {
            std::cerr << "error: failed to create RIGHT pty: "
                      << std::strerror(errno) << "\n";
            ::close(left);
            return 1;
        }
    }

    write_name_file(left_name_file, left_slave);
    write_name_file(right_name_file, right_slave);

    std::cout << "pty_bridge: LEFT  slave = " << left_slave << "\n";
    std::cout << "pty_bridge: RIGHT slave = " << right_slave << "\n";
    std::cout << "pty_bridge: attach QEMU with -serial " << left_slave
              << " and -serial " << right_slave << "\n";
    std::cout << "pty_bridge: forwarding (Ctrl-C to stop)\n";
    std::cout.flush();

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    bridge::FaultInjector injector(std::move(cfg));
    ByteFramer framer_l2r; // LEFT -> RIGHT
    ByteFramer framer_r2l; // RIGHT -> LEFT

    auto forward = [&](int from_fd, int to_fd, ByteFramer &framer,
                       const char *dir) {
        uint8_t buf[1024];
        ssize_t n = ::read(from_fd, buf, sizeof(buf));
        if (n <= 0)
            return;
        std::vector<std::vector<uint8_t>> frames;
        framer.feed(buf, static_cast<size_t>(n), frames);
        for (auto &fr : frames) {
            int delay_ms = 0;
            bool fwd = injector.process(fr, dir, delay_ms);
            if (!fwd)
                continue;
            if (delay_ms > 0)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(delay_ms));
            size_t off = 0;
            while (off < fr.size()) {
                ssize_t w = ::write(to_fd, fr.data() + off, fr.size() - off);
                if (w < 0) {
                    if (errno == EINTR)
                        continue;
                    break; // 對端尚未連線等情況：放棄此 frame
                }
                off += static_cast<size_t>(w);
            }
        }
    };

    while (g_run.load()) {
        struct pollfd fds[2];
        fds[0].fd = left;
        fds[0].events = POLLIN;
        fds[1].fd = right;
        fds[1].events = POLLIN;

        int pr = ::poll(fds, 2, 500);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            continue;
        if (fds[0].revents & POLLIN)
            forward(left, right, framer_l2r, "L->R");
        if (fds[1].revents & POLLIN)
            forward(right, left, framer_r2l, "R->L");
    }

    ::close(left);
    ::close(right);
    std::cout << "pty_bridge: stopped\n";
    return 0;
}
