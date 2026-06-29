// main.cpp - safety-linkd transport adapter
//
// 職責只限於 transport adapter：
//   /dev/ttyAMA1 RX -> SAFETY_IOC_PUSH_RX_FRAME -> /dev/safety_copro RX queue
//   SAFETY_IOC_POP_TX_FRAME -> /dev/ttyAMA1 TX
// 不維護 health state machine，不產生 ACK/NACK/recovery 決策。

#include "safety_copro_ioctl.h"
#include "safety_protocol.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace {

std::atomic_bool g_running{true};

void on_signal(int)
{
    g_running.store(false);
}

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const char *frame_type_name(uint8_t type)
{
    switch (type) {
    case SAFETY_FRAME_HEARTBEAT:       return "HEARTBEAT";
    case SAFETY_FRAME_TELEMETRY:       return "TELEMETRY";
    case SAFETY_FRAME_FAULT_EVENT:     return "FAULT_EVENT";
    case SAFETY_FRAME_COMMAND:         return "COMMAND";
    case SAFETY_FRAME_ACK:             return "ACK";
    case SAFETY_FRAME_NACK:            return "NACK";
    case SAFETY_FRAME_RECOVERY_REPORT: return "RECOVERY_REPORT";
    default:                           return "UNKNOWN";
    }
}

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Fd &operator=(Fd &&other) noexcept
    {
        if (this != &other) {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    void reset(int fd)
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

class LinkFramer {
public:
    void feed(const uint8_t *data, size_t len,
              std::vector<std::vector<uint8_t>> &out)
    {
        buf_.insert(buf_.end(), data, data + len);
        size_t pos = 0;

        for (;;) {
            while (pos + 2 <= buf_.size()) {
                uint16_t magic;
                std::memcpy(&magic, buf_.data() + pos, sizeof(magic));
                if (magic == SAFETY_PROTO_MAGIC)
                    break;
                ++pos;
            }
            if (pos + 2 > buf_.size())
                break;
            if (pos + SAFETY_HEADER_SIZE > buf_.size())
                break;

            SafetyFrameHeader hdr{};
            std::memcpy(&hdr, buf_.data() + pos, SAFETY_HEADER_SIZE);
            if (hdr.payload_length > SAFETY_MAX_PAYLOAD) {
                ++pos;
                continue;
            }

            const size_t total = SAFETY_HEADER_SIZE + hdr.payload_length;
            if (pos + total > buf_.size())
                break;

            out.emplace_back(buf_.begin() + static_cast<long>(pos),
                             buf_.begin() + static_cast<long>(pos + total));
            pos += total;
        }

        if (pos > 0)
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(pos));
    }

private:
    std::vector<uint8_t> buf_;
};

struct Args {
    std::string uart = "/dev/ttyAMA1";
    std::string device = "/dev/safety_copro";
    std::string report_dir = "reports";
    uint32_t duration_ms = 0;
};

void print_help(const char *prog)
{
    std::cout <<
        "Usage: " << prog << " [options]\n"
        "\n"
        "Transport adapter between the Linux protocol UART and /dev/safety_copro.\n"
        "\n"
        "Options:\n"
        "  --uart PATH        protocol UART path (default /dev/ttyAMA1)\n"
        "  --device PATH      safety driver path (default /dev/safety_copro)\n"
        "  --report-dir DIR   report directory (default reports)\n"
        "  --duration-ms N    run N ms then exit; 0 = until signal\n"
        "  --help             show this help and exit\n";
}

bool parse_args(int argc, char **argv, Args &args, bool &help)
{
    help = false;
    auto need_value = [&](int &i, const char *opt) -> const char * {
        if (i + 1 >= argc) {
            std::cerr << "error: option " << opt << " requires a value\n";
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string opt = argv[i];
        if (opt == "--help" || opt == "-h") {
            help = true;
            return true;
        } else if (opt == "--uart") {
            const char *v = need_value(i, "--uart");
            if (!v) return false;
            args.uart = v;
        } else if (opt == "--device") {
            const char *v = need_value(i, "--device");
            if (!v) return false;
            args.device = v;
        } else if (opt == "--report-dir") {
            const char *v = need_value(i, "--report-dir");
            if (!v) return false;
            args.report_dir = v;
        } else if (opt == "--duration-ms") {
            const char *v = need_value(i, "--duration-ms");
            if (!v) return false;
            args.duration_ms = static_cast<uint32_t>(std::stoul(v));
        } else {
            std::cerr << "error: unknown option '" << opt << "'\n";
            return false;
        }
    }
    return true;
}

bool make_report_dir(const std::string &path)
{
    if (::mkdir(path.c_str(), 0755) == 0)
        return true;
    return errno == EEXIST;
}

bool configure_uart_raw(int fd)
{
    termios tio{};
    if (::tcgetattr(fd, &tio) != 0)
        return false;
    ::cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~CRTSCTS;
    (void)::cfsetispeed(&tio, B115200);
    (void)::cfsetospeed(&tio, B115200);
    return ::tcsetattr(fd, TCSANOW, &tio) == 0;
}

bool write_all(int fd, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ::usleep(1000);
                continue;
            }
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

void log_event(std::ofstream &log, const char *kind, const char *frame_type,
               int seq, size_t bytes, const std::string &detail = "")
{
    if (!log.is_open())
        return;
    log << "{\"ts_ms\":" << now_ms()
        << ",\"kind\":\"" << kind
        << "\",\"frame_type\":\"" << frame_type
        << "\",\"seq\":" << seq
        << ",\"bytes\":" << bytes
        << ",\"detail\":\"" << detail << "\"}\n";
    log.flush();
}

bool push_rx_frame(int dev_fd, const std::vector<uint8_t> &frame)
{
    safety_copro_frame_io io{};
    if (frame.empty() || frame.size() > sizeof(io.data)) {
        errno = EMSGSIZE;
        return false;
    }
    io.len = static_cast<__u32>(frame.size());
    std::memcpy(io.data, frame.data(), frame.size());
    return ::ioctl(dev_fd, SAFETY_IOC_PUSH_RX_FRAME, &io) == 0;
}

bool pop_tx_frame(int dev_fd, safety_copro_frame_io &io)
{
    std::memset(&io, 0, sizeof(io));
    return ::ioctl(dev_fd, SAFETY_IOC_POP_TX_FRAME, &io) == 0;
}

} // namespace

int main(int argc, char **argv)
{
    Args args;
    bool help = false;
    if (!parse_args(argc, argv, args, help))
        return 2;
    if (help) {
        print_help(argv[0]);
        return 0;
    }

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    if (!make_report_dir(args.report_dir)) {
        std::cerr << "error: cannot create report dir '" << args.report_dir
                  << "': " << std::strerror(errno) << "\n";
        return 1;
    }
    std::ofstream report(args.report_dir + "/linkd_events.jsonl",
                         std::ios::out | std::ios::trunc);
    if (!report.is_open()) {
        std::cerr << "error: cannot open linkd report under '" << args.report_dir
                  << "'\n";
        return 1;
    }

    Fd uart(::open(args.uart.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK));
    if (!uart.valid()) {
        std::cerr << "error: cannot open UART '" << args.uart
                  << "': " << std::strerror(errno) << "\n";
        return 1;
    }
    if (!configure_uart_raw(uart.get())) {
        std::cerr << "error: cannot configure UART raw mode: "
                  << std::strerror(errno) << "\n";
        return 1;
    }

    Fd dev(::open(args.device.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK));
    if (!dev.valid()) {
        std::cerr << "error: cannot open device '" << args.device
                  << "': " << std::strerror(errno) << "\n";
        return 1;
    }

    std::cout << "safety-linkd: uart=" << args.uart
              << " device=" << args.device
              << " report=" << args.report_dir << "/linkd_events.jsonl\n";

    LinkFramer framer;
    const int64_t start = now_ms();
    uint8_t buf[1024];

    while (g_running.load()) {
        if (args.duration_ms > 0 &&
            now_ms() - start >= static_cast<int64_t>(args.duration_ms)) {
            break;
        }

        pollfd fds[2]{};
        fds[0].fd = uart.get();
        fds[0].events = POLLIN;
        fds[1].fd = dev.get();
        fds[1].events = POLLPRI;

        int pr = ::poll(fds, 2, 100);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            std::cerr << "error: poll failed: " << std::strerror(errno) << "\n";
            return 1;
        }

        if (fds[0].revents & POLLIN) {
            for (;;) {
                ssize_t n = ::read(uart.get(), buf, sizeof(buf));
                if (n < 0) {
                    if (errno == EINTR)
                        continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    std::cerr << "error: UART read failed: "
                              << std::strerror(errno) << "\n";
                    return 1;
                }
                if (n == 0)
                    break;

                std::vector<std::vector<uint8_t>> frames;
                framer.feed(buf, static_cast<size_t>(n), frames);
                for (const auto &frame : frames) {
                    SafetyFrameHeader hdr{};
                    if (frame.size() >= SAFETY_HEADER_SIZE)
                        std::memcpy(&hdr, frame.data(), SAFETY_HEADER_SIZE);
                    if (!push_rx_frame(dev.get(), frame)) {
                        log_event(report, "push_rx_failed",
                                  frame_type_name(hdr.type), hdr.sequence_id,
                                  frame.size(), std::strerror(errno));
                    } else {
                        log_event(report, "uart_rx_push",
                                  frame_type_name(hdr.type), hdr.sequence_id,
                                  frame.size());
                    }
                }
            }
        }

        for (;;) {
            safety_copro_frame_io tx{};
            if (!pop_tx_frame(dev.get(), tx)) {
                if (errno == EAGAIN || errno == ENODATA || errno == ENOENT)
                    break;
                if (errno == EINTR)
                    continue;
                log_event(report, "pop_tx_failed", "", -1, 0,
                          std::strerror(errno));
                break;
            }
            if (tx.len == 0 || tx.len > sizeof(tx.data))
                continue;

            SafetyFrameHeader hdr{};
            if (tx.len >= SAFETY_HEADER_SIZE)
                std::memcpy(&hdr, tx.data, SAFETY_HEADER_SIZE);
            if (!write_all(uart.get(), tx.data, tx.len)) {
                log_event(report, "uart_tx_failed", frame_type_name(hdr.type),
                          hdr.sequence_id, tx.len, std::strerror(errno));
                continue;
            }
            log_event(report, "pop_tx_uart", frame_type_name(hdr.type),
                      hdr.sequence_id, tx.len);
        }
    }

    std::cout << "safety-linkd: stopped\n";
    return 0;
}
