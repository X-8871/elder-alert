# ESP32-S3 Elder Alert Terminal

基于 ESP32-S3 的独居老人异常状态风险预警与主动提醒终端。

本项目面向健康或轻度慢病独居老人的居家安全场景，目标是在环境异常、长时间无活动或主动求助时，先通过本地声光提醒老人，若无人确认则升级告警，并将状态同步到云端网页面板，方便家属或照护者查看。

## 核心能力

- ESP32-S3 多传感器采集
- 温湿度、气压、光照、烟雾/气体、人体活动状态感知
- `NORMAL / REMIND / ALARM / SOS` 本地状态机
- LED + 蜂鸣器本地提醒
- 确认键与 SOS 键交互
- OLED 状态显示
- Wi-Fi / BLE 配网
- HTTPS 上报到云端服务
- 网页端查看最新状态与告警记录

## 系统链路

```text
环境/活动感知
-> 风险判断
-> 本地声光提醒
-> 用户确认 / 主动 SOS
-> 未确认升级
-> HTTPS 云端上报
-> 网页面板 / Agent 扩展
```

## OpenClaw 创作计划说明

本项目使用 Codex 和 GPT 系列模型辅助完成项目结构整理、风险状态机设计、云端上报链路梳理和提交版项目打包。

后续可将 OpenClaw 作为云端 Agent 增强层接入：

```text
ESP32-S3
-> 腾讯云 HTTPS 服务
-> OpenClaw webhook
-> Agent 生成家属通知、风险解释和自动化提醒
```

这种方式避免 ESP32 直接承担复杂 AI 协议和鉴权逻辑，让嵌入式端保持本地闭环优先，云端负责 AI 协作与扩展。

## 目录结构

```text
components/      ESP-IDF BSP 与中间层组件
main/            ESP32-S3 固件入口
public/          网页监控面板前端
data/            云端状态数据目录
server.js        Node.js + Express 云端服务
CMakeLists.txt   ESP-IDF 工程入口
sdkconfig        ESP-IDF 配置
```

## 项目边界

本项目不是医疗级监护设备，不替代医生、护士、护工或家属。当前核心价值是“出事后不要没人发现”：先在本地提醒老人，再通过云端留痕和远程展示辅助家属关注。


