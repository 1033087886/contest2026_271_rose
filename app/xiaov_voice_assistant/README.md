# Xiao V Voice Assistant 应用

这是运行在 openvela R528S3 DShanPi 上的小屏语音助手应用。设备端负责本地唤醒、
音频采集/播放、LVGL UI、触摸和主动事件；主机网关负责 ASR、MiMo 对话、工具调用、
TTS、天气、MQTT、音乐和视频，并通过受限 WebSocket 协议协作。

## 目录

- 根目录 C/C++ 文件：openvela NSH 应用、ALSA/网络/服务/UI 适配器和 TFLite Micro KWS。
- `core/`：不依赖 BSP 的状态机、协议、VAD、音频、提醒和媒体核心。
- `assets/`：随应用编译的字体源文件与唤醒提示音示例。
- `host/gateway/`：网关和工具 provider；`host/simulator/`：无硬件模拟器。
- `host/skills/`：天气和智能家居自定义 Skill 源文件。
- `host/tests/`：协议、天气、工具和集成回归测试子集。

## 设计要点

1. KWS 在设备端使用 TFLite Micro int8 模型，不依赖 sherpa-onnx/ONNX Runtime；
   sherpa-onnx 只作为主机侧可选的预门控，不进入固件。
2. ALSA 捕获线程只做短帧搬运、VAD 和前端推进，模型 `Invoke()` 由独立 worker 执行。
3. 设备端触发后上传 PCM，网关将文本增量和 TTS PCM 流式返回；提醒从网关主动推送到设备。
4. `xiaov skills install` 将两个 Skill 写入 `/data/agent/skills/`；网关生产模式同时加载
   `host/skills/*.md` 到 MiMo 的系统提示词，形成可审查、可运行的 Skill 闭环。

## 主机运行

```bash
cd app/xiaov_voice_assistant/host
python -m venv .venv
python -m pip install -e ".[mimo,mqtt,speech]"
python -m gateway.server --host 0.0.0.0 --port 8765 \
  --asr mimo --llm mimo --tts openai --tools production
```

密钥、MQTT JSON、TTS/ASR 地址和网关 token 只通过环境变量或本地未跟踪配置注入；
不要把它们写入命令历史、日志或代码仓库。无外部服务时可执行：

```bash
python -m gateway.server
python -m simulator.device --uri ws://127.0.0.1:8765
```

## 测试

```bash
cd host
python -m unittest discover -s tests -v
```

测试使用 mock provider 或 HTTP mock transport，不消耗 API 配额；真实天气、MQTT、
音乐和实体板验收需要参赛者自己的安全配置与硬件。
