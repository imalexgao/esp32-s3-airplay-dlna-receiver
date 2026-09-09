# ESP32-S3 AirPlay2 Receiver Pro — AirPlay 2 接收器固件（ESP32-S3 + USB 声卡）

把一块 **ESP32-S3** 开发板变成 **AirPlay 2 接收器**：手机 / iPad / Mac 通过 AirPlay 投流，ESP32-S3 通过 **USB Host** 接口把 PCM 音频流给外接的 **USB 声卡（或自带 USB 解码的音箱，例如作者使用的KEF EGG）**，由音箱解码输出。

- 开机即开热点 **`esp32-airplay2_setup`**（无密码），手机连上后打开 **http://192.168.4.1** 完成配网
- 完全汉化（可选 English）、深色近黑的 Web 配置后台
- 手机 AirPlay 音量与设备音量**独立控制**（设备音量可作最大音量上限）
- 支持 AirPlay 2（默认）与 AirPlay 1 兼容模式（遥控切歌需要 v1）
- **v1.1**：输出格式选择（44.1/48/96 kHz × 16/24 bit，Windows 式音质档位）、后台中/英语言切换、低延时模式（180/60/20 ms）

---

## 基于的项目

| 项目 | 作用 |
|---|---|
| [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32) | 独立实现的 AirPlay 2 协议栈（RTSP/HAP/SRP/DACP/mDNS 等） |
| [wasdwasd0105/airplay-esp32-usb](https://github.com/wasdwasd0105/airplay-esp32-usb) | USB Host 音频输出分支（本工程 fork 自它） |
| RemoteMapper-ESP32 | Web 配置页的交互思路参考 |

> **许可证**：上游为非商业用途许可（见上游 LICENSE）。个人 / DIY 使用没有问题，商用前请确认授权。

---

## 优势与改进（对比其他项目）

本工程不是简单换皮：**"AirPlay 投到自带 USB 解码的音箱、音量还正常、遥控器还能用"这三件事，上游和同类 ESP32 接收方案都没有做全**。下面每一项都是实测验证过的（KEF EGG 音箱 + ESP32-S3 N16R8）。

### 快速对比

| 能力 | 上游 airplay-esp32-usb | 本工程（Pro） |
|---|---|---|
| 声道型 USB 声卡音量（EGG 类） | ✗ 只写 master，设备假接受真忽略 → 声音小 | ✓ 三通道写入，开箱正常音量 |
| 手机音量 / 设备音量 | 单一音量互相覆盖 | ✓ 独立两级增益，设备音量可作上限 |
| 默认输出格式 | 44.1 kHz（EGG 实测小声 + 拉音量条可致异常关机） | ✓ 48 kHz / 24-bit（与 Windows 默认一致） |
| 后台界面 | 英文 | ✓ 完全汉化、深色近黑 |
| 实时音频状态 | ✗ | ✓ 「音频编码状态」卡片（设备/UAC/采样率/位深/峰值/音量） |
| 遥控器音量键 | 布局错位 | ✓ 实测修正（±2dB，与后台滑块联动） |
| 播放/暂停/上一首/下一首 | ✗ | ✓ DACP 传输键（AirPlay v1 模式） |
| 遥控布局适配 | 写死单一布局 | ✓ 后台可切换 苹果标准 / KEF EGG（专版锁 KEF） |
| 输出格式选择 | 固定 44.1 kHz | ✓ v1.1 六档（44.1/48/96 kHz × 16/24 bit，Windows 式音质档位） |
| 后台语言 | 仅英文 | ✓ v1.1 中文 / English 可切换 |
| 低延时模式 | ✗ | ✓ v1.1 三档（180/60/20 ms 预滚） |
| 声卡诊断 | ✗ | ✓ `/api/desc` 完整描述符 + 声道级音量回读 |
| 预编译固件 | 单一 | ✓ 通用版 + KEF EGG 专版双版本 |
| 热点配网 | 英文名 | ✓ `esp32-airplay2_setup` + captive portal |

### 1. 根治"AirPlay 声音小"——声道级音量写入（核心修复）

这是本工程最重要的修复。带 USB 解码的音箱（如 KEF EGG）的 USB 描述符里，Feature Unit 的 **master 通道只声明了 MUTE，真实音量增益在 L/R 声道（ch1/ch2）**：

```
KEF EGG FU3: bControlSize=1
  master (ch0) = 0x01  → 只有 MUTE
  ch1 (左)     = 0x02  → VOLUME  ← 真实增益在这里
  ch2 (右)     = 0x02  → VOLUME  ← 真实增益在这里
```

- **上游代码只往 master 写 0dB** → EGG 不报错、回读也是 0dB（假装接受），但实际忽略 → **真实增益一直卡在设备存储的低值，怎么调都小声**。
- **为什么电脑上声音正常？** Windows 的通用免驱驱动会正确读描述符，把音量写到声道上，所以电脑端一直正常。
- **本工程修复**：音量写入强制覆盖 **master + L + R 三通道**（`uac_fu_set_volume_db`），并对严格设备（STALL）做"任一通道成功即成功"容错。0dB 下数学上纯衰减，不会削波/爆音。

### 2. 手机音量与设备音量解耦

音频通路里两级**独立的数字增益**相乘：`最终 = PCM × 手机AirPlay音量 × 设备音量`。

- 网页滑块 / 遥控器 → **设备音量**（-30..0 dB，持久化，可作**最大音量上限**）
- iOS 音量条 → **AirPlay 音量**（跟随连接）
- 两者互不覆盖：手机调音量不会动后台音量条，网页调音量也不影响手机滑块
- 两级均钳 ≤ 0 dB、纯衰减 → 从数学上杜绝爆音（上游是单一音量互相覆盖）

### 3. 默认 48 kHz / 24-bit 输出（修正上游硬伤）

- 上游把 USB Host 输出采样率**硬覆盖为 44.1 kHz**。
- 实测 KEF EGG 在 44.1 kHz 下**声音明显变小，且拉音量条可能导致音箱异常关机**；48 kHz / 96 kHz 正常。
- 本工程移除该硬覆盖，默认 **48 kHz / 24-bit**——与 Windows 默认输出规格一致（24-bit 子槽 = 3 字节，左对齐扩展，有效位不缺）。

### 4. 遥控器全功能

- **音量键**：实测修正 KEF EGG 的 byte[1] 布局（与苹果不同），±2 dB 控制设备音量，与后台滑块联动——像电脑上用遥控器调系统音量一样。
- **播放/暂停、上一首、下一首**：byte[2] 边沿检测 + DACP 协议（AirPlay v1 模式实测可用）。
- **遥控布局后台可配置**：苹果标准 / KEF EGG 一键切换（两种布局共用比特位但含义相反，不能自动识别，所以做成用户设置，避免坑到不同设备用户）。

### 5. 完全汉化、深色近黑的后台 + 实时音频状态

- 全中文界面，深色主题（护眼、近黑）。
- **「音频编码状态」卡片**实时显示：设备名、UAC 版本、采样率、位深、声道、流状态、输出电平峰值、AirPlay 音量、设备音量——一眼看出 USB 声卡实际工作参数。

### 6. 声卡诊断接口（排查问题用）

- `GET /api/desc`：完整 USB 配置描述符 dump（十六进制 + 解析后的音频控制实体），用来判断声卡增益/控制分布。
- `GET /api/audio/usb`：声道级音量回读（`fu_ch1_db` / `fu_ch2_db`）、采样率回读等字段。

### 7. 双版本发布

- **通用版**：遥控布局后台可切换（默认苹果标准），热点 `esp32-airplay2_setup`——大多数用户推荐。
- **KEF 有源音箱专版（带 USB 解码器）**：遥控布局编译期写死为 KEF 遥控器键位（如 KEF EGG），后台不显示布局选择器；其余功能（格式选择 / 语言 / 低延时）与通用版完全一致。KEF 有源音箱（带 USB 解码器 + 原装遥控器）用户开箱即用。

### 8. 配网体验

- 上电自动开热点 `esp32-airplay2_setup`（无密码），手机连上自动弹配置页（captive portal）。
- 连上家里 WiFi 后热点自动关闭；WiFi 失联自动重新开热点，方便再次配网。
- 支持热点改名、密码、开关（后台可配置）。

### 9. v1.1：输出格式选择（Windows 式音质档位）

像 Windows 声音设置一样，后台可自由选择采样率 × 位深：

| 档位 | 采样率 | 位深 | 标注 |
|---|---|---|---|
| 0 | 44.1 kHz | 16 bit | CD 音质 |
| 1 | 48 kHz | 16 bit | DVD 音质 |
| 2 | 44.1 kHz | 24 bit | Hi-Res 入门 |
| 3 | 48 kHz | 24 bit | Hi-Res 标准（录音室音质，默认，与 Windows 默认一致） |
| 4 | 96 kHz | 24 bit | Hi-Res 高解析（实验性，USB 带宽满负荷） |
| 5 | 96 kHz | 16 bit | 实验性（高采样低精度，不推荐） |

- 声卡不支持所选组合时自动回退到最近支持项并提示。
- 点"重新应用格式"即时生效（通过 USB 热重载重新协商，免拔插、免重启）。
- ⚠️ 44.1 kHz 在部分声卡（如 KEF EGG）上实测音量偏小、拉音量条可能致异常关机——遇到请保持 48 kHz。

### 10. v1.1：后台语言切换

- 后台顶栏 中文 / EN 一键切换，全部界面文案 i18n，选择持久化（NVS），立即生效。

### 11. v1.1：低延时模式

| 模式 | 起播预滚缓冲 | 适用场景 |
|---|---|---|
| 普通（默认） | ~180 ms | 稳定优先，抗 WiFi 抖动 |
| 低延时 | ~60 ms | 声画同步、切歌后快速出声 |
| 极速 | ~20 ms | 追求最低延时，WiFi 环境要好 |

- 后台实时显示当前预滚值；切换后下一次会话生效。
- 端到端延时构成与"1 ms"的真相见下方「延时说明」。

---

## 硬件要求

- **ESP32-S3 开发板**（N8R8 / N16R8 等均可；需原生 USB-OTG 外设，ESP32 经典款不支持）
- **USB 声卡 / 自带 USB 解码的音箱**（电脑能识别为 USB 声卡即可）
- 要求声卡支持：USB Audio Class 1.0 或 2.0、PCM 16/24-bit 立体声、44.1k 或 48k（绝大多数满足）

> 如果只是调试（不接声卡），固件也能正常启动：AirPlay 会话保持，插入声卡后自动开始输出。

### 接线要点
1. **USB 声卡插在"原生 USB"口**（ESP32-S3 芯片直出的 USB-OTG，GPIO19/GPIO20，DevKitC 上通常标注 "USB"）。
2. **烧录 / 串口日志必须走另一个 USB 口**（如 UART 口 / CH343）：固件把原生 USB 口用作 USB Host 后，板载 USB-Serial-JTAG 不再可用，走外部 UART（115200）。
3. **VBUS 供电**：USB Host 模式下 VBUS 需要 5V。多数 DevKitC 把原生 USB 口的 VBUS 接到板上 5V 电源轨，总线供电的声卡可直接工作；否则请外接 5V，或在 `menuconfig → Audio Output → USB host VBUS enable GPIO` 配置 VBUS 开关。

---

## 构建与烧录

环境：PlatformIO（ESP-IDF 框架）。

```bash
# 构建（通用版）
pio run -e esp32s3-usbhost

# 构建（KEF 有源音箱专版：遥控布局锁死 KEF 键位）
pio run -e esp32s3-usbhost-kef

# 烧录固件 + SPIFFS 网页（用 UART 口连接开发板）
pio run -e esp32s3-usbhost -t upload
pio run -e esp32s3-usbhost -t uploadfs

# 串口日志（115200）
pio device monitor
```

构建产物：
- `.pio\build\esp32s3-usbhost\firmware.bin` —— 通用版应用固件（OTA 升级用这个）
- `.pio\build\esp32s3-usbhost\spiffs.bin` —— 网页资源
- `.pio\build\esp32s3-usbhost-kef\firmware.bin` / `spiffs.bin` —— KEF 专版

> 网页 OTA 只上传 `firmware.bin`；首次烧录或网页内容更新后，需用 `pio run -t uploadfs` 把 SPIFFS 一起写入。

### 预编译固件（`dist/` 目录）

仓库附带两个预编译版本，开箱即用（ESP32-S3 16MB Flash / 8MB PSRAM，分区见 `components/boards/partitions.csv`）：

| 版本 | 说明 | 适用 |
|---|---|---|
| **通用版** | 遥控布局可在后台切换（默认苹果标准），热点名 `esp32-airplay2_setup` | 大多数用户（推荐） |
| **KEF 有源音箱专版（带 USB 解码器）** | 遥控布局编译期写死为 KEF 遥控器键位（如 KEF EGG），后台无布局选择器；其余功能与通用版一致 | 使用 KEF 有源音箱（自带 USB 解码器、遥控器，如 KEF EGG）的用户，不想进后台配置遥控 |

- `ESP32-AirPlay2-通用版-firmware.bin` / `-spiffs.bin`
- `ESP32-AirPlay2-KEF有源音箱专版-firmware.bin` / `-spiffs.bin`
- `bootloader.bin` / `partitions.bin`（首次烧录用）

首次烧录（全量）：
```bash
esptool.py --chip esp32s3 -p COM3 -b 460800 write_flash \
  0x0 bootloader.bin 0x8000 partitions.bin \
  0x20000 ESP32-AirPlay2-通用版-firmware.bin \
  0x620000 ESP32-AirPlay2-通用版-spiffs.bin
```
已有固件只升级应用：仅烧 `0x20000` 段的 `firmware.bin`（或直接网页 OTA）。

---

## 首次使用（配网）

1. 开发板上电，等待约 5 秒出现热点 **`esp32-airplay2_setup`**（无密码）。
2. 手机连接该热点 → 自动弹出配置页（captive portal）；没弹出就手动访问 **http://192.168.4.1**。
3. 在 **WiFi 网络** 卡片点"扫描网络"，选中家里 WiFi，输入密码，点"连接 WiFi"。
4. 保存后自动重启并连接家里 WiFi（此时热点自动关闭；WiFi 连不上时会重新开热点，方便再次配网）。
5. 手机连回家里 WiFi，打开控制中心 AirPlay 图标，选择 **ESP32-AirPlay2**。

> 首次连接 iOS 设备可能要求输入 AirPlay 配对码：配对码打印在串口日志 / 日志页面中。

---

## 兼容边界与已知特例

### ① KEF EGG：真实音量在声道上
EGG 的 master 通道只有 MUTE，音量增益在 ch1(L)/ch2(R)。固件已做三通道写入，**开箱即用**；如果遇到"怎么都小声"的声卡，可用 `GET /api/desc` 查看描述符里 FU 的 `bmaControls` 分布。

### ② 遥控布局：苹果标准 vs KEF EGG（需在后台选择）
| 布局 | byte[1] 位图 | byte[2] |
|---|---|---|
| 苹果标准（默认） | 播放/暂停=0x01，音量+=0x02，音量-=0x04 | 不使用 |
| KEF EGG | 音量+=0x01，音量-=0x02 | 播放/暂停=0x20，下一首=0x40，上一首=0x80 |

两种布局在 byte[1] 上**共用比特位但含义相反**，无法自动区分。**默认苹果标准（上游行为）**；使用 KEF EGG 遥控器请在后台「设备设置 → 遥控布局」切换为 KEF EGG。

### ③ AirPlay v1 / v2
- 传输键（播放/暂停、上一首、下一首）走 DACP，**只有 AirPlay v1 模式可用**（AirPlay 2 下 iOS 改用 MRP，本工程未实现）
- 音量键两种模式都可用（本地设备音量）
- **切换模式后手机可能连不上 / 连上无声**：手机端缓存了旧服务（`_airplay` v2 / `_raop` v1），多试几次或手机端断开重连、忘掉设备即可恢复

### ④ 采样率 / 输出格式
- 默认 48 kHz / 24-bit 输出（源 44.1 kHz 自动重采样）；v1.1 起后台可自由选择 44.1/48/96 kHz × 16/24 bit（见「v1.1：输出格式选择」）
- 部分声卡在 44.1 kHz 下异常（EGG 实测小声 + 拉音量条可能关机），如遇此类问题请保持 48 kHz

### ⑤ 电源
- USB 声卡由开发板 VBUS 供电时，务必确认 VBUS 有 5V（见"接线要点"）

---

## 目录结构（与上游一致）

```
main/               AirPlay 2 协议栈 + 应用逻辑（rtsp/ hap/ plist/ dacp/ audio/ network/）
  audio/audio_output_usb_host.c    USB Host 音频输出后端（UAC1/2 等时流 + 音量/遥控/格式协商）
  network/wifi.c                   AP+STA、热点配网、断线自动恢复
  network/web_server.c             Web API（含 /api/audio/usb、/api/desc、/api/remote/layout、/api/audio/format、/api/audio/latency、/api/ui/lang、/api/usb/reprobe）
  settings.c                       NVS 设置（音量/遥控布局/输出格式/语言/延时模式）
data/www/           SPIFFS 网页（index.html，中英双语）
components/         板级支持
config/             sdkconfig 分层配置
```

## 版本历史

见 [CHANGELOG.md](CHANGELOG.md)。v1.0：音量根治 + 解耦 + 遥控器全功能 + 双版本；v1.1：输出格式选择 + 中英双语 + 低延时模式 + KEF 专版同步升级。

---

## 延时说明（AirPlay 端到端延时）

**结论先行：端到端（手机点击 → 音箱出声）延时由 AirPlay 协议 + WiFi + 缓冲决定，本固件与其他 AirPlay 接收器同量级（估计 150–300 ms，可调至更低）；"1 ms"级别的指标只可能指多设备之间的同步误差，不是端到端延时。**

### 两个不同的"延时"

| 指标 | 含义 | 现实水平 |
|---|---|---|
| **端到端延时** | 手机按播放/调音量到声音出来 | AirPlay 1 接收器普遍 100–200 ms 缓冲；本固件估计 150–300 ms |
| **多设备同步误差** | 两台音箱同时播放时的相互偏移 | AirPlay 2 用 NTP 时钟同步，可做到毫秒级（这是市售"1ms"宣传的真实含义） |

- 市售成品（如"audiocube"）宣传的"QQ音乐同步误差 1ms"，指的是**多音箱之间的同步偏差**（AirPlay 2 时钟同步），并且通常只在 AirPlay 2 多房间模式下有意义——不是"点击到出声"的延时。
- 本固件 **v2 模式**同样走 NTP 时钟同步（`audio_timing`/NTP），多设备同步机制相同；**v1 模式**（RAOP）是单音箱直连，无多房间同步（AirPlay 1 协议本身没有）。

### 本固件的端到端延时构成（估算）

```
iOS 编码 + WiFi 传输         ~30–80 ms   （苹果设备侧，接收器无法控制）
RTP 接收 → 播放缓冲          ~100 ms 级  （接收器主要可控项：抗 WiFi 抖动）
ALAC 解码 + 重采样           ~5–15 ms
USB 等时输出（1 ms/帧）       ~1–3 ms
音箱 USB 解码器内部缓冲        ~10–30 ms  （设备侧）
─────────────────────────────────────────
合计                          ~150–300 ms
```

### 能做到多低？

- **端到端 1 ms 物理上不可能**：2.4 GHz WiFi 包抖动本身就大于 1 ms，USB 全速帧就是 1 ms，iOS 编码侧还有几十 ms。任何 AirPlay 接收器（包括 Apple 自家 HomePod）都做不到端到端 1 ms。
- **可以优化**：v1.1 已提供**低延时模式**（普通 180 ms / 低延时 60 ms / 极速 20 ms 三档预滚缓冲，后台可切），目标端到端 **~120–150 ms**，代价是 WiFi 波动时更易出现卡顿/爆音。默认普通档，稳定优先。
- 我们**不做**"1ms"这种无法兑现的宣传，但可以公开后台的实时缓冲 / 预滚值（真正可验证的指标）。

---

## 常见问题

- **没声音？** 先看后台「音频编码状态」是否显示已连接；再看日志中 `audio_uac_host` 的 Streaming 信息。若显示 `No stereo PCM iso OUT alt-setting found`，说明声卡不是标准 UAC 输出设备。
- **声音小？** 确认固件为三通道音量写入版本；用 `GET /api/audio/usb` 看 `fu_ch1_db/fu_ch2_db` 是否为 0 dB。若为负值说明声卡增益在声道上。
- **遥控键位错乱？** 通用版：后台「遥控布局」切换苹果标准 / KEF EGG；KEF 专版已锁死 KEF 键位。
- **想换输出格式？** 后台「音频输出」选择 44.1/48/96 kHz × 16/24 bit，点"重新应用格式"即时生效；44.1 kHz 在部分声卡上异常（见兼容边界④）。
- **切歌 / 播放暂停无效？** 切到 AirPlay v1 模式。
- **热点连不上？** 确认烧录的是 `esp32s3-usbhost` 环境；连上家里 WiFi 后热点自动关闭属正常设计。
- **AirPlay 列表里看不到设备？** 确认手机与开发板同一 WiFi，设备名 / mDNS 正常（默认 `ESP32-AirPlay2`）。
