# Xiao V：会主动执行的 openvela 语音助手

## 一、作品简介

Xiao V 是运行在百问网 DShanPi R528S3 上的桌面语音助手：设备端持续完成本地
KWS、麦克风采集、扬声器播放、LVGL 显示和触摸交互，主机网关完成 ASR、MiMo
对话、工具调用和 TTS。它不是只回答问题的聊天机器人，能够在提醒到期时主动播报，
并通过 MQTT、媒体和天气工具执行真实动作。

本作品采用官方 AI 硬件赛道允许的模式 A：设备通信协议 + 云端大模型，独立完成端云
场景应用。它不把 sherpa-onnx/ONNX Runtime 塞进 NuttX，而是在 R528 上使用
TFLite Micro int8 KWS；主机侧可选 sherpa-onnx 仅作为云请求前的预门控。

## 二、选题方向

AI 硬件产品创新。目标用户是需要免手操作和主动提醒的桌面设备使用者：用户说
“你好，openvela”后可以询问天气、控制已授权的空调、播放音乐或观看监控；设置
提醒后，设备会在到期时主动推送通知和语音。

## 三、目录结构

- `app/xiaov_voice_assistant/`：openvela 应用、TFLite Micro KWS、ALSA、LVGL、协议和网关源码。
- `app/xiaov_voice_assistant/core/`：可移植 C 核心，包括状态机、VAD、事件、提醒和媒体模型。
- `app/xiaov_voice_assistant/host/`：Python 网关、MiMo/ASR/TTS、Open-Meteo、MQTT、音乐、视频和模拟器。
- `app/xiaov_voice_assistant/host/skills/`：天气、智能家居两个自定义 Markdown Skill。
- `board/r528s3-dshanpi/`：真实板型的配置片段、构建说明和启动示例。
- `logs/1033087886/`：本次 Codex Desktop 会话的脱敏提交日志；采集范围见 manifest。
- `contest2026_271_rose.xml`：把应用和板级目录映射进 openvela 构建树。

## 四、运行方式

### 1. 拉取工程

```bash
repo init -u https://github.com/open-vela/contest2026_271_rose \
  -b dev-ai-contest-2026 -m contest2026_271_rose.xml
repo sync -c -j8
```

### 2. 编译

在 openvela 根目录执行：

```bash
./build.sh vendor/allwinnertech/boards/r528/r528s3-dshanpi/configs/nsh/ -j4
```

在配置中启用 `board/r528s3-dshanpi/defconfig.fragment` 列出的选项。manifest 会将
应用映射到 `packages/demos/contest2026_271_xiaov_voice_assistant`，无需复制到公共仓。
Allwinner PhoenixSuit/FEL 烧录由板级开发环境完成，本仓不提交镜像。

### 3. 板端启动

串口使用 CH343，通常是 `COM6`，波特率 `1500000`：

```text
nsh> xiaov version
nsh> xiaov selftest
nsh> xiaov skills install
nsh> xiaov daemon <网关IP> 8765 / &
```

`xiaov skills install` 将同源 Skill 写入大赛要求的 `/data/agent/skills/`；常规启动也会
幂等安装。Wi-Fi、网关 token、MiMo、MQTT 和音乐服务 Key 只能通过环境变量或本地
未跟踪配置注入，绝不放在命令行、日志或仓库中。

### 4. 网关

```bash
cd app/xiaov_voice_assistant/host
python -m venv .venv
python -m pip install -e ".[mimo,mqtt,speech]"
python -m gateway.server --host 0.0.0.0 --port 8765 \
  --asr mimo --llm mimo --tts openai --tools production
```

生产模式读取 `MIMO_API_KEY`、`XIAOV_GATEWAY_TOKEN`、`XIAOV_MQTT_CONFIG` 等外部
配置。网关启动时加载 `host/skills/*.md` 到 MiMo 系统提示词；天气 Skill 还启用
单城市当前天气快速路径，直接调用 Open-Meteo 并缓存地理编码 24 小时、天气 60 秒。

无外部服务时可运行离线协议模拟：

```bash
python -m gateway.server
python -m simulator.device --uri ws://127.0.0.1:8765
```

## 五、功能与主动执行场景

- 本地 KWS：TFLite Micro int8 模型、VAD 门控、固定内存前端和独立推理 worker。
- 语音问答：PCM 上传、MiMo ASR、流式回答和 TTS PCM 播放。
- 快速天气：单城市当前天气绕过一次 MiMo 工具判断，返回适合朗读的一句话。
- 智能家居：通过 MQTT 控制已配置设备；GreeCam 协议等待真实 ACK。
- 媒体：本地/网络音乐 PCM 流、显示视频和监控画面控制。
- 主动提醒：持久化 timer 到期后由网关主动推送 `reminder`，设备从 standby 激活
  提醒 UI/语音；设备断线时不会把提醒错误发送给其他设备。
- 触摸控制：Talk、Finish、Interrupt、Dismiss 和媒体/监控状态回传。

## 六、AI Coding 使用说明

AI 参与了需求拆解、KWS 引擎可行性分析、TFLite Micro 移植、ALSA/线程竞态修复、
端云协议、天气快速路径、MQTT/媒体工具、实体板串口验收、测试和提交整理。每项结论
都区分源码检查、主机测试、交叉编译、镜像打包和实体板证据；真实会话日志见
`logs/1033087886/`，只保留本次参赛提交相关内容并做了凭据脱敏。

## 七、验证与边界

```bash
cd app/xiaov_voice_assistant/host
python -m unittest discover -s tests -v
```

已在 R528S3 上完成 ALSA 采集/播放、TFLite Micro KWS、LVGL、WebSocket 语音回路、
提醒、MQTT/媒体协议和 RC43 镜像构建验证。完整生产服务仍依赖评委自己的网络、
MiMo/TTS/MQTT 配置；mock 测试不等于实时云服务或每种外设都已验收。仓库不含模型
权重、固件镜像、现场录音、Wi-Fi 密码、API Key、MQTT 凭据或私钥。

## 八、许可证

本作品源码以 Apache-2.0 发布。第三方运行时和模型需分别核对许可证；本仓不提交
外部权重，避免把不可再分发的模型误带入参赛仓。
