# R528S3 DShanPi 板级说明

本作品在百问网 DShanPi R528S3（Cortex-A7、openvela `dev-ai-contest-2026`）上运行。
板级目录只保存参赛配置说明，不复制或修改公共 `vendor`、`nuttx` 仓库。

## 构建

在通过 `repo init` / `repo sync` 拉取的 openvela 工作区根目录执行：

```bash
./build.sh vendor/allwinnertech/boards/r528/r528s3-dshanpi/configs/nsh/ -j4
```

启用 `app/xiaov_voice_assistant` 后，应确认以下选项已打开：

```text
CONFIG_EXAMPLES_XIAOV=y
CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA=y
CONFIG_EXAMPLES_XIAOV_KWS=y
CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS=y
CONFIG_EXAMPLES_XIAOV_LVGL=y
CONFIG_TFLITEMICRO=y
CONFIG_NETUTILS_LIBWEBSOCKETS=y
```

完整的实板验证过的参考配置保存在 [`defconfig.reference`](defconfig.reference)，
配置增量说明见 [`defconfig.fragment`](defconfig.fragment)。最终镜像由 Allwinner
打包工具生成；本仓不提交镜像和现场密钥。

## 首次启动

串口为 CH343 `COM6`，波特率 `1500000`。启动后先运行：

```text
nsh> xiaov version
nsh> xiaov selftest
nsh> xiaov skills install
```

`skills install` 会把仓内同源 Skill 写入 `/data/agent/skills/`，满足 AI 硬件赛道
的 Skill 部署要求。网络和网关 token 只通过参赛者自己的安全配置流程写入 `/data`，
不在命令、日志或代码中提供示例密钥。

然后启动网关并运行常驻语音服务：

```text
nsh> xiaov daemon <网关IP> 8765 / &
```

## 已验证的端侧能力

- 48 kHz 双声道 ALSA 麦克风采集，转换为 16 kHz 单声道 PCM；
- TFLite Micro int8 本地 KWS，VAD 门控和固定内存前端；
- LVGL 状态/对话/触摸界面；
- WebSocket 端云语音回路、流式文本和 TTS 播放；
- 主动提醒、MQTT 智能家居、天气、音乐和视频控制协议。

模型推理和音频采集在独立 worker 中运行，避免在 ALSA 捕获回调中执行长时间
`Invoke()`。队列、超时、恢复重试和控制台输出均有上限。
