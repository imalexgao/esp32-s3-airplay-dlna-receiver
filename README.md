# ESP32-S3 AirPlay2 + DLNA Receiver

基于 ESP32-S3（N16R8）的**双协议音频接收器**：同一块开发板同时提供 **AirPlay 2** 与 **DLNA/UPnP** 推流能力，把手机/电脑/电视/功放推来的音频交给 **USB 声卡（音箱）** 解码输出。

- **iPhone / iPad / Mac / Apple 生态** → AirPlay 2
- **安卓手机 / 第三方播放器（网易云、Plexamp、BubbleUPnP 等）** → DLNA/UPnP
- 无需模拟输出，**USB 口直连**自带 USB 解码的音箱（如 KEF EGG 等有源音箱）即插即用

> **姊妹项目**：本项目的 AirPlay 协议栈源自 [ESP32-S3 AirPlay2 Receiver Pro](https://github.com/imalexgao/esp32-s3-airplay2-receiver-pro)（纯 AirPlay 单协议、更轻量）；本项目在其基础上扩展了 DLNA/UPnP 与多格式解码，为独立的双协议项目。单协议轻量版与双协议全能版各自独立发布、互不干扰。

---

## 硬件要求

| 组件 | 要求 |
|------|------|
| 开发板 | ESP32-S3 N16R8（16MB Flash / 8MB PSRAM），USB-OTG 支持 |
| 音箱 | 自带 USB 解码器、可被识别为通用 USB 声卡的有源音箱 |
| 供电 | 建议 5V/2A 独立供电（USB 声卡 + WiFi 同时工作时电流较大） |

> 已实机验证：KEF EGG（USB 解码模式）——音量三通道强制覆盖、遥控器键位等特例均已适配，见下方「已知特例」。

## 功能总览

### 网络与配网
- 上电自动开启热点 `esp32-airplay2_setup`（无密码，仅用于配网）
- 后台 `http://192.168.4.1`：配置家庭 WiFi、热点开关/名称/密码、设备名
- 连接家庭 WiFi 后可通过局域网 IP 访问后台（DHCP 分配）

### AirPlay 2
- 原生 AirPlay 2（兼容 AirPlay 1 模式切换）
- 48kHz 通告、音量独立控制（手机音量与设备后台音量互不干扰）
- 低延迟同步（多音箱同步场景）

### DLNA / UPnP
- UPnP AV MediaRenderer（SSDP 发现 / ConnectionManager / AVTransport）
- 控制点（网易云、Plexamp 等）可发现、可推流、可控制播放
- **双协议并发**：AirPlay 与 DLNA 同时可用；同一时刻后发者抢占（AirPlay 恢复时自动抢回）

### 支持的音频格式（DLNA 解码矩阵）

| 格式 | 容器/说明 | 解码器 | 采样率/位深 |
|------|-----------|--------|-------------|
| WAV | LPCM 直通 | — | 任意（声卡支持范围） |
| MP3 | MPEG Audio | minimp3 | 44.1k/48k |
| AAC | ADTS | FAAD2 | 44.1k/48k |
| FLAC | FLAC | dr_flac | 44.1k/48k，16/24bit |
| OGG | Vorbis | stb_vorbis | 44.1k/48k |
| ALAC | M4A (Apple Lossless) | libalac | 44.1k/48k，16/24bit |

> 均已在板端实测解码验证（ALAC 三条规格线 44.1k×16、44.1k×24、48k×24 全量 0 失败）。

### 后台界面
- 中/英双语，深色主题
- 音量条（设备端，独立于手机音量）、音频编码状态（当前 USB 声卡实际编码情况）
- **推流协议实时显示**：当前正在发声的是 AirPlay 2 / DLNA / 空闲，随抢断实时切换
- 采样率/位深选择（44.1k/48k × 16/24bit，重启生效）
- 延时模式（普通 180ms / 低延时 60ms / 极速 20ms 预滚，下次播放生效）
- 遥控布局选择：苹果标准 / KEF EGG
- OTA 在线升级（后台直接上传固件）
- 设备信息、格式状态诊断

## 兼容边界与已知特例

1. **KEF EGG 音量在三个声道上**（L/R/Sub），需强制三通道覆盖才有正常响度；其他音箱一般为双声道，行为不同。
2. **KEF EGG 遥控器键位**与苹果标准不同：音量键在 V2 下有效；播放/暂停/上/下一首在部分 App（AAC 静态直推）下不生效，属协议层已知边界。
3. **96kHz 采样率**不支持（会导致 USB 枚举异常），后台已移除该选项。
4. **APE** 暂不支持（Plex 不支持、BubbleUPnP 会转码为 WAV、ESP32 无成熟嵌入式 APE 解码器）。
5. 遥控器对音量的控制与手机音量**解耦**：遥控器/后台音量只作用于设备端，手机音量独立。

## 版本历史

### v1.0.0（里程碑 · 稳定版）
- **无 QPlay 标签**的稳定基线：iOS/安卓跨协议抢断正常、双 iOS 互抢可逼退、音量记忆正常
- 含：AirPlay 2 + DLNA/UPnP、ALAC/OGG/FLAC/AAC/MP3/WAV 六格式解码、音量解耦与记忆、三通道音量覆盖、遥控布局（苹果标准/KEF EGG）、双语后台、延时模式、OTA、推流协议实时显示
- 后续若探索 QPlay 等实验性功能，可随时 `git checkout v1.0.0` 退回此里程碑

## 构建

```bash
# PlatformIO + ESP-IDF 5.5
pio run -e esp32s3-usbhost
# 产物：.pio/build/esp32s3-usbhost/firmware.bin
```

首次编译会自动拉取 `managed_components`（espressif/esp_audio_codec 等）。

## 烧录

```bash
pio run -e esp32s3-usbhost -t upload   # 串口烧录
# 或后台 OTA（配网完成后）：上传 firmware.bin 至 /api/ota
```

## 借鉴与致谢

本项目借鉴了以下开源项目与库：

| 组件 | 来源 | 许可 |
|------|------|------|
| AirPlay 2 协议栈 | [ESP32-AirPlay2](https://github.com/ESP32-AirPlay2) 生态（开源 AirPlay 实现） | 见上游 |
| USB 主机/声卡 | ESP-IDF USB Host + TinyUSB | Apache-2.0 |
| AAC 解码 | FAAD2 | GPL-2.0 |
| FLAC 解码 | dr_flac | Public Domain |
| Vorbis 解码 | stb_vorbis | Public Domain |
| MP3 解码 | minimp3 | Public Domain |
| ALAC 解码 | [Apple macosforge/alac](https://github.com/macosforge/alac) | Apache-2.0（已打 PSRAM 补丁） |

**相比纯 AirPlay 接收器（v1.1.5）的主要改进**：

- **双协议共存**：AirPlay 2 + DLNA 同一固件、同一后台，安卓/苹果生态都能推流
- **DLNA 六格式全覆盖**：WAV/MP3/AAC/FLAC/OGG/ALAC，含 24bit 与 48kHz
- **音量体系重构**：设备端音量与手机音量完全解耦，后台可独立限制最大音量，音量记忆
- **USB 声卡三通道覆盖**：解决部分音箱（KEF EGG）AirPlay 下响度过低的问题
- **KEF EGG 遥控器键位适配**：后台可切换苹果标准/EGG 布局（或编译 KEF 专版锁死）
- **推流协议实时显示**、延时模式（20/60/180ms 预滚）、OTA 在线升级、完整汉化 + 中英双语后台

## License

本项目开源许可遵循各组件许可；整体代码以 MIT 发布（除上述第三方组件保留其各自许可）。

---

*ESP32-S3 AirPlay2 + DLNA Receiver —— 一个板子，全家桶投送。*
