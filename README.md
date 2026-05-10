# ESP32-S3 Elder Alert Terminal

基于 ESP32-S3 的独居老人异常状态风险预警与主动提醒终端。

这个项目面向健康或轻度慢病独居老人的居家安全场景，目标不是替代医疗监护，而是在老人遇到异常环境、长时间无明显活动或主动求助时，先在本地完成提醒和确认闭环，再把关键状态同步到云端，让家属、照护者或后续智能体能够及时看到“发生了什么、是否需要关注”。

## 项目亮点

- **本地闭环优先**：断网时仍能完成传感器采集、风险判断、声光提醒、确认解除和 SOS 求助。
- **状态机清晰可解释**：使用 `NORMAL / REMIND / ALARM / SOS` 四态模型，适合答辩演示、工程排查和后续产品化扩展。
- **多传感器融合**：接入温湿度、气压、光照、MQ-2 烟雾/气体、AM312 人体活动检测，为居家异常判断提供基础数据。
- **云端留痕与网页展示**：ESP32-S3 通过 HTTP 上报事件和周期遥测，Node.js + Express 服务提供网页面板查看最新状态、告警记录和语音识别结果。
- **语音交互雏形已打通**：INMP441 I2S 麦克风完成采样验证，GPIO17 录音键可触发短音频上传到云端 ASR，为后续语音助手、风险解释和家属沟通打基础。
- **可持续演进**：后续可接入 LD2410B 毫米波人体存在检测、MAX98357A 语音播放、TTS、AI 回复、可穿戴健康趋势数据等能力。

## 当前能力

### 端侧固件

- ESP32-S3 + ESP-IDF 工程结构
- AHT20 / BMP280 / BH1750 / MQ-2 / AM312 采集
- `RiskEngine` 风险判断与中文可解释原因
- `AppController` 状态机：`NORMAL / REMIND / ALARM / SOS`
- LED + 蜂鸣器本地声光提醒
- GPIO7 确认键，GPIO8 SOS 键，GPIO17 录音键
- OLED 状态与传感器摘要显示
- Wi-Fi / BLE provisioning 配网
- INMP441 I2S 麦克风采样与短录音上传测试

### 云端与网页

- Node.js + Express 服务
- `POST /api/alert` 接收设备上报
- `GET /api/latest` / `GET /api/alerts` 提供状态和事件记录
- `POST /api/speech/transcribe` 接收短音频并调用腾讯云一句话识别
- `GET /api/speech/latest` 查看最近语音识别结果
- 网页监控面板展示设备状态、风险原因、历史事件和语音识别测试结果
- 可选 `X-Device-Token` 设备鉴权机制

## 系统链路

```text
环境/活动感知
-> 风险判断
-> 本地声光提醒
-> 用户确认 / 主动 SOS
-> 未确认升级
-> 云端留痕
-> 网页面板 / 家属查看 / 后续 AI Agent 扩展
```

语音扩展链路：

```text
GPIO17 录音键
-> INMP441 采音
-> ESP32-S3 封装短 WAV
-> 云端 ASR
-> 网页显示识别文本
-> 后续 TTS / AI 回复 / 风险解释
```

## 硬件接线摘要

| 功能 | ESP32-S3 引脚 |
|---|---|
| I2C SDA | GPIO4 |
| I2C SCL | GPIO5 |
| MQ-2 AO | GPIO1 |
| AM312 OUT | GPIO6 |
| 确认键 | GPIO7 |
| SOS 键 | GPIO8 |
| 蜂鸣器 | GPIO9 |
| 提示 LED | GPIO10 |
| INMP441 BCLK | GPIO12 |
| INMP441 WS | GPIO13 |
| INMP441 DIN | GPIO14 |
| 录音键 | GPIO17 |
| MAX98357A DIN（规划） | GPIO15 |

## 目录结构

```text
elder_alert_project/
  components/      ESP-IDF BSP 与中间层组件
  main/            ESP32-S3 固件入口
  public/          网页监控面板前端
  data/            云端状态数据目录
  server.js        Node.js + Express 云端服务
  CMakeLists.txt   ESP-IDF 工程入口
  sdkconfig        ESP-IDF 配置
```

## 运行方式

固件构建：

```powershell
C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe C:\Users\22061\esp\v5.5.2\tools\idf.py build
```

云端服务本地运行：

```bash
cd elder_alert_project
npm install
node server.js
```

访问网页面板：

```text
http://localhost:3000/
http://spectator0618.online/  （网页正在审核中，预计5/20号上线）
```

## 项目前景

这个项目的价值在于把“低成本嵌入式终端”和“可解释的风险状态机”结合起来：端侧负责可靠、及时、可离线运行的安全闭环，云端负责展示、留痕、语音识别和后续智能体扩展。它可以继续向三个方向演进：

- **更可靠的感知**：加入 LD2410B 毫米波人体存在检测，降低静坐、睡眠等场景误报。
- **更自然的交互**：加入 MAX98357A 播放、TTS 和按键式语音对话，让设备能主动解释风险和回应老人。
- **更完整的照护协同**：结合网页面板、家属通知、可穿戴健康趋势数据和 AI Agent，形成轻量化居家照护辅助系统。

## 项目边界

本项目不是医疗级监护设备，不替代医生、护士、护工或家属。当前核心价值是“出事后不要没人发现”：本地先提醒老人，未确认再升级告警并云端留痕，为家属和照护者提供更早的关注线索。
