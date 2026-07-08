require("dotenv").config();
const express = require("express");
const crypto = require("crypto");
const fs = require("fs");
const nodemailer = require("nodemailer");
const path = require("path");

const app = express();
const port = process.env.PORT || 3000;
const alertPath = "/api/alert";
const latestPath = "/api/latest";
const speechTranscribePath = "/api/speech/transcribe";
const speechLatestPath = "/api/speech/latest";
const speechReplyAudioPath = "/api/speech/reply-audio";
const voicePromptsPath = "/api/voice-prompts";
const voicePromptAudioPath = "/api/voice-prompts/audio";
const maxAlerts = 50;
const maxSpeechRecords = 10;
const maxStoredReminders = 100;
const maxPendingCommands = 20;
const maxTimerDelayMs = 2147000000;
const maxSpeechAudioBytes = 600 * 1024;
const dashboardUser = "olderalert";
const dashboardPassword = "88888888";
const dashboardSessions = new Set();
const manualVoiceModes = new Set(["CARE", "INTERACT"]);
const offlineVoiceReply = "已接收到信息。如有紧急情况，请按红色按钮。网络暂不可用。";
let currentVoiceMode = "CARE";
const deviceOfflineAfterMs = Number(process.env.ELDER_DEVICE_OFFLINE_AFTER_MS || 45000);
const speechTranscribeInlineAiWaitMs = Number(process.env.ELDER_SPEECH_INLINE_AI_WAIT_MS || 1000);
const deviceToken = (process.env.ELDER_ALERT_TOKEN || "").trim();
const aiReplyConfig = {
  apiKey: (process.env.ELDER_AI_API_KEY || "").trim(),
  baseUrl: (process.env.ELDER_AI_BASE_URL || "https://api.openai.com/v1").trim().replace(/\/+$/, ""),
  model: (process.env.ELDER_AI_MODEL || "").trim(),
  timeoutMs: Number(process.env.ELDER_AI_TIMEOUT_MS || 8000),
};
const ttsConfig = {
  provider: (process.env.ELDER_TTS_PROVIDER || "").trim().toLowerCase(),
  apiKey: (process.env.ELDER_TTS_API_KEY || process.env.ELDER_AI_API_KEY || "").trim(),
  baseUrl: (process.env.ELDER_TTS_BASE_URL || process.env.ELDER_AI_BASE_URL || "").trim().replace(/\/+$/, ""),
  model: (process.env.ELDER_TTS_MODEL || "").trim(),
  voice: (process.env.ELDER_TTS_VOICE || "mimo_default").trim(),
  format: (process.env.ELDER_TTS_FORMAT || "wav").trim().toLowerCase(),
  sampleRate: Number(process.env.ELDER_TTS_SAMPLE_RATE || 16000),
  timeoutMs: Number(process.env.ELDER_TTS_TIMEOUT_MS || 12000),
};
const tencentAsrConfig = {
  secretId: (process.env.TENCENTCLOUD_SECRET_ID || "").trim(),
  secretKey: (process.env.TENCENTCLOUD_SECRET_KEY || "").trim(),
  appId: (process.env.TENCENTCLOUD_ASR_APPID || "").trim(),
  engine: (process.env.TENCENTCLOUD_ASR_ENGINE || "16k_zh").trim(),
  region: (process.env.TENCENTCLOUD_ASR_REGION || "ap-shanghai").trim(),
};
const sosEmailConfig = {
  host: (process.env.ELDER_SMTP_HOST || "smtp.qq.com").trim(),
  port: Number(process.env.ELDER_SMTP_PORT || 465),
  secure: String(process.env.ELDER_SMTP_SECURE || "true").trim().toLowerCase() !== "false",
  user: (process.env.ELDER_SMTP_USER || "").trim(),
  pass: (process.env.ELDER_SMTP_PASS || "").trim(),
  recipients: String(process.env.ELDER_SOS_EMAIL_TO || "")
    .split(/[;,]/)
    .map(item => item.trim())
    .filter(Boolean),
  fromName: (process.env.ELDER_SMTP_FROM_NAME || "独居老人守护终端").trim(),
  dashboardUrl: (process.env.ELDER_DASHBOARD_PUBLIC_URL || "").trim(),
  dedupeMs: Number(process.env.ELDER_SOS_EMAIL_DEDUPE_MS || 60000),
  timeoutMs: Number(process.env.ELDER_SMTP_TIMEOUT_MS || 10000),
};
const defaultStorePath = path.join(__dirname, "data", "alerts-store.json");
const storePath = process.env.ELDER_STORE_PATH
  ? path.resolve(process.env.ELDER_STORE_PATH)
  : defaultStorePath;
const dataDir = path.dirname(storePath);

const alerts = [];
const speechRecords = [];
let latestSnapshot = null;
let latestSpeech = null;
let cachedReplyAudio = null;  /* 预生成的 TTS 音频缓存，由 transcribe 端点异步生成 */
let cachedReplyAudioReady = false;  /* 异步 AI+TTS 是否完成标志 */
let voicePrompts = [];
let preferredRunMode = null; // null = 跟随设备默认；"DEMO" | "REAL"
let sosMailTransporter = null;
let sosEmailConfigWarningLogged = false;
const lastSosEmailAtByDevice = new Map();

/* 通知配置：网页可自定义收件人、阈值和邮件模板 */
let notificationConfig = {
enabled: true,
recipients: [],
threshold: "SOS", /* SOS | ALARM_SOS | REMIND_ALARM_SOS */
subjectTemplate: "[{{state}}] 设备 {{device_id}} 触发紧急通知",
bodyTemplate: "独居老人守护终端检测到 {{state}} 状态。\n设备：{{device_id}}\n时间：{{time}}\n原因：{{reason}}\n监控网页：{{dashboard_url}}",
};

/* 患者信息：网页可编辑，注入 AI 系统提示词 */
let patientContext = {
name: "",
address: "",
conditions: "",
notes: "",
};

function normalizeNotificationConfig(input) {
  if (!input || typeof input !== "object") {
    return null;
  }
  const enabled = input.enabled !== false;
  const recipients = String(input.recipients || "")
    .split(/[;,\n]/)
    .map((s) => s.trim())
    .filter((s) => /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(s));
  const validThresholds = ["SOS", "ALARM_SOS", "REMIND_ALARM_SOS"];
  const threshold = validThresholds.includes(input.threshold) ? input.threshold : "SOS";
  const subjectTemplate = String(input.subjectTemplate || notificationConfig.subjectTemplate).slice(0, 200);
  const bodyTemplate = String(input.bodyTemplate || notificationConfig.bodyTemplate).slice(0, 1000);
  return { enabled, recipients, threshold, subjectTemplate, bodyTemplate };
}

function getEffectiveRecipients() {
  const configRecipients = Array.isArray(notificationConfig.recipients) ? notificationConfig.recipients : [];
  return configRecipients.length > 0 ? configRecipients : sosEmailConfig.recipients;
}

function shouldSendNotificationForState(state) {
  if (!notificationConfig.enabled) {
    return false;
  }
  const upperState = String(state || "").toUpperCase();
  const threshold = notificationConfig.threshold || "SOS";
  if (threshold === "SOS") {
    return upperState === "SOS";
  }
  if (threshold === "ALARM_SOS") {
    return upperState === "SOS" || upperState === "ALARM";
  }
  if (threshold === "REMIND_ALARM_SOS") {
    return upperState === "SOS" || upperState === "ALARM" || upperState === "REMIND";
  }
  return false;
}

function fillTemplate(template, vars) {
  return String(template || "").replace(/\{\{(\w+)\}\}/g, (match, key) => {
    return vars[key] !== undefined ? String(vars[key]) : match;
  });
}

function escapeHtml(value) {
  return String(value ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

function sanitizeHeaderValue(value) {
  return String(value ?? "").replace(/[\r\n]+/g, " ").trim().slice(0, 120);
}

function hasSosEmailConfig() {
  return Boolean(
    sosEmailConfig.host
    && Number.isFinite(sosEmailConfig.port)
    && sosEmailConfig.port > 0
    && sosEmailConfig.user
    && sosEmailConfig.pass
    && sosEmailConfig.recipients.length > 0
  );
}

function getSosMailTransporter() {
  if (!sosMailTransporter) {
    sosMailTransporter = nodemailer.createTransport({
      host: sosEmailConfig.host,
      port: sosEmailConfig.port,
      secure: sosEmailConfig.secure,
      auth: {
        user: sosEmailConfig.user,
        pass: sosEmailConfig.pass,
      },
      connectionTimeout: sosEmailConfig.timeoutMs,
      greetingTimeout: sosEmailConfig.timeoutMs,
      socketTimeout: sosEmailConfig.timeoutMs,
    });
  }
  return sosMailTransporter;
}

function buildSosEmail(record) {
  const deviceId = sanitizeHeaderValue(record.device_id || "unknown") || "unknown";
  const reason = String(record.reason || "设备触发紧急求助").slice(0, 500);
  const state = String(record.state || "SOS").toUpperCase();
  const receivedAt = new Date(record.received_at || Date.now());
  const localTime = receivedAt.toLocaleString("zh-CN", {
    timeZone: "Asia/Shanghai",
    hour12: false,
  });
  const dashboardLink = /^https?:\/\//i.test(sosEmailConfig.dashboardUrl)
    ? `<p><a href="${escapeHtml(sosEmailConfig.dashboardUrl)}">打开监控网页查看详情</a></p>`
    : "";

  const templateVars = {
    state,
    device_id: deviceId,
    time: localTime,
    reason,
    dashboard_url: sosEmailConfig.dashboardUrl,
  };

  const subject = fillTemplate(notificationConfig.subjectTemplate, templateVars);
  const bodyText = fillTemplate(notificationConfig.bodyTemplate, templateVars);

  return {
    subject,
    text: bodyText,
    html: [
      `<h2 style="color:#b42318">${escapeHtml(state)} 紧急通知</h2>`,
      `<p>独居老人守护终端检测到 ${escapeHtml(state)} 状态，请及时确认老人安全。</p>`,
      `<p><strong>设备：</strong>${escapeHtml(deviceId)}</p>`,
      `<p><strong>时间：</strong>${escapeHtml(localTime)}</p>`,
      `<p><strong>原因：</strong>${escapeHtml(reason)}</p>`,
      dashboardLink,
    ].join(""),
  };
}

async function sendSosEmail(record) {
  const message = buildSosEmail(record);
  const recipients = getEffectiveRecipients();
  if (recipients.length === 0) {
    console.warn("[sos-email] no recipients configured");
    return;
  }
  await getSosMailTransporter().sendMail({
    from: {
      name: sanitizeHeaderValue(sosEmailConfig.fromName),
      address: sosEmailConfig.user,
    },
    to: recipients,
    ...message,
  });
}

function scheduleSosEmail(record, reportMode) {
  if (reportMode !== "event") {
    return;
  }
  if (!shouldSendNotificationForState(record.state)) {
    return;
  }
  if (!hasSosEmailConfig()) {
    if (!sosEmailConfigWarningLogged) {
      sosEmailConfigWarningLogged = true;
      console.warn("[sos-email] SMTP configuration is incomplete; email disabled");
    }
    return;
  }

  const now = Date.now();
  const deviceId = String(record.device_id || "unknown");
  const lastSentAt = lastSosEmailAtByDevice.get(deviceId) || 0;
  if (now - lastSentAt < sosEmailConfig.dedupeMs) {
    console.log(`[sos-email] duplicate suppressed device=${deviceId}`);
    return;
  }
  lastSosEmailAtByDevice.set(deviceId, now);

  setImmediate(() => {
    sendSosEmail(record)
      .then(() => console.log(`[sos-email] sent device=${deviceId}`))
      .catch(error => {
        if (lastSosEmailAtByDevice.get(deviceId) === now) {
          lastSosEmailAtByDevice.delete(deviceId);
        }
        console.warn(`[sos-email] failed device=${deviceId}: ${error.message}`);
      });
  });
}

// ---- Agent 工具与定时器管理 ----
// 14 个 Agent 工具，充分利用所有硬件能力
const agentTools = [
  // 1. 环境传感器（AHT20 温湿度 + BH1750 光照 + MQ2 空气质量）
  {
    type: "function",
    function: {
      name: "get_environment",
      description: "查看当前家中的环境传感器数据，包括温度、湿度、光照强度和空气质量(MQ2)。老人问'现在温度多少'、'家里空气好不好'、'热不热'、'亮不亮'时调用。",
      parameters: { type: "object", properties: {} }
    }
  },
  // 2. 查看时间
  {
    type: "function",
    function: {
      name: "get_time",
      description: "查看当前日期和时间。老人问'现在几点'、'今天几号'、'星期几'时调用。",
      parameters: { type: "object", properties: {} }
    }
  },
  // 3. 设置定时提醒/闹钟
  {
    type: "function",
    function: {
      name: "set_reminder",
      description: "为老人设置一个定时提醒或闹钟。老人说'提醒我吃药'、'30分钟后叫我'、'帮我定个闹钟'、'几点提醒我'时调用。",
      parameters: {
        type: "object",
        properties: {
          minutes: {
            type: "number",
            description: "几分钟后提醒，最小1分钟，最大1440分钟（24小时）"
          },
          message: {
            type: "string",
            description: "提醒内容，用简短中文描述，例如'该吃药了'、'该喝水了'"
          }
        },
        required: ["minutes", "message"]
      }
    }
  },
  // 4. 查看待办提醒
  {
    type: "function",
    function: {
      name: "list_reminders",
      description: "查看当前所有活跃的定时提醒和待办事项。老人问'我有什么提醒'、'有没有闹钟'、'待办事情'时调用。",
      parameters: { type: "object", properties: {} }
    }
  },
  // 5. 取消提醒
  {
    type: "function",
    function: {
      name: "cancel_reminder",
      description: "取消一个已设置的定时提醒。老人说'取消闹钟'、'不要提醒了'、'删掉那个提醒'时调用。",
      parameters: {
        type: "object",
        properties: {
          id: {
            type: "string",
            description: "要取消的提醒ID"
          }
        },
        required: ["id"]
      }
    }
  },
  // 6. 确认/关闭提醒报警（OK键功能）
  {
    type: "function",
    function: {
      name: "confirm_alert",
      description: "帮老人远程确认/关闭当前的提醒报警，相当于按设备上的OK确认键。老人说'关掉报警'、'别响了'、'我没事'、'好的我知道了'时调用。",
      parameters: { type: "object", properties: {} }
    }
  },
  // 7. 屏幕显示消息（TFT）
  {
    type: "function",
    function: {
      name: "show_screen_message",
      description: "在设备的TFT彩色屏幕上显示一条文字消息，用于给老人看文字提示。老人说'给我看个字'、'显示一下'时调用，或者AI需要传达较长文字信息时使用。",
      parameters: {
        type: "object",
        properties: {
          message: {
            type: "string",
            description: "要显示在屏幕上的消息内容，简短中文，最多50个字"
          },
          duration: {
            type: "number",
            description: "显示持续时间（秒），默认5秒"
          }
        },
        required: ["message"]
      }
    }
  },
  // 8. 蜂鸣器响一次
  {
    type: "function",
    function: {
      name: "beep_once",
      description: "让设备的蜂鸣器短响一声，用于引起老人注意。老人说'响一下'、'滴一声'时调用，或AI需要提醒老人注意时使用。",
      parameters: { type: "object", properties: {} }
    }
  },
  // 9. 设备状态查询
  {
    type: "function",
    function: {
      name: "get_device_status",
      description: "查看设备整体运行状态，包括在线状态、当前风险等级、运行模式(DEMO/REAL)、Wi-Fi连接等。老人问'设备正常吗'、'现在什么状态'时调用。",
      parameters: { type: "object", properties: {} }
    }
  },
  // 10. 人体存在状态（LD2410B 毫米波）
  {
    type: "function",
    function: {
      name: "get_presence",
      description: "查看LD2410B毫米波人体存在传感器的数据，包括是否有人存在、运动/静止状态、距离和能量值。老人问'家里有人吗'、'有没有人在动'时调用。",
      parameters: { type: "object", properties: {} }
    }
  },
  // 11. 播放语音（MAX98357A 功放）
  {
    type: "function",
    function: {
      name: "play_voice",
      description: "通过设备喇叭播放一段语音提示。老人说'说句话'、'给我念一下'时调用，或AI需要用语音传达信息时使用。",
      parameters: {
        type: "object",
        properties: {
          text: {
            type: "string",
            description: "要播放的语音文本内容，简短中文，最多100个字"
          }
        },
        required: ["text"]
      }
    }
  },
  // 12. 切换语音模式
  {
    type: "function",
    function: {
      name: "set_voice_mode",
      description: "切换设备语音助手的工作模式。看护模式(CARE)优先安全提醒，互动模式(INTERACT)可以更自然地聊天。老人说'换个模式'、'切到互动'时调用。",
      parameters: {
        type: "object",
        properties: {
          mode: {
            type: "string",
            enum: ["CARE", "INTERACT"],
            description: "语音模式：CARE=看护模式，INTERACT=互动模式"
          }
        },
        required: ["mode"]
      }
    }
  },
  // 13. 传感器健康状态
  {
    type: "function",
    function: {
      name: "get_sensor_health",
      description: "查看所有传感器的工作健康状态，包括温湿度(AHT20)、光照(BH1750)、气体(MQ2)、毫米波(LD2410B)是否正常工作。老人问'传感器正常吗'、'设备有没有坏'时调用。",
      parameters: { type: "object", properties: {} }
    }
  },
  // 14. 紧急状态综合评估
  {
    type: "function",
    function: {
      name: "emergency_check",
      description: "综合评估当前家中安全状况，结合所有传感器数据和风险等级给出整体判断。老人问'家里安全吗'、'有没有问题'、'一切正常吗'时调用。",
      parameters: { type: "object", properties: {} }
    }
  }
];

// Agent 工具元数据（供网页展示）
const agentToolMeta = [
  { name: "get_environment", label: "环境数据", icon: "🌡️", desc: "温度、湿度、光照、空气质量", hardware: "AHT20 + BH1750 + MQ2" },
  { name: "get_time", label: "查看时间", icon: "🕐", desc: "当前日期和时间", hardware: "SNTP" },
  { name: "set_reminder", label: "设置提醒", icon: "⏰", desc: "定时提醒/闹钟", hardware: "服务器定时器" },
  { name: "list_reminders", label: "待办提醒", icon: "📋", desc: "查看活跃提醒列表", hardware: "服务器定时器" },
  { name: "cancel_reminder", label: "取消提醒", icon: "❌", desc: "取消已设置的提醒", hardware: "服务器定时器" },
  { name: "confirm_alert", label: "确认报警", icon: "✅", desc: "远程关闭提醒报警", hardware: "OK键(GPIO7)" },
  { name: "show_screen_message", label: "屏幕消息", icon: "📺", desc: "TFT屏幕显示文字", hardware: "ST7735 TFT" },
  { name: "beep_once", label: "蜂鸣提示", icon: "🔊", desc: "蜂鸣器短响一声", hardware: "蜂鸣器(GPIO9)" },
  { name: "get_device_status", label: "设备状态", icon: "📱", desc: "在线状态、风险等级、模式", hardware: "系统综合" },
  { name: "get_presence", label: "人体存在", icon: "👤", desc: "毫米波人体检测数据", hardware: "LD2410B" },
  { name: "play_voice", label: "播放语音", icon: "🗣️", desc: "喇叭播放文字语音", hardware: "MAX98357A" },
  { name: "set_voice_mode", label: "切换模式", icon: "🔄", desc: "看护/互动模式切换", hardware: "系统配置" },
  { name: "get_sensor_health", label: "传感器健康", icon: "💓", desc: "各传感器工作状态", hardware: "AHT20+BH1750+MQ2+LD2410B" },
  { name: "emergency_check", label: "安全评估", icon: "🛡️", desc: "综合安全状况评估", hardware: "全传感器融合" },
];

const activeTimers = [];
const pendingCommands = [];
const reminderTimeouts = new Map();
let recentAgentActions = [];

function normalizeReminder(value) {
  if (!value || typeof value !== "object") {
    return null;
  }
  const id = String(value.id || "").trim().slice(0, 80);
  const message = String(value.message || "").trim().slice(0, 200);
  const fireAtMs = Date.parse(value.fire_at);
  if (!id || !message || !Number.isFinite(fireAtMs)) {
    return null;
  }
  return {
    id,
    minutes: Number.isFinite(Number(value.minutes)) ? Number(value.minutes) : null,
    message,
    fire_at: new Date(fireAtMs).toISOString(),
    tts_text: String(value.tts_text || `提醒您，${message}。`).slice(0, 240),
    fired: Boolean(value.fired),
    fired_at: value.fired_at && Number.isFinite(Date.parse(value.fired_at))
      ? new Date(value.fired_at).toISOString()
      : null,
  };
}

function pruneReminderHistory() {
  if (activeTimers.length <= maxStoredReminders) {
    return;
  }
  const pending = activeTimers.filter(timer => !timer.fired);
  const history = activeTimers
    .filter(timer => timer.fired)
    .sort((left, right) => Date.parse(right.fired_at || right.fire_at) - Date.parse(left.fired_at || left.fire_at));
  activeTimers.splice(0, activeTimers.length, ...pending, ...history.slice(0, Math.max(0, maxStoredReminders - pending.length)));
}

function fireReminder(id) {
  reminderTimeouts.delete(id);
  const timer = activeTimers.find(item => item.id === id);
  if (!timer || timer.fired) {
    return;
  }

  timer.fired = true;
  timer.fired_at = new Date().toISOString();
  const alreadyQueued = pendingCommands.some(command => command.reminder_id === timer.id);
  if (!alreadyQueued) {
    pendingCommands.push({
      type: "play_tts",
      reminder_id: timer.id,
      tts_text: timer.tts_text,
      reason: `定时提醒: ${timer.message}`,
      created_at: timer.fired_at,
    });
    if (pendingCommands.length > maxPendingCommands) {
      pendingCommands.splice(0, pendingCommands.length - maxPendingCommands);
    }
  }
  pruneReminderHistory();
  saveStore();
}

function scheduleReminder(timer) {
  if (!timer || timer.fired || reminderTimeouts.has(timer.id)) {
    return;
  }
  const delayMs = Date.parse(timer.fire_at) - Date.now();
  if (delayMs <= 0) {
    setImmediate(() => fireReminder(timer.id));
    return;
  }
  const timeout = setTimeout(() => {
    reminderTimeouts.delete(timer.id);
    if (Date.parse(timer.fire_at) > Date.now()) {
      scheduleReminder(timer);
    } else {
      fireReminder(timer.id);
    }
  }, Math.min(delayMs, maxTimerDelayMs));
  reminderTimeouts.set(timer.id, timeout);
}

function addReminder(minutes, message) {
  const normalizedMinutes = Math.min(1440, Math.max(1, Number(minutes) || 1));
  const normalizedMessage = String(message || "提醒时间到了").trim().slice(0, 200) || "提醒时间到了";
  const id = `timer_${Date.now()}_${crypto.randomBytes(3).toString("hex")}`;
  const fire_at = new Date(Date.now() + normalizedMinutes * 60000).toISOString();
  const tts_text = `提醒您，${normalizedMessage}。`;
  const timer = {
    id,
    minutes: normalizedMinutes,
    message: normalizedMessage,
    fire_at,
    tts_text,
    fired: false,
    fired_at: null,
  };
  activeTimers.push(timer);
  pruneReminderHistory();
  scheduleReminder(timer);
  saveStore();
  return timer;
}

function getActiveTimers() {
  return activeTimers.filter(t => !t.fired);
}

function takePendingCommands() {
  const cmds = [...pendingCommands];
  pendingCommands.length = 0;
  return cmds;
}

function executeAgentTool(name, args) {
  recentAgentActions.push({ time: new Date().toISOString(), tool: name, args });
  if (recentAgentActions.length > 20) recentAgentActions.shift();

  const snap = latestSnapshot || {};

  switch (name) {
    // 1. 环境传感器数据
    case 'get_environment': {
      return {
        temperature: snap.temperature ?? '未知',
        humidity: snap.humidity ?? '未知',
        lux: snap.lux ?? '未知',
        mq2_raw: snap.mq2_raw ?? '未知',
        state: snap.state ?? '未知',
        risk_level: snap.risk_level ?? '未知',
        ld2410b_presence: snap.ld2410b_presence ?? '未知',
        current_time: new Date().toLocaleString('zh-CN', { timeZone: 'Asia/Shanghai' })
      };
    }
    // 2. 查看时间
    case 'get_time': {
      const now = new Date();
      const shanghaiTime = new Intl.DateTimeFormat('zh-CN', {
        timeZone: 'Asia/Shanghai',
        year: 'numeric', month: 'long', day: 'numeric',
        weekday: 'long',
        hour: '2-digit', minute: '2-digit', second: '2-digit',
        hour12: false,
      }).format(now);
      return { current_time: shanghaiTime, timestamp: now.toISOString() };
    }
    // 3. 设置定时提醒
    case 'set_reminder': {
      const timer = addReminder(args.minutes, args.message);
      return { 
        success: true, 
        message: `已设置${timer.minutes}分钟后提醒: ${timer.message}`,
        fire_at: timer.fire_at,
        id: timer.id
      };
    }
    // 4. 查看待办提醒
    case 'list_reminders': {
      const timers = getActiveTimers();
      return {
        count: timers.length,
        reminders: timers.map(t => ({
          id: t.id,
          message: t.message,
          fire_at: t.fire_at,
          minutes: t.minutes
        }))
      };
    }
    // 5. 取消提醒
    case 'cancel_reminder': {
      const id = String(args.id || '').trim();
      if (!id) return { success: false, reason: '需要提供提醒ID' };
      const idx = activeTimers.findIndex(t => t.id === id && !t.fired);
      if (idx < 0) return { success: false, reason: `未找到ID为${id}的活跃提醒` };
      const cancelled = activeTimers.splice(idx, 1)[0];
      const timeout = reminderTimeouts.get(id);
      if (timeout) { clearTimeout(timeout); reminderTimeouts.delete(id); }
      saveStore();
      return { success: true, message: `已取消提醒: ${cancelled.message}` };
    }
    // 6. 确认/关闭提醒报警（设备命令）
    case 'confirm_alert': {
      pendingCommands.push({
        type: 'confirm_alert',
        created_at: new Date().toISOString()
      });
      return { success: true, message: '已发送确认指令到设备，提醒将在几秒内关闭。' };
    }
    // 7. 屏幕显示消息（设备命令）
    case 'show_screen_message': {
      const message = String(args.message || '').trim().slice(0, 100);
      if (!message) return { success: false, reason: '消息内容不能为空' };
      const duration = Math.min(30, Math.max(1, Number(args.duration) || 5));
      pendingCommands.push({
        type: 'show_screen_message',
        message,
        duration,
        created_at: new Date().toISOString()
      });
      return { success: true, message: `已发送屏幕消息: ${message}（显示${duration}秒）` };
    }
    // 8. 蜂鸣器响一次（设备命令）
    case 'beep_once': {
      pendingCommands.push({
        type: 'beep_once',
        created_at: new Date().toISOString()
      });
      return { success: true, message: '已发送蜂鸣指令到设备' };
    }
    // 9. 设备状态查询
    case 'get_device_status': {
      return {
        state: snap.state ?? '未知',
        risk_level: snap.risk_level ?? '未知',
        reason: snap.reason ?? '无',
        run_mode: snap.run_mode ?? '未知',
        online: snap.state ? true : false,
        no_motion_remind_ms: snap.no_motion_remind_ms ?? '未知',
        remind_confirm_timeout_ms: snap.remind_confirm_timeout_ms ?? '未知',
        current_time: new Date().toLocaleString('zh-CN', { timeZone: 'Asia/Shanghai' })
      };
    }
    // 10. 人体存在状态（LD2410B）
    case 'get_presence': {
      return {
        ld2410b_ok: snap.ld2410b_ok ?? '未知',
        ld2410b_presence: snap.ld2410b_presence ?? '未知',
        ld2410b_moving_target: snap.ld2410b_moving_target ?? '未知',
        ld2410b_stationary_target: snap.ld2410b_stationary_target ?? '未知',
        ld2410b_moving_distance_cm: snap.ld2410b_moving_distance_cm ?? '未知',
        ld2410b_stationary_distance_cm: snap.ld2410b_stationary_distance_cm ?? '未知',
        ld2410b_detection_distance_cm: snap.ld2410b_detection_distance_cm ?? '未知',
        ld2410b_moving_energy: snap.ld2410b_moving_energy ?? '未知',
        ld2410b_stationary_energy: snap.ld2410b_stationary_energy ?? '未知',
        mmwave_fusion_active: snap.mmwave_fusion_active ?? '未知'
      };
    }
    // 11. 播放语音（设备命令）
    case 'play_voice': {
      const text = String(args.text || '').trim().slice(0, 200);
      if (!text) return { success: false, reason: '语音文本不能为空' };
      pendingCommands.push({
        type: 'play_tts',
        tts_text: text,
        url: `/api/agent/notification-audio?text=${encodeURIComponent(text)}`,
        created_at: new Date().toISOString()
      });
      return { success: true, message: `已发送语音播放指令: ${text.slice(0, 30)}...` };
    }
    // 12. 切换语音模式
    case 'set_voice_mode': {
      const mode = String(args.mode || '').toUpperCase().trim();
      if (!manualVoiceModes.has(mode)) {
        return { success: false, reason: `无效模式: ${mode}，请使用 CARE 或 INTERACT` };
      }
      const oldMode = currentVoiceMode;
      currentVoiceMode = mode;
      console.log(`[voice-mode] changed by agent: ${oldMode} -> ${mode}`);
      return { success: true, message: `语音模式已切换为${voiceModeLabel(mode)}`, old_mode: oldMode, new_mode: mode };
    }
    // 13. 传感器健康状态
    case 'get_sensor_health': {
      return {
        aht20_ok: snap.sensor_aht20_ok ?? '未知',
        bh1750_ok: snap.sensor_bh1750_ok ?? '未知',
        mq2_ok: snap.sensor_mq2_ok ?? '未知',
        ld2410b_ok: snap.ld2410b_ok ?? '未知',
        all_sensors_healthy: !!(snap.sensor_aht20_ok && snap.sensor_bh1750_ok && snap.sensor_mq2_ok && snap.ld2410b_ok),
        current_time: new Date().toLocaleString('zh-CN', { timeZone: 'Asia/Shanghai' })
      };
    }
    // 14. 紧急状态综合评估
    case 'emergency_check': {
      const state = snap.state ?? '未知';
      const riskLevel = snap.risk_level ?? '未知';
      const allSensorsOk = !!(snap.sensor_aht20_ok && snap.sensor_bh1750_ok && snap.sensor_mq2_ok && snap.ld2410b_ok);
      let assessment = '一切正常';
      if (state === 'SOS') assessment = '紧急求助状态，请立即关注';
      else if (state === 'ALARM') assessment = '告警状态，需要关注';
      else if (state === 'REMIND') assessment = '提醒状态，建议确认';
      else if (!allSensorsOk) assessment = '部分传感器异常，需要检查';
      return {
        state, risk_level: riskLevel, reason: snap.reason ?? '无',
        all_sensors_healthy: allSensorsOk,
        assessment,
        temperature: snap.temperature ?? '未知',
        humidity: snap.humidity ?? '未知',
        mq2_raw: snap.mq2_raw ?? '未知',
        ld2410b_presence: snap.ld2410b_presence ?? '未知',
        current_time: new Date().toLocaleString('zh-CN', { timeZone: 'Asia/Shanghai' })
      };
    }
    default:
      return { error: `未知工具: ${name}` };
  }
}
// --------------------------------

const defaultVoicePromptItems = [
  {
    event_key: "no_motion_remind",
    label: "NORMAL -> REMIND / 长时间无活动",
    tts_text: "长时间未检测到活动，请按确认键报平安。",
    enabled: true,
    cooldown_ms: 30000,
  },
  {
    event_key: "high_temp_remind",
    label: "NORMAL -> REMIND / 高温",
    tts_text: "室内温度偏高，请注意通风、降温和补水。",
    enabled: true,
    cooldown_ms: 30000,
  },
  {
    event_key: "mq2_mild_remind",
    label: "NORMAL -> REMIND / MQ2 轻度异常",
    tts_text: "检测到烟雾或气体异常，请立即检查现场情况。",
    enabled: true,
    cooldown_ms: 30000,
  },
  {
    event_key: "no_motion_timeout_alarm",
    label: "REMIND -> ALARM / 无活动提醒超时",
    tts_text: "长时间未收到确认，已升级报警，请立即查看。",
    enabled: true,
    cooldown_ms: 15000,
  },
  {
    event_key: "mq2_mild_timeout_alarm",
    label: "REMIND -> ALARM / MQ2 轻度异常提醒超时",
    tts_text: "烟雾或气体异常持续存在，已升级报警，请立即处理。",
    enabled: true,
    cooldown_ms: 15000,
  },
  {
    event_key: "mq2_danger_alarm",
    label: "NORMAL/REMIND -> ALARM / MQ2 高危险",
    tts_text: "检测到高风险烟雾或气体异常，请立即远离并检查现场。",
    enabled: true,
    cooldown_ms: 10000,
  },
  {
    event_key: "mq2_temp_alarm",
    label: "NORMAL/REMIND -> ALARM / MQ2 异常并伴随温升",
    tts_text: "检测到烟雾异常并伴随升温，请立即处理并注意安全。",
    enabled: true,
    cooldown_ms: 10000,
  },
  {
    event_key: "fall_detected_alarm",
    label: "NORMAL/REMIND -> ALARM / 疑似跌倒",
    tts_text: "检测到疑似跌倒，已启动报警并通知家属，请保持镇定。",
    enabled: true,
    cooldown_ms: 10000,
  },
  {
    event_key: "manual_sos",
    label: "任意状态 -> SOS / 用户主动求助",
    tts_text: "已发出紧急求助，请保持镇定并等待帮助。",
    enabled: false,
    cooldown_ms: 0,
  },
  {
    event_key: "user_confirm_normal",
    label: "REMIND/ALARM/SOS -> NORMAL / 用户确认解除",
    tts_text: "已恢复正常监测。",
    enabled: true,
    cooldown_ms: 5000,
  },
  {
    event_key: "risk_cleared_normal",
    label: "REMIND -> NORMAL / 风险自行解除",
    tts_text: "风险已解除，已恢复正常监测。",
    enabled: true,
    cooldown_ms: 5000,
  },
];

const requiredFields = [
  "device_id",
  "state",
  "risk_level",
  "reason",
  "temperature",
  "humidity",
  "lux",
  "mq2_raw",
  "timestamp_ms",
];

function validateAlertPayload(payload) {
  if (payload == null || typeof payload !== "object" || Array.isArray(payload)) {
    return "payload must be a JSON object";
  }

  for (const field of requiredFields) {
    if (!(field in payload)) {
      return `missing field: ${field}`;
    }
  }

  return null;
}

function tokenMatches(providedToken, expectedToken) {
  if (typeof providedToken !== "string" || providedToken.length === 0) {
    return false;
  }

  const provided = Buffer.from(providedToken);
  const expected = Buffer.from(expectedToken);
  return provided.length === expected.length && crypto.timingSafeEqual(provided, expected);
}

function parseCookies(req) {
  const header = req.headers.cookie || "";
  const cookies = {};
  for (const part of header.split(";")) {
    const index = part.indexOf("=");
    if (index <= 0) {
      continue;
    }
    const key = part.slice(0, index).trim();
    const value = part.slice(index + 1).trim();
    cookies[key] = decodeURIComponent(value);
  }
  return cookies;
}

function safeStringEqual(left, right) {
  const leftBuffer = Buffer.from(String(left || ""));
  const rightBuffer = Buffer.from(String(right || ""));
  return leftBuffer.length === rightBuffer.length && crypto.timingSafeEqual(leftBuffer, rightBuffer);
}

function createDashboardSession() {
  const token = crypto.randomBytes(32).toString("hex");
  dashboardSessions.add(token);
  return token;
}

function getDashboardSession(req) {
  const token = parseCookies(req).elder_alert_session;
  return dashboardSessions.has(token) ? token : null;
}

function setDashboardSessionCookie(res, token) {
  res.setHeader(
    "Set-Cookie",
    `elder_alert_session=${encodeURIComponent(token)}; HttpOnly; SameSite=Lax; Path=/; Max-Age=43200`
  );
}

function clearDashboardSessionCookie(res) {
  res.setHeader("Set-Cookie", "elder_alert_session=; HttpOnly; SameSite=Lax; Path=/; Max-Age=0");
}

function requireDashboardAuth(req, res, next) {
  if (getDashboardSession(req)) {
    return next();
  }

  if (req.path.startsWith("/api/")) {
    return res.status(401).json({ ok: false, message: "未登录" });
  }
  if (req.accepts("html")) {
    return res.redirect("/login.html");
  }
  return res.status(401).json({ ok: false, message: "未登录" });
}

function requireDashboardOrDeviceToken(req, res, next) {
  if (getDashboardSession(req)) {
    return next();
  }
  if (!deviceToken) {
    return next();
  }
  if (tokenMatches(req.get("X-Device-Token"), deviceToken)) {
    return next();
  }
  return res.status(401).json({ ok: false, message: "unauthorized" });
}

function hasTencentAsrConfig() {
  return Boolean(tencentAsrConfig.secretId && tencentAsrConfig.secretKey);
}

function hasAiReplyConfig() {
  return Boolean(aiReplyConfig.apiKey && aiReplyConfig.baseUrl && aiReplyConfig.model);
}

function hasTtsConfig() {
  return Boolean(ttsConfig.apiKey && ttsConfig.baseUrl && ttsConfig.model);
}

function normalizeManualVoiceMode(value) {
  const mode = String(value || "").trim().toUpperCase();
  return manualVoiceModes.has(mode) ? mode : "CARE";
}

function normalizeVoiceMode(value) {
  const mode = String(value || "").trim().toUpperCase();
  return mode === "OFFLINE" || manualVoiceModes.has(mode) ? mode : "CARE";
}

function normalizeVoicePromptItem(item, fallback = {}) {
  const eventKey = String(item?.event_key || fallback.event_key || "").trim();
  if (!eventKey) {
    return null;
  }

  const label = String(item?.label || fallback.label || "").trim();
  const text = String(item?.tts_text || fallback.tts_text || "").trim();
  const cooldown = Number(item?.cooldown_ms ?? fallback.cooldown_ms ?? 0);
  const updatedAt = String(item?.updated_at || fallback.updated_at || new Date().toISOString());

  return {
    event_key: eventKey,
    label,
    tts_text: text,
    enabled: Boolean(item?.enabled ?? fallback.enabled),
    cooldown_ms: Number.isFinite(cooldown) && cooldown >= 0 ? Math.round(cooldown) : 0,
    updated_at: updatedAt,
  };
}

function buildDefaultVoicePrompts() {
  return defaultVoicePromptItems.map((item) =>
    normalizeVoicePromptItem(
      {
        ...item,
        updated_at: new Date().toISOString(),
      },
      item,
    )
  );
}

function mergeVoicePrompts(storedItems) {
  const storedMap = new Map(
    Array.isArray(storedItems)
      ? storedItems
          .map((item) => normalizeVoicePromptItem(item, item))
          .filter(Boolean)
          .map((item) => [item.event_key, item])
      : []
  );

  return defaultVoicePromptItems.map((item) =>
    normalizeVoicePromptItem(storedMap.get(item.event_key) || item, item)
  );
}

function getVoicePromptByEventKey(eventKey) {
  return voicePrompts.find((item) => item.event_key === eventKey) || null;
}

function normalizeRunMode(value) {
  const upper = String(value || "").trim().toUpperCase();
  return upper === "DEMO" || upper === "REAL" ? upper : null;
}

function isDeviceOffline() {
  if (!latestSnapshot?.received_at) {
    return true;
  }
  const receivedAt = new Date(latestSnapshot.received_at);
  if (Number.isNaN(receivedAt.getTime())) {
    return true;
  }
  const offlineAfterMs = Number.isFinite(deviceOfflineAfterMs) && deviceOfflineAfterMs > 0
    ? deviceOfflineAfterMs
    : 45000;
  return Date.now() - receivedAt.getTime() >= offlineAfterMs;
}

function getEffectiveVoiceMode() {
  return isDeviceOffline() ? "OFFLINE" : currentVoiceMode;
}

function voiceModeLabel(mode) {
  switch (normalizeVoiceMode(mode)) {
    case "OFFLINE":
      return "离线模式";
    case "INTERACT":
      return "互动模式";
    case "CARE":
    default:
      return "看护模式";
  }
}

function getLatestDeviceContext() {
  if (!latestSnapshot) {
    return "当前没有设备状态快照。";
  }

  return [
    `设备状态: ${latestSnapshot.state ?? "-"}`,
    `风险等级: ${latestSnapshot.risk_level ?? "-"}`,
    `原因: ${latestSnapshot.reason ?? "-"}`,
    `温度: ${latestSnapshot.temperature ?? "-"}C`,
    `湿度: ${latestSnapshot.humidity ?? "-"}%`,
    `光照: ${latestSnapshot.lux ?? "-"}lx`,
    `MQ2: ${latestSnapshot.mq2_raw ?? "-"}`,
    `毫米波有效: ${latestSnapshot.ld2410b_ok ?? "-"}`,
    `毫米波存在: ${latestSnapshot.ld2410b_presence ?? "-"}`,
    `毫米波运动: ${latestSnapshot.ld2410b_moving_target ?? "-"}`,
    `毫米波静止: ${latestSnapshot.ld2410b_stationary_target ?? "-"}`,
    `毫米波距离: ${latestSnapshot.ld2410b_detection_distance_cm ?? "-"}cm`,
  ].join("\n");
}

function sanitizeAiReply(value) {
  return String(value || "")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, 120);
}

function isPromptEchoReply(value) {
  const text = String(value || "");
  return /用户说|关键指令|设备状态|风险等级|最多\d+个汉字|直接朗读|不要复述|不要解释|首先/.test(text);
}

function buildFallbackAiReply(transcript, mode = currentVoiceMode) {
  const text = String(transcript || "");
  const normalizedMode = normalizeVoiceMode(mode);
  if (/几点|时间|现在.*点/.test(text)) {
    const time = new Intl.DateTimeFormat("zh-CN", {
      timeZone: "Asia/Shanghai",
      hour: "2-digit",
      minute: "2-digit",
      hour12: false,
    }).format(new Date());
    return `现在是${time}。`;
  }
  if (/救命|不舒服|难受|疼|摔|晕|危险|报警/.test(text)) {
    return "请马上按SOS键或联系家属。";
  }
  if (/吃药|药|用药|剂量|病|医生|血压|血糖/.test(text)) {
    return "这个我不能判断，请咨询医生或家属。";
  }
  if (normalizedMode === "CARE" && /天气|新闻|外面|路线|导航|电话|微信|支付|故事|笑话|唱歌|聊天|陪我/.test(text)) {
    return "这个我在看护模式下不方便聊，可以切换到互动模式试试。";
  }
  if (/听到|听见|听清|能听/.test(text)) {
    return normalizedMode === "INTERACT" ? "听见了，您可以继续说。" : "听见了，我在看护您。";
  }
  if (/干嘛|在吗|你好|喂/.test(text)) {
    return normalizedMode === "INTERACT" ? "我在呢，有什么事尽管说。" : "我在帮您留意家里安全。";
  }
  if (normalizedMode === "INTERACT") {
    return "嗯，我在听，您可以继续说。";
  }
  return "这个我在看护模式下不方便回答，可以切换到互动模式。";
}

function buildAiSystemPrompt(mode) {
  const normalizedMode = normalizeVoiceMode(mode);
  if (normalizedMode === "OFFLINE") {
    return "";
  }
  
  const timers = getActiveTimers();
  const timerInfo = timers.length > 0 
    ? `\n当前活跃提醒: ${timers.map(t => `${t.message}(${t.fire_at})`).join('、')}`
    : '\n当前没有活跃提醒。';

  /* 患者信息注入 */
  let patientInfo = '';
  if (patientContext.name || patientContext.address || patientContext.conditions || patientContext.notes) {
    const parts = [];
    if (patientContext.name) parts.push(`称呼：${patientContext.name}`);
    if (patientContext.address) parts.push(`称呼方式：${patientContext.address}`);
    if (patientContext.conditions) parts.push(`健康状况：${patientContext.conditions}`);
    if (patientContext.notes) parts.push(`注意事项：${patientContext.notes}`);
    patientInfo = `\n【患者信息】${parts.join('；')}。\n`;
  }

  if (normalizedMode === "INTERACT") {
    return (
      "你是独居老人的语音助手，可以自由聊天，也可以调用工具帮老人。回复最多50字，简短亲切。" +
      "需要查数据、定闹钟、关提醒等操作时直接调用工具。危险求救提醒按SOS键。" + patientInfo + timerInfo
    );
  }

  /* CARE 看护模式：只做安全播报，不接管聊天 */
  return (
    "你是独居老人家里的安全看护语音助手。只回答与安全、健康看护相关的问题，最多40个汉字。\n" +
    "你的职责是：播报安全状态、解释风险原因、提醒老人确认报警。\n" +
    "不要陪聊、不要讲故事、不要闲聊。与安全无关的问题请说'这个我在看护模式下不方便聊，可以切换到互动模式试试'。\n" +
    "可以使用工具定闹钟、查环境数据、确认提醒，但优先保持简洁的安全播报。\n" +
    "危险求救请提醒按SOS键。" +
    "不要反复说我听到了，不要复述用户原话，不要列设备状态，不要输出分析过程。" + patientInfo + timerInfo
  );
}

function extractAiReply(data) {
  const choices = Array.isArray(data?.choices) ? data.choices : [];

  for (const choice of choices) {
    const message = choice?.message || choice?.delta || {};
    const candidates = [
      message.content,
      choice.text,
      choice.content,
    ];

    for (const candidate of candidates) {
      if (Array.isArray(candidate)) {
        const text = candidate
          .map((part) => part?.text || part?.content || "")
          .join(" ");
        const reply = sanitizeAiReply(text);
        if (reply && !isPromptEchoReply(reply)) {
          return reply;
        }
      } else {
        const reply = sanitizeAiReply(candidate);
        if (reply && !isPromptEchoReply(reply)) {
          return reply;
        }
      }
    }
  }

  return "";
}

function mimeTypeForAudioFormat(format) {
  switch (String(format || "").toLowerCase()) {
    case "mp3":
      return "audio/mpeg";
    case "pcm":
      return "application/octet-stream";
    case "wav":
    default:
      return "audio/wav";
  }
}

function extractTtsAudio(data) {
  function decodeAudioCandidate(candidate) {
    if (typeof candidate === "string") {
      const value = candidate.trim();
      if (!value) {
        return null;
      }
      const base64 = value.startsWith("data:")
        ? value.slice(value.indexOf(",") + 1)
        : value;
      if (base64.length < 64 || !/^[A-Za-z0-9+/=\s]+$/.test(base64)) {
        return null;
      }
      const buffer = Buffer.from(base64, "base64");
      return buffer.length > 0 ? buffer : null;
    }

    if (Array.isArray(candidate)) {
      for (const item of candidate) {
        const buffer = decodeAudioCandidate(item);
        if (buffer) {
          return buffer;
        }
      }
      return null;
    }

    if (candidate && typeof candidate === "object") {
      const keys = ["data", "b64_json", "base64", "audio"];
      for (const key of keys) {
        const buffer = decodeAudioCandidate(candidate[key]);
        if (buffer) {
          return buffer;
        }
      }
    }

    return null;
  }

  const topLevelCandidates = [
    data?.audio?.data,
    data?.audio,
    data?.output_audio,
  ];
  for (const candidate of topLevelCandidates) {
    const buffer = decodeAudioCandidate(candidate);
    if (buffer) {
      return buffer;
    }
  }

  const choices = Array.isArray(data?.choices) ? data.choices : [];

  for (const choice of choices) {
    const message = choice?.message || {};
    const candidates = [
      message.audio?.data,
      message.audio,
      choice.audio?.data,
      choice.audio,
      message.content,
    ];

    for (const candidate of candidates) {
      const buffer = decodeAudioCandidate(candidate);
      if (buffer) {
        return buffer;
      }
    }
  }

  return null;
}

function describeError(error) {
  const parts = [error?.message || String(error)];
  if (error?.code) {
    parts.push(`code=${error.code}`);
  }
  if (error?.cause) {
    parts.push(`cause=${error.cause.message || String(error.cause)}`);
    if (error.cause.code) {
      parts.push(`cause_code=${error.cause.code}`);
    }
    if (error.cause.errno) {
      parts.push(`cause_errno=${error.cause.errno}`);
    }
  }
  return parts.join(" ");
}

function sha256Hex(value) {
  return crypto.createHash("sha256").update(value, "utf8").digest("hex");
}

function hmacSha256(key, value, outputEncoding) {
  return crypto.createHmac("sha256", key).update(value, "utf8").digest(outputEncoding);
}

function getTencentCloudAuthorization({ timestamp, payload }) {
  const service = "asr";
  const host = "asr.tencentcloudapi.com";
  const algorithm = "TC3-HMAC-SHA256";
  const date = new Date(timestamp * 1000).toISOString().slice(0, 10);
  const canonicalRequest = [
    "POST",
    "/",
    "",
    "content-type:application/json; charset=utf-8",
    `host:${host}`,
    "",
    "content-type;host",
    sha256Hex(payload),
  ].join("\n");
  const credentialScope = `${date}/${service}/tc3_request`;
  const stringToSign = [
    algorithm,
    String(timestamp),
    credentialScope,
    sha256Hex(canonicalRequest),
  ].join("\n");
  const secretDate = hmacSha256(`TC3${tencentAsrConfig.secretKey}`, date);
  const secretService = hmacSha256(secretDate, service);
  const secretSigning = hmacSha256(secretService, "tc3_request");
  const signature = hmacSha256(secretSigning, stringToSign, "hex");

  return `${algorithm} Credential=${tencentAsrConfig.secretId}/${credentialScope}, SignedHeaders=content-type;host, Signature=${signature}`;
}

function normalizeVoiceFormat(value) {
  const normalized = String(value || "wav").trim().toLowerCase();
  const aliases = {
    opus: "ogg-opus",
    ogg: "ogg-opus",
  };
  const voiceFormat = aliases[normalized] || normalized;
  const allowedFormats = new Set(["wav", "mp3", "m4a", "pcm", "ogg-opus", "speex", "silk", "aac", "amr"]);
  return allowedFormats.has(voiceFormat) ? voiceFormat : "wav";
}

async function transcribeWithTencentAsr(audioBuffer, voiceFormat) {
  if (!hasTencentAsrConfig()) {
    const error = new Error("Tencent ASR credentials are not configured");
    error.statusCode = 503;
    throw error;
  }

  if (typeof fetch !== "function") {
    const error = new Error("Node.js fetch API is unavailable; use Node.js 18 or newer");
    error.statusCode = 500;
    throw error;
  }

  const payload = JSON.stringify({
    SubServiceType: 2,
    ProjectId: 0,
    EngSerViceType: tencentAsrConfig.engine || "16k_zh",
    VoiceFormat: voiceFormat,
    SourceType: 1,
    Data: audioBuffer.toString("base64"),
    DataLen: audioBuffer.length,
  });
  const timestamp = Math.floor(Date.now() / 1000);
  const response = await fetch("https://asr.tencentcloudapi.com", {
    method: "POST",
    headers: {
      Authorization: getTencentCloudAuthorization({ timestamp, payload }),
      "Content-Type": "application/json; charset=utf-8",
      Host: "asr.tencentcloudapi.com",
      "X-TC-Action": "SentenceRecognition",
      "X-TC-Version": "2019-06-14",
      "X-TC-Region": tencentAsrConfig.region || "ap-shanghai",
      "X-TC-Timestamp": String(timestamp),
    },
    body: payload,
  });
  const data = await response.json().catch(() => null);
  const apiResponse = data && data.Response ? data.Response : null;

  if (!response.ok || !apiResponse || apiResponse.Error) {
    const message = apiResponse?.Error?.Message || response.statusText || "Tencent ASR request failed";
    const error = new Error(message);
    error.statusCode = response.ok ? 502 : response.status;
    error.tencentRequestId = apiResponse?.RequestId || null;
    throw error;
  }

  return apiResponse;
}

async function generateAiReply(transcript, mode = currentVoiceMode) {
  const normalizedMode = normalizeVoiceMode(mode);
  const text = String(transcript || "").trim();
  if (!text && normalizedMode !== "OFFLINE") {
    return null;
  }

  if (normalizedMode === "OFFLINE") {
    return offlineVoiceReply;
  }

  if (!hasAiReplyConfig()) {
    return null;
  }

  if (typeof fetch !== "function") {
    const error = new Error("Node.js fetch API is unavailable; use Node.js 18 or newer");
    error.statusCode = 500;
    throw error;
  }

  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), Number.isFinite(aiReplyConfig.timeoutMs) ? aiReplyConfig.timeoutMs : 12000);
  
  let messages = [
    { role: "system", content: buildAiSystemPrompt(normalizedMode) },
    {
      role: "user",
      content:
        `老人刚说：${text}\n` +
        `设备状态仅供你内部判断，不要复述：\n${getLatestDeviceContext()}\n` +
        "请给出一句自然、有用的回复。",
    }
  ];

  try {
    for (let round = 0; round < 2; round++) {
      const payload = {
        model: aiReplyConfig.model,
        messages,
        temperature: 0.3,
        max_tokens: 80,
        tools: agentTools,
        tool_choice: "auto"
      };

      const response = await fetch(`${aiReplyConfig.baseUrl}/chat/completions`, {
        method: "POST",
        headers: {
          Authorization: `Bearer ${aiReplyConfig.apiKey}`,
          "Content-Type": "application/json",
        },
        body: JSON.stringify(payload),
        signal: controller.signal,
      });
      const data = await response.json().catch(() => null);

      if (!response.ok) {
        const message = data?.error?.message || response.statusText || "AI reply request failed";
        const error = new Error(message);
        error.statusCode = response.status;
        throw error;
      }

      const choice = data?.choices?.[0];
      if (!choice) return null;

      if (choice.finish_reason === 'tool_calls' || choice.message?.tool_calls) {
        messages.push(choice.message);
        
        for (const tc of choice.message.tool_calls) {
          let args = {};
          try { args = JSON.parse(tc.function.arguments); } catch(e) {}
          const result = executeAgentTool(tc.function.name, args);
          messages.push({
            role: "tool",
            tool_call_id: tc.id,
            content: JSON.stringify(result)
          });
        }
        continue;
      }

      const reply = extractAiReply(data);
      return reply || buildFallbackAiReply(text, normalizedMode);
    }
    return buildFallbackAiReply(text, normalizedMode);
  } finally {
    clearTimeout(timeout);
  }
}

async function runAiReplyGeneration(record, transcript) {
  const mode = normalizeVoiceMode(record.voice_mode);
  if (mode !== "OFFLINE" && (!hasAiReplyConfig() || !String(transcript || "").trim())) {
    return false;
  }

  try {
    const reply = await generateAiReply(transcript, mode);
    record.ai_reply = reply;
    record.ai_error = null;
    saveStore();
    console.log(`[speech-ai] reply generated mode=${mode} model=${record.ai_model || "offline"} reply=${reply || ""}`);
    return Boolean(reply);
  } catch (error) {
    record.ai_error = error.message;
    saveStore();
    console.warn(`[speech-ai] reply failed: ${error.message}`);
    return false;
  }
}

function waitForBoolean(promise, timeoutMs) {
  if (!Number.isFinite(timeoutMs) || timeoutMs <= 0) {
    return Promise.resolve(false);
  }

  return Promise.race([
    promise,
    new Promise((resolve) => setTimeout(() => resolve(false), timeoutMs)),
  ]);
}

/**
 * 从 MiniMax /t2a_v2 响应中提取 hex 编码的音频数据并转为 Buffer。
 * MiniMax 返回 JSON: { "data": { "audio": "hex_encoded_audio", "status": 0, ... } }
 */
function extractMinimaxTtsAudio(data) {
  const hex = data?.data?.audio || data?.audio;
  if (typeof hex !== "string" || hex.length === 0) {
    return null;
  }
  if (!/^[0-9a-fA-F]+$/.test(hex)) {
    return null;
  }
  return Buffer.from(hex, "hex");
}

/**
 * 调用 MiniMax /t2a_v2 端点生成 TTS 音频。
 * MiniMax API 格式与 OpenAI /chat/completions 不同，需要单独的请求体和响应解析。
 */
async function generateMinimaxTtsAudio(text, controller) {
  const payload = {
    model: ttsConfig.model,
    text: text,
    stream: false,
    voice_setting: {
      voice_id: ttsConfig.voice,
    },
    audio_setting: {
      sample_rate: ttsConfig.sampleRate,
      bitrate: 128000,
      format: ttsConfig.format,
      channel: 1,
    },
  };

  const response = await fetch(`${ttsConfig.baseUrl}/t2a_v2`, {
    method: "POST",
    headers: {
      Authorization: `Bearer ${ttsConfig.apiKey}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify(payload),
    signal: controller.signal,
  });

  if (!response.ok) {
    const data = await response.json().catch(() => null);
    const providerMessage =
      data?.error?.message ||
      data?.message ||
      data?.Message ||
      data?.msg ||
      response.statusText ||
      "MiniMax TTS request failed";
    const message = `${providerMessage}${data ? ` body=${JSON.stringify(data).slice(0, 500)}` : ""}`;
    const error = new Error(message);
    error.statusCode = response.status;
    throw error;
  }

  const contentType = response.headers.get("content-type") || "";
  let audioBuffer;
  if (contentType.includes("application/json")) {
    const data = await response.json();
    audioBuffer = extractMinimaxTtsAudio(data);
    if (!audioBuffer || audioBuffer.length === 0) {
      const error = new Error("MiniMax TTS audio was empty");
      error.statusCode = 502;
      throw error;
    }
  } else {
    audioBuffer = Buffer.from(await response.arrayBuffer());
    if (!audioBuffer || audioBuffer.length === 0) {
      const error = new Error("MiniMax TTS audio was empty");
      error.statusCode = 502;
      throw error;
    }
  }

  return audioBuffer;
}

async function generateReplyAudio(text) {
  if (!hasTtsConfig()) {
    const error = new Error("TTS model is not configured");
    error.statusCode = 503;
    throw error;
  }

  const cleanText = sanitizeAiReply(text);
  if (!cleanText) {
    const error = new Error("TTS text is empty");
    error.statusCode = 400;
    throw error;
  }

  if (typeof fetch !== "function") {
    const error = new Error("Node.js fetch API is unavailable; use Node.js 18 or newer");
    error.statusCode = 500;
    throw error;
  }

  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), Number.isFinite(ttsConfig.timeoutMs) ? ttsConfig.timeoutMs : 20000);

  try {
    /* MiniMax TTS 分支：使用 /t2a_v2 端点 */
    if (ttsConfig.provider === "minimax") {
      return await generateMinimaxTtsAudio(cleanText, controller);
    }

    /* 默认分支：OpenAI 兼容 /chat/completions 端点 */
    const payload = {
      model: ttsConfig.model,
      messages: [
        {
          role: "assistant",
          content: cleanText,
        },
      ],
      modalities: ["audio"],
      audio: {
        voice: ttsConfig.voice,
        format: ttsConfig.format,
      },
    };

    const response = await fetch(`${ttsConfig.baseUrl}/chat/completions`, {
      method: "POST",
      headers: {
        Authorization: `Bearer ${ttsConfig.apiKey}`,
        "Content-Type": "application/json",
      },
      body: JSON.stringify(payload),
      signal: controller.signal,
    });

    if (!response.ok) {
      const data = await response.json().catch(() => null);
      const providerMessage =
        data?.error?.message ||
        data?.message ||
        data?.Message ||
        data?.msg ||
        response.statusText ||
        "TTS request failed";
      const message = `${providerMessage}${data ? ` body=${JSON.stringify(data).slice(0, 500)}` : ""}`;
      const error = new Error(message);
      error.statusCode = response.status;
      throw error;
    }

    const contentType = response.headers.get("content-type") || "";
    let audioBuffer = Buffer.from(await response.arrayBuffer());
    const responseText = audioBuffer.toString("utf8");
    const looksLikeJson = /^[\s]*[\[{]/.test(responseText);
    if (contentType.includes("application/json") || looksLikeJson) {
      const data = JSON.parse(responseText);
      audioBuffer = extractTtsAudio(data);
    }
    if (!audioBuffer || audioBuffer.length === 0) {
      const error = new Error("TTS audio was empty");
      error.statusCode = 502;
      throw error;
    }

    return audioBuffer;
  } finally {
    clearTimeout(timeout);
  }
}

function requireDeviceToken(req, res, next) {
  if (!deviceToken) {
    return next();
  }

  if (!tokenMatches(req.get("X-Device-Token"), deviceToken)) {
    return res.status(401).json({
      ok: false,
      message: "unauthorized",
    });
  }

  return next();
}

function ensureDataDir() {
  fs.mkdirSync(dataDir, { recursive: true });
}

function loadStore() {
  try {
    ensureDataDir();
    voicePrompts = buildDefaultVoicePrompts();
    if (!fs.existsSync(storePath)) {
      return;
    }

    const raw = fs.readFileSync(storePath, "utf8");
    if (!raw.trim()) {
      return;
    }

    const parsed = JSON.parse(raw);
    const loadedAlerts = Array.isArray(parsed.alerts) ? parsed.alerts.slice(0, maxAlerts) : [];
    const loadedSpeechRecords = Array.isArray(parsed.speechRecords)
      ? parsed.speechRecords.slice(0, maxSpeechRecords)
      : [];
    alerts.splice(0, alerts.length, ...loadedAlerts);
    speechRecords.splice(0, speechRecords.length, ...loadedSpeechRecords);
    latestSnapshot = parsed.latestSnapshot && typeof parsed.latestSnapshot === "object"
      ? parsed.latestSnapshot
      : null;
    latestSpeech = parsed.latestSpeech && typeof parsed.latestSpeech === "object"
      ? parsed.latestSpeech
      : null;
    voicePrompts = mergeVoicePrompts(parsed.voicePrompts);
    preferredRunMode = normalizeRunMode(parsed.preferredRunMode);
    const loadedNotifConfig = normalizeNotificationConfig(parsed.notificationConfig);
if (loadedNotifConfig) {
notificationConfig = loadedNotifConfig;
}
if (parsed.patientContext && typeof parsed.patientContext === "object") {
patientContext = {
name: String(parsed.patientContext.name || "").slice(0, 50),
address: String(parsed.patientContext.address || "").slice(0, 20),
conditions: String(parsed.patientContext.conditions || "").slice(0, 500),
notes: String(parsed.patientContext.notes || "").slice(0, 500),
};
}
    const loadedReminders = Array.isArray(parsed.reminders)
      ? parsed.reminders.map(normalizeReminder).filter(Boolean).slice(-maxStoredReminders)
      : [];
    const loadedCommands = Array.isArray(parsed.pendingCommands)
      ? parsed.pendingCommands.filter(command => command && typeof command === "object").slice(-maxPendingCommands)
      : [];
    activeTimers.splice(0, activeTimers.length, ...loadedReminders);
    pendingCommands.splice(0, pendingCommands.length, ...loadedCommands);
    for (const timer of activeTimers) {
      scheduleReminder(timer);
    }
  } catch (error) {
    console.warn(`[store] failed to load ${storePath}: ${error.message}`);
  }
}

function saveStore() {
  try {
    ensureDataDir();
    const payload = {
      savedAt: new Date().toISOString(),
      latestSnapshot,
      alerts,
      latestSpeech,
      speechRecords,
      voicePrompts,
      preferredRunMode,
      reminders: activeTimers,
      pendingCommands,
      notificationConfig,
      patientContext,
    };
    fs.writeFileSync(storePath, JSON.stringify(payload, null, 2), "utf8");
  } catch (error) {
    console.warn(`[store] failed to save ${storePath}: ${error.message}`);
  }
}

app.use(express.json());

app.get("/login", (req, res) => {
  res.redirect("/login.html");
});

app.get("/login.html", (req, res) => {
  if (getDashboardSession(req)) {
    return res.redirect("/");
  }
  return res.sendFile(path.join(__dirname, "public", "login.html"));
});

app.get("/login.css", (req, res) => {
  res.sendFile(path.join(__dirname, "public", "login.css"));
});

app.post("/api/login", (req, res) => {
  const username = String(req.body?.username || "");
  const password = String(req.body?.password || "");
  if (!safeStringEqual(username, dashboardUser) || !safeStringEqual(password, dashboardPassword)) {
    return res.status(401).json({ ok: false, message: "账号或密码错误" });
  }

  const token = createDashboardSession();
  setDashboardSessionCookie(res, token);
  return res.json({ ok: true });
});

app.post("/api/logout", (req, res) => {
  const token = getDashboardSession(req);
  if (token) {
    dashboardSessions.delete(token);
  }
  clearDashboardSessionCookie(res);
  return res.json({ ok: true });
});

app.get("/api/session", (req, res) => {
  res.json({ ok: true, authenticated: Boolean(getDashboardSession(req)) });
});

app.get("/api/voice-mode", requireDashboardAuth, (req, res) => {
  const effectiveVoiceMode = getEffectiveVoiceMode();
  res.json({
    ok: true,
    mode: effectiveVoiceMode,
    label: voiceModeLabel(effectiveVoiceMode),
    selectedMode: currentVoiceMode,
    selectedLabel: voiceModeLabel(currentVoiceMode),
    deviceOffline: effectiveVoiceMode === "OFFLINE",
    offlineReply: offlineVoiceReply,
  });
});

app.post("/api/voice-mode", requireDashboardAuth, (req, res) => {
  currentVoiceMode = normalizeManualVoiceMode(req.body?.mode);
  const effectiveVoiceMode = getEffectiveVoiceMode();
  res.json({
    ok: true,
    mode: effectiveVoiceMode,
    label: voiceModeLabel(effectiveVoiceMode),
    selectedMode: currentVoiceMode,
    selectedLabel: voiceModeLabel(currentVoiceMode),
    deviceOffline: effectiveVoiceMode === "OFFLINE",
    offlineReply: offlineVoiceReply,
  });
});


// ---- 运行模式（DEMO/REAL） ----
app.get("/api/run-mode", requireDashboardAuth, (req, res) => {
  const deviceMode = latestSnapshot?.run_mode
    ? String(latestSnapshot.run_mode).trim().toUpperCase()
    : null;
  res.json({
    ok: true,
    deviceRunMode: deviceMode || null,
    preferredRunMode,
    effectiveRunMode: preferredRunMode || deviceMode || "DEMO",
  });
});

app.post("/api/run-mode", requireDashboardAuth, (req, res) => {
  const newMode = normalizeRunMode(req.body?.mode);
  const modeChanged = newMode !== preferredRunMode;
  preferredRunMode = newMode;
  saveStore();

  /* 模式变化时，向设备下发 set_run_mode 命令 */
  if (modeChanged && preferredRunMode) {
    const alreadyQueued = pendingCommands.some(
      cmd => cmd.type === 'set_run_mode'
    );
    if (!alreadyQueued) {
      pendingCommands.push({
        type: 'set_run_mode',
        mode: preferredRunMode,
        created_at: new Date().toISOString(),
      });
      if (pendingCommands.length > maxPendingCommands) {
        pendingCommands.splice(0, pendingCommands.length - maxPendingCommands);
      }
      saveStore();
      console.log(`[run-mode] queued set_run_mode command: ${preferredRunMode}`);
    }
  }

  const deviceMode = latestSnapshot?.run_mode
    ? String(latestSnapshot.run_mode).trim().toUpperCase()
    : null;
  res.json({
    ok: true,
    deviceRunMode: deviceMode || null,
    preferredRunMode,
    effectiveRunMode: preferredRunMode || deviceMode || "DEMO",
  });
});

app.get(voicePromptsPath, requireDashboardAuth, (req, res) => {
  res.json({
    ok: true,
    items: voicePrompts,
      preferredRunMode,
  });
});

app.post(voicePromptsPath, requireDashboardAuth, (req, res) => {
  const inputItems = Array.isArray(req.body?.items) ? req.body.items : null;
  if (!inputItems || inputItems.length === 0) {
    return res.status(400).json({ ok: false, message: "items is required" });
  }

  const currentMap = new Map(voicePrompts.map((item) => [item.event_key, item]));
  const nextItems = [...voicePrompts];
  for (const inputItem of inputItems) {
    const currentItem = currentMap.get(String(inputItem?.event_key || "").trim());
    if (!currentItem) {
      return res.status(400).json({
        ok: false,
        message: `unknown event_key: ${String(inputItem?.event_key || "")}`,
      });
    }

    const merged = normalizeVoicePromptItem(
      {
        ...currentItem,
        ...inputItem,
        updated_at: new Date().toISOString(),
      },
      currentItem,
    );
    if (!merged?.tts_text) {
      return res.status(400).json({
        ok: false,
        message: `tts_text is required for ${currentItem.event_key}`,
      });
    }

    const index = nextItems.findIndex((item) => item.event_key === currentItem.event_key);
    nextItems[index] = merged;
  }

  voicePrompts = nextItems;
  saveStore();
  return res.json({
    ok: true,
    items: voicePrompts,
      preferredRunMode,
  });
});

app.get("/", requireDashboardAuth, (req, res) => {
  res.sendFile(path.join(__dirname, "public", "index.html"));
});

app.get("/index.html", requireDashboardAuth, (req, res) => {
  res.sendFile(path.join(__dirname, "public", "index.html"));
});

app.get("/app.js", requireDashboardAuth, (req, res) => {
  res.sendFile(path.join(__dirname, "public", "app.js"));
});

app.get("/styles.css", requireDashboardAuth, (req, res) => {
  res.sendFile(path.join(__dirname, "public", "styles.css"));
});

app.use(express.static(path.join(__dirname, "public"), { index: false }));

// ---- 通知配置 ----
app.get("/api/notification/config", requireDashboardAuth, (req, res) => {
  res.json({
    ok: true,
    config: {
      enabled: notificationConfig.enabled,
      recipients: notificationConfig.recipients.join(", "),
      threshold: notificationConfig.threshold,
      subjectTemplate: notificationConfig.subjectTemplate,
      bodyTemplate: notificationConfig.bodyTemplate,
      defaultRecipients: sosEmailConfig.recipients,
      smtpUser: sosEmailConfig.user || "",
    },
  });
});

app.post("/api/notification/config", requireDashboardAuth, (req, res) => {
  const normalized = normalizeNotificationConfig({
    enabled: req.body?.enabled,
    recipients: req.body?.recipients,
    threshold: req.body?.threshold,
    subjectTemplate: req.body?.subjectTemplate,
    bodyTemplate: req.body?.bodyTemplate,
  });
  if (!normalized) {
    return res.status(400).json({ ok: false, message: "invalid config" });
  }
  notificationConfig = normalized;
  saveStore();
  console.log(`[notification] config updated recipients=${normalized.recipients.length} threshold=${normalized.threshold}`);
  res.json({
    ok: true,
    config: {
      enabled: notificationConfig.enabled,
      recipients: notificationConfig.recipients.join(", "),
      threshold: notificationConfig.threshold,
      subjectTemplate: notificationConfig.subjectTemplate,
      bodyTemplate: notificationConfig.bodyTemplate,
    },
  });
});

// ---- 患者信息 ----
app.get("/api/patient-context", requireDashboardAuth, (req, res) => {
  res.json({ ok: true, context: patientContext });
});

app.post("/api/patient-context", requireDashboardAuth, (req, res) => {
  patientContext = {
    name: String(req.body?.name || "").slice(0, 50),
    address: String(req.body?.address || "").slice(0, 20),
    conditions: String(req.body?.conditions || "").slice(0, 500),
    notes: String(req.body?.notes || "").slice(0, 500),
  };
  saveStore();
  console.log(`[patient-context] updated name="${patientContext.name}" address="${patientContext.address}"`);
  res.json({ ok: true, context: patientContext });
});

app.post("/api/notification/test", requireDashboardAuth, async (req, res) => {
  if (!hasSosEmailConfig()) {
    return res.status(503).json({ ok: false, message: "SMTP 未配置，无法发送测试邮件" });
  }
  const recipients = getEffectiveRecipients();
  if (recipients.length === 0) {
    return res.status(400).json({ ok: false, message: "未配置收件人" });
  }
  const testRecord = {
    device_id: "TEST_EMAIL",
    state: "TEST",
    reason: "这是一封测试邮件，用于验证 SOS 通知邮件功能是否正常工作。",
    received_at: new Date().toISOString(),
  };
  try {
    await sendSosEmail(testRecord);
    console.log(`[notification] test email sent to ${recipients.length} recipients`);
    res.json({ ok: true, message: `测试邮件已发送到 ${recipients.length} 个收件人` });
  } catch (error) {
    console.warn(`[notification] test email failed: ${error.message}`);
    res.status(502).json({ ok: false, message: `测试邮件发送失败：${error.message}` });
  }
});

app.get("/api/status", requireDashboardAuth, (req, res) => {
  const effectiveVoiceMode = getEffectiveVoiceMode();
  res.json({
    ok: true,
    service: "elder-alert-dashboard",
    alertPath,
    latestPath,
    speechTranscribePath,
    speechLatestPath,
    speechReplyAudioPath,
    voicePromptsPath,
    voicePromptAudioPath,
    speechAsrConfigured: hasTencentAsrConfig(),
    speechAsrEngine: tencentAsrConfig.engine || "16k_zh",
    aiReplyConfigured: hasAiReplyConfig(),
    aiReplyModel: aiReplyConfig.model || null,
    ttsConfigured: hasTtsConfig(),
    ttsModel: ttsConfig.model || null,
    ttsFormat: ttsConfig.format || null,
    voiceMode: effectiveVoiceMode,
    voiceModeLabel: voiceModeLabel(effectiveVoiceMode),
    selectedVoiceMode: currentVoiceMode,
    selectedVoiceModeLabel: voiceModeLabel(currentVoiceMode),
    deviceOffline: effectiveVoiceMode === "OFFLINE",
    offlineVoiceReply,
    totalAlerts: alerts.length,
    totalSpeechRecords: speechRecords.length,
    lastReceivedAt: latestSnapshot ? latestSnapshot.received_at : null,
    lastSpeechAt: latestSpeech ? latestSpeech.received_at : null,
    deviceRunMode: latestSnapshot?.run_mode
      ? String(latestSnapshot.run_mode).trim().toUpperCase()
      : null,
    preferredRunMode,
    effectiveRunMode: preferredRunMode || (latestSnapshot?.run_mode
      ? String(latestSnapshot.run_mode).trim().toUpperCase()
      : "DEMO"),
  });
});

app.get(latestPath, requireDashboardAuth, (req, res) => {
  res.json({
    ok: true,
    latest: latestSnapshot,
  });
});

app.get("/api/alerts", requireDashboardAuth, (req, res) => {
  res.json({
    ok: true,
    count: alerts.length,
    alerts,
  });
});

app.get(speechLatestPath, requireDashboardAuth, (req, res) => {
  res.json({
    ok: true,
    latest: latestSpeech,
    records: speechRecords,
  });
});

app.get(voicePromptAudioPath, requireDashboardOrDeviceToken, async (req, res) => {
  try {
    const eventKey = String(req.query?.event_key || "").trim();
    if (!eventKey) {
      return res.status(400).json({ ok: false, message: "event_key is required" });
    }

    const promptItem = getVoicePromptByEventKey(eventKey);
    if (!promptItem) {
      return res.status(404).json({ ok: false, message: "prompt_not_found" });
    }
    if (!promptItem.enabled) {
      return res.status(409).json({ ok: false, message: "prompt_disabled" });
    }
    if (!promptItem.tts_text) {
      return res.status(409).json({ ok: false, message: "prompt_text_empty" });
    }

    const audioBuffer = await generateReplyAudio(promptItem.tts_text);
    res.set({
      "Content-Type": mimeTypeForAudioFormat(ttsConfig.format),
      "Content-Length": audioBuffer.length,
      "Cache-Control": "no-store",
      "X-TTS-Model": ttsConfig.model,
      "X-TTS-Format": ttsConfig.format,
      "X-Voice-Prompt-Key": promptItem.event_key,
    });
    return res.status(200).send(audioBuffer);
  } catch (error) {
    console.warn(`[voice-prompt-tts] failed: ${describeError(error)}`);
    return res.status(error.statusCode || 500).json({
      ok: false,
      message: describeError(error),
    });
  }
});

app.get(speechReplyAudioPath, requireDashboardOrDeviceToken, async (req, res) => {
  try {
    const text = latestSpeech?.ai_reply;
    if (!text) {
      return res.status(404).json({
        ok: false,
        message: "latest AI reply is not available",
      });
    }

    /* 优先返回 transcribe 端点预缓存的音频，避免重复 TTS 调用 */
    let audioBuffer;
    if (cachedReplyAudio && cachedReplyAudio.length > 0) {
      audioBuffer = cachedReplyAudio;
      console.log(`[speech-tts] serving cached reply audio bytes=${audioBuffer.length}`);
    } else {
      audioBuffer = await generateReplyAudio(text);
      console.log(`[speech-tts] generated reply audio on-demand bytes=${audioBuffer.length}`);
    }

    res.set({
      "Content-Type": mimeTypeForAudioFormat(ttsConfig.format),
      "Content-Length": audioBuffer.length,
      "Cache-Control": "no-store",
      "X-TTS-Model": ttsConfig.model,
      "X-TTS-Format": ttsConfig.format,
    });
    return res.status(200).send(audioBuffer);
  } catch (error) {
    console.warn(`[speech-tts] failed: ${describeError(error)}`);
    return res.status(error.statusCode || 500).json({
      ok: false,
      message: describeError(error),
    });
  }
});

app.get("/api/agent/notification-audio", requireDashboardOrDeviceToken, async (req, res) => {
  try {
    const text = String(req.query?.text || "").trim();
    if (!text) return res.status(400).json({ ok: false, message: "text is required" });

    const audioBuffer = await generateReplyAudio(text);
    res.set({
      "Content-Type": mimeTypeForAudioFormat(ttsConfig.format),
      "Content-Length": audioBuffer.length,
      "Cache-Control": "no-store",
    });
    return res.status(200).send(audioBuffer);
  } catch (error) {
    console.warn(`[agent-tts] failed: ${describeError(error)}`);
    return res.status(error.statusCode || 500).json({ ok: false, message: describeError(error) });
  }
});

app.get("/api/agent/status", requireDashboardAuth, (req, res) => {
res.json({
ok: true,
activeTimers: getActiveTimers().map(t => ({
...t,
type: "定时提醒",
description: t.message,
expiresAt: new Date(t.fire_at).getTime()
})),
recentActions: recentAgentActions,
availableTools: agentToolMeta,
currentVoiceMode,
patientContext,
});
});

app.get("/api/agent/tools", requireDashboardAuth, (req, res) => {
res.json({
ok: true,
tools: agentToolMeta,
count: agentToolMeta.length,
});
});

app.post(alertPath, requireDeviceToken, (req, res) => {
  const validationError = validateAlertPayload(req.body);
  if (validationError) {
    return res.status(400).json({
      ok: false,
      message: validationError,
    });
  }

  const record = {
    ...req.body,
    received_at: new Date().toISOString(),
  };
  const reportMode = req.get("X-Report-Mode") === "event" ? "event" : "telemetry";

  latestSnapshot = record;

  if (reportMode === "event" && record.state !== "NORMAL") {
    alerts.unshift(record);
    if (alerts.length > maxAlerts) {
      alerts.length = maxAlerts;
    }
  }
  saveStore();

  console.log(
    `[telemetry] mode=${reportMode} device=${record.device_id} state=${record.state} risk=${record.risk_level} reason=${record.reason}`
  );
  scheduleSosEmail(record, reportMode);

  const pendingCmds = takePendingCommands();
  if (pendingCmds.length > 0) {
    saveStore();
  }
  for (const cmd of pendingCmds) {
    if (cmd.type === 'play_tts' && !cmd.url && cmd.tts_text) {
      cmd.url = `/api/agent/notification-audio?text=${encodeURIComponent(cmd.tts_text)}`;
    }
  }

  return res.status(200).json({
    ok: true,
    message: "telemetry received",
    count: alerts.length,
    commands: pendingCmds.length > 0 ? pendingCmds : undefined,
  });
});

app.post(
  speechTranscribePath,
  requireDeviceToken,
  express.raw({
    type: ["audio/*", "application/octet-stream"],
    limit: maxSpeechAudioBytes,
  }),
  async (req, res) => {
    try {
      if (!Buffer.isBuffer(req.body) || req.body.length === 0) {
        return res.status(400).json({
          ok: false,
          message: "audio body is required",
        });
      }

      const voiceFormat = normalizeVoiceFormat(req.get("X-Audio-Format") || req.query.format);
      const asrResponse = await transcribeWithTencentAsr(req.body, voiceFormat);
      const transcript = asrResponse.Result || "";
      const effectiveVoiceMode = getEffectiveVoiceMode();
      const record = {
        result: transcript,
        audio_duration_ms: asrResponse.AudioDuration ?? null,
        word_size: asrResponse.WordSize ?? null,
        voice_format: voiceFormat,
        engine: tencentAsrConfig.engine || "16k_zh",
        voice_mode: effectiveVoiceMode,
        voice_mode_label: voiceModeLabel(effectiveVoiceMode),
        ai_reply: null,
        ai_model: effectiveVoiceMode === "OFFLINE" ? null : hasAiReplyConfig() ? aiReplyConfig.model : null,
        ai_error: null,
        bytes: req.body.length,
        request_id: asrResponse.RequestId || null,
        received_at: new Date().toISOString(),
      };

      latestSpeech = record;
      speechRecords.unshift(record);
      if (speechRecords.length > maxSpeechRecords) {
        speechRecords.length = maxSpeechRecords;
      }
      saveStore();

      /* 立即返回 ASR 结果给设备，AI + TTS 在后台异步执行 */
      cachedReplyAudio = null;
      cachedReplyAudioReady = false;
      console.log(
        `[speech] format=${record.voice_format} bytes=${record.bytes} duration_ms=${record.audio_duration_ms} result=${record.result} reply_ready=false (async)`
      );

      /* 异步执行 AI 回复 + TTS 生成，不阻塞 HTTP 响应 */
      (async () => {
        let replyText = null;
        try {
          if (!String(transcript || "").trim()) {
            replyText = "我没有听清楚，请再按一次按钮说一遍。";
            console.log("[speech-ai] transcript empty, using fallback reply");
          } else {
            replyText = await generateAiReply(transcript, effectiveVoiceMode);
          }

          if (replyText) {
            record.ai_reply = replyText;
            record.ai_error = null;
            saveStore();
            console.log(`[speech-ai] reply generated (async) mode=${effectiveVoiceMode} model=${record.ai_model || "fallback"} reply=${replyText || ""}`);

            if (hasTtsConfig()) {
              try {
                cachedReplyAudio = await generateReplyAudio(replyText);
                cachedReplyAudioReady = true;
                console.log(`[speech-tts] pre-generated reply audio (async) bytes=${cachedReplyAudio ? cachedReplyAudio.length : 0}`);
              } catch (ttsError) {
                cachedReplyAudioReady = true; /* 标记完成，允许 on-demand 生成 */
                console.warn(`[speech-tts] pre-generation failed (async): ${describeError(ttsError)}, will generate on-demand`);
              }
            } else {
              cachedReplyAudioReady = true;
            }
          }
        } catch (aiError) {
          record.ai_error = aiError.message;
          replyText = "我暂时无法回复，请稍后再试。";
          record.ai_reply = replyText;
          saveStore();
          console.warn(`[speech-ai] reply failed (async): ${aiError.message}`);

          if (hasTtsConfig()) {
            try {
              cachedReplyAudio = await generateReplyAudio(replyText);
              cachedReplyAudioReady = true;
            } catch (ttsError) {
              cachedReplyAudioReady = true;
              console.warn(`[speech-tts] fallback pre-generation failed (async): ${describeError(ttsError)}`);
            }
          } else {
            cachedReplyAudioReady = true;
          }
        }
      })().catch(err => console.warn(`[speech-ai] async pipeline error: ${err.message}`));

      return res.status(200).json({
        ok: true,
        speech: record,
        reply_ready: false,
      });
    } catch (error) {
      console.warn(`[speech] transcribe failed: ${error.message}`);
      return res.status(error.statusCode || 500).json({
        ok: false,
        message: error.message,
        request_id: error.tencentRequestId || null,
      });
    }
  }
);

loadStore();

app.listen(port, () => {
  console.log(`Local dashboard running at http://localhost:${port}`);
  console.log(`POST alerts to http://localhost:${port}${alertPath}`);
  console.log(`POST speech audio to http://localhost:${port}${speechTranscribePath}`);
  console.log(`GET voice prompts at http://localhost:${port}${voicePromptsPath}`);
  console.log(`[store] using ${storePath}`);
  console.log(`[auth] device token ${deviceToken ? "enabled" : "disabled"}`);
  console.log(`[speech] Tencent ASR ${hasTencentAsrConfig() ? "configured" : "not configured"}`);
  console.log(`[speech] inline AI wait ${Number.isFinite(speechTranscribeInlineAiWaitMs) ? speechTranscribeInlineAiWaitMs : 1000}ms`);
  console.log(`[speech-ai] reply model ${hasAiReplyConfig() ? aiReplyConfig.model : "not configured"}`);
  console.log(`[speech-tts] model ${hasTtsConfig() ? ttsConfig.model : "not configured"}`);
  console.log(`[sos-email] SMTP ${hasSosEmailConfig() ? `configured recipients=${sosEmailConfig.recipients.length}` : "not configured"}`);
  console.log(`[reminders] active=${getActiveTimers().length} pending_commands=${pendingCommands.length}`);
});
