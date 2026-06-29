// gen_fault_log.cpp - 產生 tests/fixtures/fault_log.bin 的小型產生器
//
// 用法：g++ -std=c++17 -I../../userspace/common gen_fault_log.cpp -o gen && ./gen fault_log.bin
//
// 內容（依規格）：~10 個 HEARTBEAT（seq 遞增、ts +100ms），2 個 TELEMETRY，
// 然後一個 HEARTBEAT GAP（ts 跳 +600ms 觸發 timeout），接著一個 CHECKSUM-CORRUPT
// frame（header 合法但 checksum 錯），再一個 UNKNOWN-TYPE frame，最後更多
// HEARTBEAT（recovery）。所有輸出英文。

#include "safety_protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static void append_frame(std::vector<uint8_t> &buf, uint8_t type, uint16_t seq,
                         uint32_t ts_ms, const void *payload,
                         uint16_t payload_len, bool corrupt)
{
    SafetyFrameHeader hdr;
    safety_frame_init(&hdr, type, seq, payload_len, ts_ms);
    hdr.checksum = safety_frame_compute_checksum(&hdr, payload, payload_len);
    if (corrupt)
        hdr.checksum ^= 0xDEADBEEFu; // 蓄意破壞 checksum

    size_t off = buf.size();
    buf.resize(off + SAFETY_HEADER_SIZE + payload_len);
    std::memcpy(buf.data() + off, &hdr, SAFETY_HEADER_SIZE);
    if (payload && payload_len)
        std::memcpy(buf.data() + off + SAFETY_HEADER_SIZE, payload, payload_len);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <output.bin>\n", argv[0]);
        return 2;
    }

    std::vector<uint8_t> buf;
    uint16_t seq = 0;
    uint32_t ts = 1000;
    uint32_t beat = 0;

    auto hb = [&](uint32_t t) {
        SafetyHeartbeatPayload p{};
        p.uptime_ms = t;
        p.beat_seq = beat++;
        append_frame(buf, SAFETY_FRAME_HEARTBEAT, seq++, t, &p, sizeof(p),
                     false);
    };

    // 1) ~10 個正常 HEARTBEAT，ts 每 +100ms。
    for (int i = 0; i < 10; ++i) {
        hb(ts);
        ts += 100;
    }

    // 2) 2 個 TELEMETRY。
    for (int i = 0; i < 2; ++i) {
        SafetyTelemetryPayload p{};
        p.temperature_c_x10 = 372;
        p.voltage_mv = 3300;
        p.cpu_load_pct = 42;
        p.fault_count = 0;
        append_frame(buf, SAFETY_FRAME_TELEMETRY, seq++, ts, &p, sizeof(p),
                     false);
        ts += 100;
    }

    // 3) HEARTBEAT GAP：ts 跳 +600ms（> 350ms timeout）。
    ts += 600;
    hb(ts); // 此 heartbeat 與上一個 heartbeat 的 gap 觸發 timeout
    ts += 100;

    // 4) CHECKSUM-CORRUPT frame（header 合法、checksum 錯）。
    {
        SafetyHeartbeatPayload p{};
        p.uptime_ms = ts;
        p.beat_seq = beat++;
        append_frame(buf, SAFETY_FRAME_HEARTBEAT, seq++, ts, &p, sizeof(p),
                     true /* corrupt */);
        ts += 100;
    }

    // 5) UNKNOWN-TYPE frame（type=0x7F，header 合法、checksum 正確）。
    {
        uint8_t dummy[4] = {0xAA, 0xBB, 0xCC, 0xDD};
        append_frame(buf, 0x7F, seq++, ts, dummy, sizeof(dummy), false);
        ts += 100;
    }

    // 6) 更多 HEARTBEAT（recovery）：使 FSM 由 RECOVERING 回到 HEALTHY。
    for (int i = 0; i < 5; ++i) {
        hb(ts);
        ts += 100;
    }

    FILE *f = std::fopen(argv[1], "wb");
    if (!f) {
        std::fprintf(stderr, "error: cannot open output '%s'\n", argv[1]);
        return 1;
    }
    std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    std::fprintf(stderr, "wrote %zu bytes to %s\n", buf.size(), argv[1]);
    return 0;
}
