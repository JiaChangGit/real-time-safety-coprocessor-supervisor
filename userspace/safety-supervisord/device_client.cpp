// device_client.cpp - LIVE 模式裝置 I/O 實作

#include "device_client.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>

namespace safety {

bool DeviceClient::open_device(const std::string &device_path)
{
    int dfd = ::open(device_path.c_str(), O_RDWR | O_CLOEXEC);
    if (dfd < 0) {
        last_error_ = "failed to open device '" + device_path +
                      "': " + std::strerror(errno);
        return false;
    }
    device_.reset(dfd);
    return true;
}

ssize_t DeviceClient::read_some(int fd, uint8_t *buf, size_t cap)
{
    for (;;) {
        ssize_t n = ::read(fd, buf, cap);
        if (n < 0 && errno == EINTR)
            continue; // 被 signal 打斷，重試
        return n;
    }
}

bool DeviceClient::write_all(int fd, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            last_error_ = std::string("write failed: ") + std::strerror(errno);
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool DeviceClient::send_out(const uint8_t *data, size_t len)
{
    return write_all(device_.get(), data, len);
}

bool DeviceClient::write_device(const uint8_t *data, size_t len)
{
    return write_all(device_.get(), data, len);
}

} // namespace safety
