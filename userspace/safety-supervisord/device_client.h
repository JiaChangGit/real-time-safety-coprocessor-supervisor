// device_client.h - 輸入/輸出抽象與 RAII fd 包裝
//
// 提供：
//   - UniqueFd：對 POSIX file descriptor 的 RAII 包裝（move-only，析構自動 close）。
//   - DeviceClient：LIVE 模式下只對 --device 開檔/讀寫。UART 由 safety-linkd
//     獨占，supervisord 透過 /dev/safety_copro 收送完整 frame。
//
// MOCK 與 REPLAY 模式不需開真實裝置，分別由 mock/replay 模組直接讀檔處理。

#ifndef SAFETY_DEVICE_CLIENT_H
#define SAFETY_DEVICE_CLIENT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <unistd.h>
#include <utility>

namespace safety {

// move-only RAII file descriptor。
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}

    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    UniqueFd(UniqueFd &&o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    UniqueFd &operator=(UniqueFd &&o) noexcept
    {
        if (this != &o) {
            reset();
            fd_ = o.fd_;
            o.fd_ = -1;
        }
        return *this;
    }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

    // 釋放所有權，回傳原 fd（呼叫端負責 close）。
    int release()
    {
        int f = fd_;
        fd_ = -1;
        return f;
    }

    void reset(int fd = -1)
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

// LIVE 模式裝置客戶端。
class DeviceClient {
public:
    DeviceClient() = default;

    // 開啟 device（O_RDWR，必要）。
    // 回傳 false 並設定 last_error() 文字（英文）表示失敗。
    bool open_device(const std::string &device_path);

    int device_fd() const { return device_.get(); }

    // 從某 fd 讀取一段位元組到 buf；回傳實際讀到的位元組數，0=EOF，-1=錯誤。
    ssize_t read_some(int fd, uint8_t *buf, size_t cap);

    // 將一個完整 frame 寫入 device TX queue；linkd 會透過 ioctl POP_TX 轉送 UART。
    bool send_out(const uint8_t *data, size_t len);

    // 將位元組寫入 device（write() = ingest 給 kernel）。
    bool write_device(const uint8_t *data, size_t len);

    const std::string &last_error() const { return last_error_; }

private:
    bool write_all(int fd, const uint8_t *data, size_t len);

    UniqueFd device_;
    std::string last_error_;
};

} // namespace safety

#endif // SAFETY_DEVICE_CLIENT_H
