# Protocol

Canonical source 是 `include/safety_protocol.h`。Consumer copies 必須 byte-identical：

```text
userspace/common/safety_protocol.h
linux/drivers/safety_copro/safety_protocol.h
zephyr/safety_copro_firmware/include/safety_protocol.h
```

驗證方式：

```sh
cmp include/safety_protocol.h userspace/common/safety_protocol.h
cmp include/safety_protocol.h linux/drivers/safety_copro/safety_protocol.h
cmp include/safety_protocol.h zephyr/safety_copro_firmware/include/safety_protocol.h
```

Frame header 固定 16 bytes，payload 最大 256 bytes。CRC-32 與 Python `binascii.crc32` 相容。

Protocol 層提供的欄位：checksum、version、sequence ID、timestamp、ACK/NACK frame type。Retry handling、timeout detection、duplicate detection 由 kernel driver 與 supervisor 實作，不屬於 protocol 定義。
