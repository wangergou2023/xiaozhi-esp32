# Kitty 电子宠物 · 功能设计

> 状态：草案。本文只描述设计与范围，不含实现。
> 目标板卡：Spotpear ESP32-S3-0.85-AI（本 fork 已有支持）
> 产品方向：**A 陪伴 + B Agent 的身体 + C 养成 + D 夜间陪伴**（一个产品，四条线）

## 1. 定位

一台离线自足的桌面电子宠物：0.85 寸屏上的 Kitty，会困、会饿、会自己找你，
不依赖任何云端服务，不需要账号与激活。

插上电脑时，她把控制权交给本地 agent（例如 WorkBuddy），成为那个 agent 的"身体"。

而她的身体是**有代价的**：agent 干活消耗她的精力与耐心，你必须亲自照顾她才能让她撑下去。
这条把"挂件"和"养成"咬在一起——她不是显示器，是一只需要经营的动物。

一句话：**拔网线、断云端、公司跑路，她照样活着；而她的状态，是你和 agent 共同经营出来的；
到了晚上，她安静下来陪你睡觉。**

### 四条线为什么能并存

| 线 | 本质 | 与其他线的关系 |
| --- | --- | --- |
| A 陪伴 | 底座人格与表现层 | 没它 B/C/D 都是空壳 |
| B Agent 的身体 | 一条传输通道 | 纯管道，不占玩法 |
| C 养成 | 叠在 A 上的数值与成长 | 靠 A 的表情与 D 的作息表达 |
| D 夜间 | A 的一个时段模式 | **不是独立产品**，是 A 的行为子集 |

唯一风险是范围最大、工期最长；对策见 §14 的排期顺序，每个阶段结束设备都可长期使用。

## 2. 目标与非目标

### 目标

- 离线完整可玩：无网络、无账号、无激活流程
- 插电脑后可由本地 agent 控制：表情、说话、读状态、接收输入事件
- 养成闭环：饥饿 / 精力 / 清洁 / 亲密度四条数值，随时间衰减，需要人照顾
- agent 与养成的双向影响：agent 消耗她的状态，她的状态反作用于 agent 的行为
- 夜间陪伴：到点安静下来，微光 + 呼吸节律 + 白噪音，早上轻声唤醒
- 状态连续：模式切换不丢存档
- 在 128×128 屏、一颗灯、两个键的硬约束下把表达做满

### 非目标（明确不做）

- 云端 LLM 对话、语音助手、语音识别转文字
- 小智云协议、设备激活、云端 OTA、云端资产下发
- 触摸交互、摄像头、多机联网对战
- 手机 App

## 3. 硬件基线

| 项 | 规格 |
| --- | --- |
| SoC | ESP32-S3R8，8MB PSRAM |
| 屏幕 | GC9107，128×128 SPI（旋转由帧数据在主机侧完成） |
| 音频 | ES8311 codec + NS4150B 功放，单麦单喇叭，16 kHz |
| 灯光 | 1× WS2812B（GP13） |
| 按键 | BOOT（GP0）、POWER（GP2），**无触摸** |
| 传感器 | **无 IMU**（不能摇晃） |
| 引脚 | 见 `main/boards/spotpear/sp-esp32-s3-0.85-ai/config.h` |

未决：见 §13。

## 4. 三种身份

设备始终处于以下三态之一，切换有明确条件，且**状态向量跨模式连续**。

| 模式 | 进入条件 | 退出条件 | 行为 |
| --- | --- | --- | --- |
| `autonomous` | 上电默认；或 agent 断连超时 | 检测到 USB 供电 / 配网触发 | 完全自主：表情、行为调度、按键互动、数值衰减 |
| `docked` | 检测到 USB 供电且未握手 | 拔线；或 host 握手成功 | 充电态：睡觉、打呼、电量表达；数值仍衰减但减缓 |
| `remote` | host 完成 `pet.hello` 握手 | 拔线；或心跳超时 3 s | 表现层移交 agent；行为调度器降级为"待命"；数值照常衰减 |

**关键决定：插电不等于交权。** 插上 USB 只进入 `docked`；只有 host 主动握手才进入 `remote`。
否则插个充电宝，宠物就被强行接管了。

## 5. 分层架构

```
main/pet/
  pet_core.cc       状态向量、衰减、成长阶段、存档（NVS）
  behavior.cc       行为调度器：tick、按权重挑动作
  expression.cc     状态 → 表情行 / 音效 / 灯色 / 小动作
  action.cc         动作原语执行器：唯一允许直接碰硬件的层
  agent_link.cc     USB CDC 行式 JSON-RPC，暴露 pet.*
main/gateway/       SoftAP 配网页（短生命周期，写完参数即关射频）
main/boards/       只做接线与初始化，不含业务逻辑
```

**边界规则**：`boards/` 目录下不出现业务逻辑。历史上 `feba432` 把 456 行的 GLM 客户端塞进
`boards/spotpear/...`，绕开了协议层与配置层，导致该能力既无法复用也无法关闭——本次设计不重复。

## 6. 养成数值

四条数值，0–100，随 tick 衰减，定期落盘 NVS。

| 数值 | 衰减诱因 | 补充方式 | 影响 |
| --- | --- | --- | --- |
| `hunger`（饱食） | 时间；agent 高强度工作 | 喂食 | 低了表情发蔫、动作变慢 |
| `energy`（精力） | 清醒时长；**agent 每完成一次任务扣** | 睡觉、docked 充电 | 见底强制入睡，拒绝 pet.* |
| `clean`（清洁） | 时间 | 清洁动作 | 低了掉毛、灯色发暗 |
| `affection`（亲密度） | 长期不理；被忽视的 pet.poke | 抚摸、陪玩、完成任务 | 决定主动行为频率与成长 |

### 成长阶段

由 `affection` 与总互动次数决定，共 3 阶段。每阶段解锁表情行与音效：
幼体（动作笨拙、多睡）→ 成长（解锁主动搭话）→ 成熟（解锁全部表情与稳定人格）。

**不做"养死"**。长时间不管，她只会进入"闹脾气"状态——变冷淡、不回应用、数值停在上限以下，
但一次认真照顾就能拉回来。理由是：一台坏掉的桌面设备没有意义，情绪上的疏远比数值死亡更有力。

### 与 agent 的咬合

| 事件 | 效果 |
| --- | --- |
| agent 完成任务 | `energy -= 5`，`affection += 2`，播放"完成"表情 |
| agent 报错 / 被打断 | `energy -= 8`，播放"炸毛"表情，`affection -= 1` |
| agent 长时间思考 | 转眼睛、抓耳，`energy` 缓慢下降 |
| `energy` 见底 | 强制 `pet.sleep`，并在 `pet.status` 里报告"累了"给 host |

这是本设计的核心：**agent 的能力受她的体力约束**，而不是她去蹭 agent 的算力。

## 7. 动作原语

固件只暴露有限的动词，离线调度器与远程 agent **共用同一套**。两套大脑、一套身体。

| 原语 | 参数 | 离线来源 | 在线用途 |
| --- | --- | --- | --- |
| `pet.face` | `emotion`, `intensity` | 情绪状态映射 | agent 指定 |
| `pet.say` | `clip_id` 或 `pcm` 流 | 内置音效片段 | 播放 host 推来的 PCM |
| `pet.look` | `dx`, `dy` | 眼球/头部偏移 | 同左 |
| `pet.attention` | `on` | 抬头看用户 | 同左 |
| `pet.feed` / `pet.clean` / `pet.play` | — | 按键手势调用 | agent 也可代劳（省事但有上限） |
| `pet.status` | — | 返回全部数值与模式 | 同左 |
| `pet.poke` | 事件 | 按键/唤醒词触发 | 上报给 host |
| `pet.sleep` / `pet.wake` | — | 精力见底 / 靠触摸 | agent 强制 |

新增玩法优先组合既有原语；需要新原语时，先问"离线时它该做什么"——答不上来说明这个原语
把玩法绑死在 agent 上了。

## 8. 输入手势

只有一个 BOOT 键不够用，因此两个键都参与。

| 手势 | 动作 | 备注 |
| --- | --- | --- |
| BOOT 短按 | 摸摸头（`affection`） | 主交互，随时可用 |
| BOOT 双击 | 喂食（`hunger`） | 需 ~300 ms 判定窗口 |
| BOOT 长按 3 s | 进入配网模式 | 不占用短按 |
| POWER 短按 | 清洁 / 陪玩（自动选最急的一条） | 依赖 §13 未决项 1 |
| POWER 长按 | 手动进入夜间模式（见 §10.1） | |

**已知风险**：单键三种含义必然带来误触。缓解手段：
- 双击判定失败时降级为一次短按（宁可多摸一下，不要误喂）
- 喂食有饱食上限，撑不坏，只是浪费
- 灯色反馈即时确认，摸到/喂到立刻看得见

## 9. 与 agent 的协议

- **传输**：USB CDC 串口，一行一条 JSON（MCP stdio 风格的 JSON-RPC 2.0）
- **握手**：host 发 `pet.hello` → 设备回 `status` + 固件版本 + 能力表。未握手不进入 `remote`
- **心跳**：host 每 1 s 发 `pet.ping`；3 s 无输入即回 `autonomous`
- **事件**：设备主动上报，无 `id`，如 `pet.poke`、`pet.state_changed`、`pet.tired`

```json
{"jsonrpc":"2.0","id":7,"method":"pet.face","params":{"emotion":"happy","intensity":0.8}}
{"jsonrpc":"2.0","method":"pet.poke","params":{"source":"button","count":3}}
{"jsonrpc":"2.0","method":"pet.state_changed","params":{"energy":12,"hunger":40,"mood":"tired"}}
```

选择串口而非局域网的理由：USB 供电与数据同一条线，插上即有；不引入 WiFi 常开的功耗。
若将来需要无线控制，再在 `agent_link` 增加一条 WebSocket transport，动作原语与协议不变。

## 10. 离线玩法

### 行为调度

1 s tick 更新衰减；每 N 秒按权重表挑一个候选动作（张望、哼一声、伸懒腰、打瞌睡、舔爪子）。
权重由数值与成长阶段调制——饿了更爱叫，困了更容易睡，亲密度高才更爱主动凑近。

**不使用 LLM**。离线"智能"来自状态衰减 + 权重随机，成本为零、确定性可测。

### 表达

- 表情：沿用现有 57 帧 / 9 行动画，按情绪选行，按数值选强度
  （注意：素材的九行是**动作**，不是九张脸 —— 见 §16 的实现记录）
- 音效：内置短片段（叫声、哈欠、咕噜、咀嚼），不做 TTS
- LED：WS2812 表达情绪与电量，替代目前的"设备状态灯"

### 夜间陪伴

A 的时段模式，也是原方案 D 的全部内容。到点进入，不需要用户操作。

**进入**：作息时段到达（依赖时间源），或 POWER 长按手动进入，或 `docked` 且电量充满后自动。

**表现**：
- 屏幕亮度降到微光档，表情切到"睡姿"帧，只保留极慢的呼吸起伏
- 呼吸节律音（4–6 次/分的引导节奏），可选白噪音
- 打呼、翻身、偶尔咂嘴——用极低频的随机行为保持"活着"的存在感

**白噪音不占 Flash**：用算法生成（滤波噪声），几十行代码，不用存音频文件。

**禁止项**：夜间不主动行为、不主动搭话、拒绝 `pet.say` 之外的一切 agent 请求；
agent 在夜间只允许推极短的提示音（如任务完成），不允许连续输出。

**退出**：作息时段结束、轻触按键、或 agent 显式 `pet.wake`，并给出起床表情。

### 昼夜

需要绝对时间；时间源见 §13 未决项。拿不到绝对时间时退化为"仅运行时长"，
昼夜玩法整体关闭，其余不受影响。

## 11. 配网与参数

WiFi 在本设计中**只是配网射频**，不是联网能力。

- **触发**：长按 BOOT 3 s（短按/双击留给养成交互）
- **形态**：SoftAP + HTTP 配置页，3 分钟无操作自动关闭射频
- **落盘**：NVS，掉电保留

| 参数 | 说明 |
| --- | --- |
| `name` | 宠物名字（用于唤醒词与界面） |
| `volume` / `brightness` | 音量、亮度 |
| `wake_sensitivity` | 唤醒词灵敏度 |
| `sleep_window` | 作息时段（依赖时间源） |
| `personality` | 行为权重档位：黏人 / 冷淡 / 活泼 |
| `difficulty` | 数值衰减速率：轻松 / 正常 / 硬核 |
| `clock` | 手动对时（无网络时的时间源之一） |

不做 WiFi STA 联网。配网页里不需要填 SSID 密码。

## 12. 复用小智的部分

| 保留 | 位置 | 用途 |
| --- | --- | --- |
| 音频栈 | `main/audio/` | I2S、ES8311、播放队列、AEC/VAD |
| 唤醒词引擎 | `main/audio/wake_words/`、`engines/` | 叫名字有反应 |
| 显示层 | `main/display/lvgl_display/` | LVGL 与主题 |
| 灯 / 按键 | `main/led/`、`main/button*` | WS2812、双键 |
| 配置存储 | `main/settings.cc` | NVS 封装 |
| SoftAP 配网 | `boards/wifi_board.cc` | 配置页与热点（需改为长按触发） |

| 移除 | 原因 |
| --- | --- |
| `main/protocols/` | 云协议，本设计不存在云端 |
| `main/ota.cc` 云端升级路径 | 改由 USB 烧录 |
| `main/assets.cc` 云端资产下发 | 资源编译进固件 |
| 激活流程 | 无账号体系 |
| Opus 编解码（编码侧） | 本地音频用 PCM / ADPCM，省 Flash 与 CPU |

## 13. 资源与未决项

### 资源

- `pet_frames.bin` 1.28 MB 已进仓库，编译期内嵌（`EMBED_FILES`）；成长阶段若要新增表情行，
  需先算 Flash 预算
- 音效片段预留 ~512 KB（白噪音走算法生成，不占 Flash）
- 数值存档是小块 NVS，无压力

### 未决

1. **POWER 键是否可用作普通按键**：GP2 若被接到电源锁存/复位电路上，§8 的双键方案需要重做。
   这是**阻塞项**——不确认则养成交互只能压在一个键上
2. **电池接口**：该板能否脱离 USB 运行。不确认则 `autonomous` 与"不插电脑"都无意义
3. **时间源**：无网络即无 NTP，需从 USB agent 同步、或配网页手动对时
4. **射频功耗**：配网期间 WiFi 功耗需实测；待机必须关射频
5. **ESP-IDF 版本**：与上游同步节奏（当前 fork 基于 release-v6.1 CI）
6. **衰减速率标定**：数值衰减太快会变成负担，太慢则没有存在感，需真机试玩调参
7. **夜间连续播放**：白噪音与呼吸节律要连续放 6–8 小时，功放温升与功耗需实测
8. **作息时间失准**：时间源不可靠时，夜间模式可能白天启动，需要"无时间源则只认手动触发"的降级

## 14. 路线图

按"能独立验收"切分，每阶段结束设备都可长期使用。

### V0 · 活着
- 表情层：待机/眨眼/呼吸，四态（idle / sleep / docked / 低数值）
- BOOT 短按摸摸头有反应
- 状态向量 + 衰减 + NVS 存档
- 拔掉一切网络仍可运行

### V1 · 可被接管
- `agent_link`：USB CDC 行式 JSON-RPC
- `pet.hello` 握手 / 心跳 / 断连回自主
- `docked` ↔ `remote` 切换与状态继承
- 用一个脚本假扮 agent 跑通全部原语

### V2 · 有脾气 + 会睡觉
- 行为调度器：权重表 + 概率行为
- 昼夜与作息
- **夜间陪伴模式**：微光、呼吸节律、算法白噪音、静音作息、起床
- LED 情绪表达
- 唤醒词叫名字有反应

### V3 · 养成闭环
- 四条数值 + 手势输入（含双击判定与误触降级）
- 成长 3 阶段与表情解锁
- agent 与数值的双向咬合（任务完成/报错/强制入睡）
- 配网页参数全集（含 `difficulty`）

### V4 · 扩展
- 音效包与人格档位可换
- 可选：无线 transport（不改协议与动作原语）
- 可选：多宠物联动

## 15. 明确排除

以下内容本设计不包含，未来若要做需单独提案：

- 任何形式的云端 LLM 推理
- 语音转文字、语义理解
- 联网内容（天气、新闻、时间服务）
- 触摸屏（硬件不支持）
- 摇晃交互（无 IMU）
- 养死机制

## 16. 实现记录

### V0 · 活着（已实现）

代码位置（严格按 §5 分层，`boards/` 下只剩接线）：

| 文件 | 职责 |
| --- | --- |
| `main/pet/pet_types.h` | 模式、阶段、情绪、动作行的枚举 |
| `main/pet/pet_core.cc` | 四条数值、衰减、成长阶段、NVS 存档 |
| `main/pet/pet_behavior.cc` | 权重表调度器，没人理她自己找事做 |
| `main/pet/pet_expression.cc` | 数值 → 动作行 / 呼吸幅度 / 灯色 / 亮度 |
| `main/pet/pet_action.cc` | 唯一碰硬件的层：LVGL 脸、WS2812、背光 |
| `main/pet/pet.cc` | 10 Hz 身体循环、手势队列、USB 充电态判定 |
| `main/pet/assets/` | `pet_frames.bin` + 帧索引，编译期内嵌（`EMBED_FILES`） |

开关：`CONFIG_KITTY_PET_MODE`（本板默认 y）。打开后 `main.cc` 不创建 `Application`，
不连 WiFi、不起协议、不做激活 —— 设备就是一台离线宠物。

与设计的偏差 / 需要知道的事实：

1. **素材的九行是动作，不是情绪脸。** 原 spritesheet 是同一个身体的转身与动作循环
   （待机、转身、跑、挥手、跳、蔫、张望、端详）。所以"开心/生气/困"不是换一张脸，
   而是换一组动作 + 速度 + 灯色。说明书 §6 已按事实改写。
2. **眨眼用缩放做。** 素材没有闭眼帧，眨眼是把脸垂直压到约 90% 保持 120 ms。
   看上去是"眯一下"，不是真正的眼皮。
3. **充电态用 `usb_serial_jtag_is_connected()` 判定。** 这块板没有 VBUS 检测脚，
   只能看 USB 主机在不在。插充电宝时这会判成"没插"，属于已知不精确。
4. **POWER 键没接。** §13 未决项 1 仍未确认（GP2 是否被电源锁存占用），
   所以清洁/陪玩/夜间模式现在都没有入口，`pet_core` 里的 `Clean()`/`Play()` 已经可用，
   只等一个按键。
5. **配网页没有。** 长按 BOOT 现在只播一个"端详"动作作为反馈，不改任何参数。
6. **没有音效。** V0 不需要，`AudioService` 也没启动 —— 整机更省电。
7. **衰减速率是估的。** 数值都是真机试玩才能定的，见 §13 未决项 6。

硬件验证（2026-11-05，实测串口日志）：

- 上电 458 ms 内加载 57 帧（PSRAM），创建 128×88 的动画对象
- 数值从 NVS 恢复；60 s 心跳日志正常；连续 75 s 无复位、无看门狗
- 插着 USB 时自动进入 `docked`，心跳里能看到模式

**尚未验证**（需要人动手）：

- 画面观感（呼吸幅度、眨眼的观感、灯色）
- BOOT 短按 / 双击 / 长按的实际手势判定（`iot_button` 的窗口参数没调过）
- 拔掉 USB 后是否稳定回到 `autonomous`

### V1 · 可被接管（已实现）

`main/pet/agent_link.cc` 是第二个大脑：USB 串口上的一行一条 JSON-RPC 2.0。

| 项 | 实现 |
| --- | --- |
| 传输 | USB-Serial/JTAG，和日志共用同一个口；不是 JSON 的行一律忽略 |
| `pet.hello` | 返回名字、固件版本、能力表、完整状态；同时进入 `remote` |
| 心跳 | 3 秒没有一行 JSON 就退出 `remote`，模式交还给「插没插 USB」的判断 |
| 原语 | `pet.face` `pet.look` `pet.attention` `pet.status` `pet.feed` `pet.clean` `pet.play` `pet.task` `pet.sleep` `pet.wake` `pet.ping` |
| 事件 | `pet.poke`（被摸）、`pet.state_changed`（数值/情绪/模式变了）、`pet.tired`（精力 < 15） |
| 咬合 | `pet.task result=done` → 精力 −5、亲密度 +2；`result=error` → 精力 −8、亲密度 −1 |
| 脚本 | `scripts/pet_agent.py` 假扮 host，把上面每一条都跑一遍 |

与设计的偏差：

1. **`pet.say` 没实现**（返回 -32601）。她要说话就得起音频栈，那是 V2 的事；
   现在整机没有初始化 `AudioService`，是省电的一部分。
2. **`pet.look` 是整脸平移**，不是眼珠转动 —— 眼睛在帧里，切不出来。±8 px。
3. **代劳只补一半**（§10.3 那条限制）没做：agent 调 `pet.feed` 就是完整的 +25，
   和手摸一样。这条等 V3 的手势一起做，否则没有对照。
4. **独占 / 抢占**没做：谁的 `pet.hello` 最后到谁说了算，不区分连接。
5. **控制台从 UART0 挪到了 USB-Serial/JTAG。** 之前 USB 只是第二控制台，
   驱动没装、RX 的 64 字节 FIFO 没人取，host 一写就卡住。现在 USB 是唯一控制台，
   固件在启动时就把驱动装好，`read()` 才有东西。代价是日志和 JSON 混在一个流里。

硬件验证（2026-11-05，实测）：

```
-> {"jsonrpc":"2.0","id":1,"method":"pet.hello","params":{}}
<- {"jsonrpc":"2.0","id":1,"result":{"ok":true,"name":"Kitty","firmware":"kitty-pet 0.2.0",
    "capabilities":[...],"status":{"hunger":94,"energy":75,...,"mode":"remote"}}}
<- {"jsonrpc":"2.0","method":"pet.state_changed","params":{...}}
-> {"jsonrpc":"2.0","id":2,"method":"pet.task","params":{"result":"done"}}
<- {"jsonrpc":"2.0","id":2,"result":{"energy":70,"affection":37,...}}   # 精力 75 -> 70
-> 之后不再发任何东西
<- I (3520) PetLink: Back on her own: heartbeat timeout
<- I (3520) PetCore: Mode remote -> docked
```

`pet.say` 返回 -32601，未知方法同样；`pet.sleep`/`pet.wake` 让 `mood` 在
`asleep` / `content` 之间切换，都被 `pet.state_changed` 报出来了。

### V2 · 有脾气 + 会睡觉（大部分已实现）

| 项 | 实现 |
| --- | --- |
| 行为调度 | `behavior.cc` 权重表：困了多打哈欠、亲密度高才爱主动凑近、夜里安静 |
| 音效 | `pet_audio.cc` 现场合成：叫声、两声雀跃、哈欠、咕噜、咀嚼、叹气。**没有音频文件** |
| 音频通路 | 只用板卡的 ES8311 输出，不起 `AudioService`：没有麦克风、没有 AEC、没有唤醒词 |
| 呼吸节律 | 夜间每 12 秒一次的低频沙沙，幅度随呼吸起伏 |
| 白噪音 | 低通滤波噪声，和呼吸节律同一条通路，零 Flash |
| 时钟 | `pet.time {epoch, tz_offset}` 由 host 推过来；没有 RTC 也没有 NTP |
| 作息 | `sleep_start` / `sleep_end`（默认 23:00–07:00，跨午夜），5 秒轮询一次 |
| 夜间模式 | 屏幕 4%、灯全灭、脸部定格睡姿、行为调度停摆、音频换成呼吸床 |
| 夜间保护 | agent 的 `pet.face` `pet.look` `pet.attention` `pet.feed` `pet.play` `pet.clean` `pet.task` `pet.say` 全部返回 -32001；只留 `pet.wake` `pet.status` `pet.ping` `pet.night` |
| 退出 | 作息结束自动醒、agent `pet.wake`、或者按一下 BOOT |

与设计的偏差：

1. **唤醒词没做。** esp-sr 的唤醒词是训练出来的模型，仓库里只有「你好小智」那一个；
   要做「叫 Kitty」得单独训练并塞进固件。这一版先不假装有。
2. **时间必须由 host 推。** 拔了电脑就没有时钟，作息时段自动失效 —— 这正是 §13
   未决项 3 说的状态，现在是"没有时间就只认手动"的降级实现。
3. **夜间是"睡死"而不是"浅睡"**：`energy` 会回，但 `hunger` 照样掉，第二天醒来可能饿。
4. **白噪音不可关。** §11 说"可选"；现在进了夜间就有，没有单独的开关。

硬件验证（2026-11-05，实测）：

```
I (321) Es8311AudioCodec: Es8311AudioCodec initialized
I (341) PetAudio: Voice ready: 16000 Hz, volume 85
I (760) PetCore: Clock set to 23:20            # 由 pet.time 推入
I (5460) PetCore: Night mode on                # 5 秒轮询撞上作息窗口
I (5460) Pet: Night mode on (schedule)
I (60420) Pet: alive 60s | asleep | h 97 e 52 ...   # 夜间连跑 60 秒，精力 +1
```

`pet.face` 在夜间返回 -32001；`pet.wake` 把 `night` 关掉并播一声雀跃。
喇叭里到底好不好听得人来听 —— 我只能确认它没崩、音频通路是通的。

### V3 · 养成闭环（大部分已实现）

| 项 | 实现 |
| --- | --- |
| 四条数值 | 已衰减、已存档、已能补 |
| 手势 | BOOT 单击摸头 / 双击喂食 / 长按 3 秒开配网；双击失败降级为单击（`iot_button` 自带窗口） |
| 成长 | 幼体只会张望打瞌睡 → 成长多一个伸懒腰 → 成熟再加雀跃；`behavior.cc` 直接按阶段清零权重 |
| 代劳上限 | agent 喂/清洁/陪玩只补一半，且补到 85 就补不动（`kAgentCareCap`），人才能补满 |
| 配网页 | `pet_gateway.cc`：SoftAP `Kitty-XXXX` / `kitty1234`，`http://192.168.4.1/`，3 分钟没人动就关射频 |
| 参数 | 名字、音量、亮度、难度、性格、作息起止、手动对时；全部落 NVS |
| 屏幕上 | 配网期间 `lv_layer_top` 上盖一块黑底小字，显示 SSID 和地址 |

与设计的偏差：

1. **POWER 键仍然没接。** §13 未决项 1 没确认，所以清洁/陪玩/夜间只有 agent 和 BOOT 两条路。
   代劳逻辑本身已经能跑（`pet.clean` / `pet.play`），只差一个按键入口。
2. **多了一个 `pet.config` 原语**（不在 §7 的表里）：让 host 也能打开配网页。
   加它一半是为了测试，一半是因为把手机贴到桌下按长按并不总是方便。
3. **配网页没有认证**，热点密码就是全部防线。局域网里有人连上就能改设置 ——
   按 §11「WiFi 只是配网射频」的定位，它只在 3 分钟里存在，可以接受。
4. **唤醒灵敏度这一项留空**：没有唤醒词，填了也没用。

硬件验证（2026-11-05，实测）：

```
I (510) PetGateway: Configuration AP up: Kitty-6319 / kitty1234, http://192.168.4.1/
# PC 连上热点后
GET  /        -> 1700 字节表单页，含 name/volume/brightness/difficulty/personality/sleep/clock
POST /save    -> "已保存" 页
I (18060) PetGateway: Settings saved
I (19640) PetGateway: Configuration AP down        # 保存后 1.6 秒射频关闭
```

关掉再开再来一遍也不崩。落盘验证：重启后 `pet.hello` 的 name 是 `小咪`（UTF-8 `e5b08f e592aa`）、
音量 55、亮度 45、作息 22:30–06:30。

代劳上限实测：`pet.feed` 在她饱食 99 时被拒绝（"already full enough"），
`pet.play` 只加 1 点亲密度（人的三倍是一半的三倍）。

### V2 后续 · 耳朵与命令词

`main/pet/pet_ears.cc` 已经给麦克风开了一条通路：一个 32 ms 一帧的采集任务，
算 RMS、维护一个缓慢的房间噪声基线，超过阈值就"吓一跳"。

```
I (341) PetEars: No command model: she will only notice loud noises
I (361) PetEars: Sound: rms 21663 (floor 727, trigger 4200)
I (371) PetEars: Loud noise: rms 21663 (floor 727)
```

唤醒词（MultiNet 中文命令词）做到了哪一步，为什么停在这里：

1. **模型放得下。** 打开 `CONFIG_SR_MN_CN_MULTINET7_QUANT=y` 后固件从 3.15 MB 长到 3.34 MB，
   离 4 MB 分区还很远。
2. **但模型不在固件里，而在资源包里。** 这一版 esp-sr（2.4.7）的 `esp_mn_handle_from_name()`
   会去 `/srmodel/mn7_cn/mn7_index` 找文件，找不到就**不返回**（板子直接卡死在那里，
   日志停在 `Please check if there are target files in /srmodel/mn7_cn/mn7_index`）。
3. **资源包里没有它。** 当前的 `generated_assets.bin` 只打了 WakeNet 的 `srmodels.bin`
   （里面有 `wn9`，没有 `mn7`）。要真的认名字，需要：
   - 让 `scripts/build_default_assets.py` 把 MultiNet 模型一起打进 srmodels；
   - 运行期挂载 assets 分区、`srmodel_load()` 出 model list、再 `esp_srmodel_filter()`；
   - 这条链要动资源生成脚本，属于独立的一块工作。

后来这条链已经接通了（见下面"唤醒词"一节），剩下的只是"识别率"问题。

## 唤醒词 · 现在的样子

| 件 | 状态 |
| --- | --- |
| 模型 | `CONFIG_SR_MN_CN_MULTINET7_QUANT=y`，`config.json` 里 `"wake_word": "custom"` |
| 资源 | 构建时把 `mn7_cn` + `fst` 打进 `srmodels.bin`（资源包 1.2 MB → 3.6 MB，8 MB 分区够用） |
| 加载 | `Ears::Start()` 读 `index.json` → `srmodel_load()` → `esp_srmodel_filter("mn","cn")` |
| 命令词 | NVS `wake_phrase`（默认 `ni hao xiao mi`），`esp_mn_commands_alloc/add/update` |
| 采音 | 16 kHz 单声道，32 ms 一帧；麦克风增益 38 dB，送模型前再放大 3 倍 |
| 回调 | 命中 → 抬头、两声雀跃、`pet.called` 事件推给 host |

`scripts/build.py` 加了一个 `custom` 唤醒词档位，并且允许 variant 在 `config.json` 里用
`"wake_word"` 指定自己的唤醒词 —— 否则这个组合只能靠手敲 menuconfig 复现。

还有两件事要说清楚：

1. **识别率没在真人身上验证过。** 我手上的唯一声源是笔记本 SPDIF（没接音箱，实测无声）
   和板子自己的小喇叭。用 TTS 把"你好小咪"推给她、从自己的喇叭放出来再自己听，
   能量是够的（rms 1560），但模型没命中 —— 这不代表真人说话也不行，
   只是我这边没法证明。**需要人对着板子说一句。**
2. **`pet.say` 顺手做出来了**：一行一条 base64 PCM16，单条上限 12 KB（约 0.3 秒），
   host 分片推。这也算把 V1 里"agent 推音频让她说话"那条补上了。

### 唤醒词 · 调参记录（第二轮）

1. **模型只加载一次。** `Ears::Start()` 原来每次退出夜间都会重新 `srmodel_load()`，
   等于把权重再拷一份进 PSRAM；夜里起来一次就把堆吃干，紧接着
   `Item psram alloc failed` 然后 panic。现在 bundle 与 model 实例都是懒加载、只建一次。
2. **PSRAM 预算。** 这块板 8 MB，原本 LVGL 拿走 2.5 MB 池 + 2 MB 图片缓存，
   加上 1.28 MB 帧和 ~1 MB 模型，只剩 442 KB。把这两个数做成 Kconfig
   （`LV_PSRAM_POOL_EXTRA_KB` / `LV_IMAGE_CACHE_KB`，默认值保持原样），
   本板设成 1024 / 512，空闲回到约 2.4 MB —— 现在整机有 2.4 MB 余量。
3. **`pet.say` 改成先攒后放。** 分片边收边播会断断续续，模型听断句更不可能命中；
   现在收到 `last` 才一次性交给播放。缓冲 4 s（可退到 2 s / 1 s）。
4. **`calls` 计数。** 她每"听到名字"就给 NVS 里的计数 +1，`pet.status` 里带出来。
   这样验证不需要有人守在串口前：说一句、过一会儿读 `calls` 就知道。
5. **离线自测。** `pet.say` 支持 `{"test": true}`：不播放，直接把这段 PCM 喂给命令模型，
   返回识别结果。用它把"模型认不认"和"房间传不传得进来"分开了 ——
   结果是**连数字音频都不认**，所以问题不在麦克风，在模型与合成语音的匹配上。

### 唤醒词 · 第三轮：模型是好的

关键实验：用 `edge-tts`（神经语音，zh-CN-XiaoxiaoNeural）合成"你好小咪"，
把 PCM 直接喂给命令模型（`pet.say {"test": true}`）：

```
I (9951) PetEars: Offline test heard:  ni hao xiao mi (prob 0.28)
```

**模型、命令图、pinyin 全部是对的。** 之前一直不响，是 Windows 自带的
Microsoft Huihui（2010 年代的拼接式语音）压根不在模型的训练分布里。

所以现在的状态是：

| 环节 | 结论 |
| --- | --- |
| 命令词图 | ✅ 运行期替换有效，`ni hao xiao mi` 生效 |
| 模型 | ✅ 能识别自然语音（概率 0.28，阈值 0.12） |
| 数字通路 | ✅ 喂 PCM 就能命中 |
| 声学通路 | ❓ 房间里的声音进不了麦克风（rms 只有几百，模型需要几千） |

声学这一环有三条待办：
1. 真人对着板子说（音量远大于我这边 0.85 寸喇叭重放）；
2. 或者把蓝牙音箱的音量开到能听清、靠近板子；
3. 代码侧已经做了自适应归一化（每 32 ms 把信号拉到 rms 3500，增益上限 8 倍），
   阈值 0.12 —— 这两条都是为"小声也能唤醒"准备的。

`pet.record`（把麦克风数据回传）做了一半就去掉了：I2S 只有一个消费者，
录音任务和听音任务抢同一个 codec，把 `pet_link` 任务拖进了看门狗。
把这条调试通道再打开前，得先让两者互斥。

### 唤醒词 · 通了

把 1 kHz 音调和 edge-tts 的"你好小咪"通过蓝牙音箱放到房间里，她的麦克风收到
rms 1500–2200（安静时 ~330），命令模型连中三次：

```
I (2842) PetEars: Heard her name:  ni hao xiao mi (prob 0.07)
I (5682) PetEars: Heard her name:  ni hao xiao mi (prob 0.12)
I (8502) PetEars: Heard her name:  ni hao xiao mi (prob 0.07)
```

`pet.status` 的 `calls` 从 0 变成 3，说明计数也落盘了。随后 **120 秒静音零误触**。

最终参数：

| 项 | 值 | 说明 |
| --- | --- | --- |
| 麦克风增益 | 42 dB（ES8311 上限附近） | 板载麦克风很小 |
| 送模型前归一化 | 目标 rms 6000，增益上限 12× | 每 32 ms 调整一次 |
| 检测阈值 | 0.05 | 命中概率只有 0.05–0.12，因为房间里信号偏弱 |
| 命令词 | `ni hao xiao mi`（NVS `wake_phrase`） | 可在配网页改 |

要紧的是**概率很低**（0.05–0.12），说明信噪比贴着实底线。真人靠近说话会高得多，
但如果房间很吵、或者离得远，就可能叫不醒 —— 这是硬件（单个小麦克风、无 AGC）
决定的，不是调参能解决的。要更稳只能上 AFE + AGC，或者换麦克风。

顺带修的两处：
- 归一化原来按每块 32 ms 独立算，静音段会把底噪放大 12 倍；现在增益有上下限且
  低概率同时压住了误触。
- 录音回传（`pet.record` / `pet.mic`）两次尝试都把设备搞进看门狗或重启，
  已经从固件移除；耳朵内部的 5 秒环形缓冲留着，等音频通路真正能互斥了再开。

### 收尾：灵敏度与两处崩溃

手册里最后两项能做出来的缺口：

- **唤醒灵敏度**（§14 参数表）：低/中/高 → 检测阈值 0.08 / 0.05 / 0.03，
  配网页可改，NVS 键 `wake_sens`。实测 0.03 在安静房间连跑 60 秒零误触。
- **灯快速闪烁**（§10）：agent 接管时 4 Hz 明灭，交还后回到呼吸。

过程中踩到两个真 bug，都不是"逻辑写错"，而是"边界":

1. **NVS 键名超长**：`wake_sensitivity` 有 16 个字符，NVS 上限 15，于是
   `nvs_set_i32` 返回 `ESP_ERR_NVS_KEY_TOO_LONG`，而 `Settings::SetInt` 用
   `ESP_ERROR_CHECK` 包着 —— 保存配网页直接 abort 重启。改成 `wake_sens`。
   这类错误只在写的时候炸，读的时候静默返回默认值，所以更难发现。
2. **`pdMS_TO_TICKS(5)` 在这个工程里等于 0**：`CONFIG_FREERTOS_HZ=100`，
   5 ms 换算成 tick 是 0，`vTaskDelay(0)` 不会让出 CPU 给空闲任务，
   于是 `pet_link` 任务把 IDLE1 饿死 → 看门狗复位。改成 `vTaskDelay(1)`。

两个 bug 都是"看起来正常的代码在特定配置下不成立"，值得记在文档里。
