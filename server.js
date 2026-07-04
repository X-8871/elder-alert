const express = require("express");
const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

const app = express();
app.disable("x-powered-by");
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
const maxSpeechAudioBytes = 600 * 1024;
const dashboardUser = (process.env.ELDER_DASHBOARD_USER || "").trim();
const dashboardPassword = process.env.ELDER_DASHBOARD_PASSWORD || "";
const dashboardCookieSecure = !["0", "false", "no"].includes(
  String(process.env.ELDER_DASHBOARD_COOKIE_SECURE || "true").trim().toLowerCase()
);
if (!dashboardUser || !dashboardPassword) {
  throw new Error("缺少 ELDER_DASHBOARD_USER 或 ELDER_DASHBOARD_PASSWORD 环境变量");
}
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
  timeoutMs: Number(process.env.ELDER_AI_TIMEOUT_MS || 12000),
};
const ttsConfig = {
  provider: (process.env.ELDER_TTS_PROVIDER || "openai-compatible").trim().toLowerCase(),
  apiKey: (process.env.ELDER_TTS_API_KEY || process.env.ELDER_AI_API_KEY || "").trim(),
  baseUrl: (process.env.ELDER_TTS_BASE_URL || process.env.ELDER_AI_BASE_URL || "").trim().replace(/\/+$/, ""),
  model: (process.env.ELDER_TTS_MODEL || "").trim(),
  voice: (process.env.ELDER_TTS_VOICE || "mimo_default").trim(),
  format: (process.env.ELDER_TTS_FORMAT || "wav").trim().toLowerCase(),
  sampleRate: Number(process.env.ELDER_TTS_SAMPLE_RATE || 16000),
  timeoutMs: Number(process.env.ELDER_TTS_TIMEOUT_MS || 20000),
};
const tencentAsrConfig = {
  secretId: (process.env.TENCENTCLOUD_SECRET_ID || "").trim(),
  secretKey: (process.env.TENCENTCLOUD_SECRET_KEY || "").trim(),
  appId: (process.env.TENCENTCLOUD_ASR_APPID || "").trim(),
  engine: (process.env.TENCENTCLOUD_ASR_ENGINE || "16k_zh").trim(),
  region: (process.env.TENCENTCLOUD_ASR_REGION || "ap-shanghai").trim(),
};
const dataDir = path.join(__dirname, "data");
const storePath = path.join(dataDir, "alerts-store.json");
const agentConfigPath = path.join(dataDir, "agent-config.json");

const alerts = [];
const speechRecords = [];
let latestSnapshot = null;
let latestSpeech = null;
let voicePrompts = [];
let preferredRunMode = null; // null = 跟随设备默认；"DEMO" | "REAL"

let agentConfig = { enabledTools: [] }; // Agent 工具配置
// ---- Agent 工具与定时器管理 ----
const agentTools = [
  {
    type: "function",
    function: {
      name: "set_reminder",
      description: "为老人设置一个定时提醒。老人说'提醒我吃药'、'30分钟后叫我'、'帮我定个闹钟'时调用。",
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
  {
    type: "function",
    function: {
      name: "get_environment",
      description: "查看当前家中的环境传感器数据。老人问'现在温度多少'、'家里空气好不好'时调用。",
      parameters: { type: "object", properties: {} }
    }
  },
  {
    type: "function",
    function: {
      name: "confirm_alert",
      description: "帮老人确认/关闭当前的提醒报警。老人说'关掉报警'、'别响了'、'我没事'时调用。仅能关闭REMIND级别提醒。",
      parameters: { type: "object", properties: {} }
    }
  },
  {
    type: "function",
    function: {
      name: "get_temperature",
      description: "查询当前室内温度和湿度",
      parameters: { type: "object", properties: {} }
    }
  },
  {
    type: "function",
    function: {
      name: "get_current_time",
      description: "查询当前时间",
      parameters: { type: "object", properties: {} }
    }
  },
  {
    type: "function",
    function: {
      name: "get_light_level",
      description: "查询光照强度，判断白天还是晚上",
      parameters: { type: "object", properties: {} }
    }
  },
  {
    type: "function",
    function: {
      name: "get_air_quality",
      description: "查询空气质量状态（MQ2传感器）",
      parameters: { type: "object", properties: {} }
    }
  },
  {
    type: "function",
    function: {
      name: "show_message_on_screen",
      description: "在TFT屏幕上显示提醒消息",
      parameters: {
        type: "object",
        properties: {
          message: { type: "string", description: "要显示的消息" }
        },
        required: ["message"]
      }
    }
  },
  {
    type: "function",
    function: {
      name: "beep_once",
      description: "蜂鸣器响一次作为提醒",
      parameters: { type: "object", properties: {} }
    }
  },
  {
    type: "function",
    function: {
      name: "set_timer_reminder",
      description: "设置定时语音播报提醒",
      parameters: {
        type: "object",
        properties: {
          minutes: { type: "number", description: "几分钟后提醒" },
          message: { type: "string", description: "提醒内容" }
        },
        required: ["minutes", "message"]
      }
    }
  },
  {
    type: "function",
    function: {
      name: "list_active_reminders",
      description: "列出当前所有活跃的定时提醒",
      parameters: { type: "object", properties: {} }
    }
  },
  {
    type: "function",
    function: {
      name: "cancel_reminder",
      description: "取消指定的定时提醒",
      parameters: {
        type: "object",
        properties: {
          id: { type: "string", description: "提醒ID" }
        },
        required: ["id"]
      }
    }
  },
  {
    type: "function",
    function: {
      name: "get_recent_alerts",
      description: "查询最近的异常事件记录",
      parameters: { type: "object", properties: {} }
    }
  },
  {
    type: "function",
    function: {
      name: "get_motion_summary",
      description: "获取毫米波活动摘要（有人/无人/活动/静止）",
      parameters: { type: "object", properties: {} }
    }
  },
];

const activeTimers = [];
const pendingCommands = [];
let recentAgentActions = [];

function addReminder(minutes, message) {
  const id = `timer_${Date.now()}`;
  const fire_at = new Date(Date.now() + minutes * 60000).toISOString();
  const tts_text = `提醒您，${message}。`;
  const timer = { id, minutes, message, fire_at, tts_text, fired: false };
  activeTimers.push(timer);
  
  setTimeout(() => {
    const t = activeTimers.find(x => x.id === id);
    if (t && !t.fired) {
      t.fired = true;
      pendingCommands.push({
        type: 'play_tts',
        tts_text: t.tts_text,
        reason: `定时提醒: ${message}`,
        created_at: new Date().toISOString()
      });
      saveStore();
    }
  }, minutes * 60000);
  
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

  switch (name) {
    case 'set_reminder': {
      const timer = addReminder(args.minutes, args.message);
      return { 
        success: true, 
        message: `已设置${args.minutes}分钟后提醒: ${args.message}`,
        fire_at: timer.fire_at 
      };
    }
    case 'get_environment': {
      const snap = latestSnapshot || {};
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
    case 'confirm_alert': {
      const snap = latestSnapshot || {};
      if (snap.state !== 'REMIND') {
        return { success: false, reason: '语音只能关闭REMIND级别的提醒。当前状态不允许语音关闭，请按设备上的确认按钮。' };
      }
      pendingCommands.push({
        type: 'confirm_alert',
        created_at: new Date().toISOString()
      });
      return { success: true, message: '已发送确认指令，提醒将在几秒内关闭。' };
    }
    case 'get_temperature': {
      const snap = latestSnapshot || {};
      return {
        temperature: snap.temperature ?? '未知',
        humidity: snap.humidity ?? '未知',
        message: `当前温度 ${snap.temperature ?? '未知'}°C，湿度 ${snap.humidity ?? '未知'}%`
      };
    }
    case 'get_current_time': {
      const now = new Date().toLocaleString('zh-CN', { timeZone: 'Asia/Shanghai' });
      return { current_time: now, message: `现在是${now}` };
    }
    case 'get_light_level': {
      const snap = latestSnapshot || {};
      const lux = snap.lux ?? 0;
      const isDaytime = lux > 50;
      return {
        lux: lux,
        is_daytime: isDaytime,
        message: `当前光照 ${lux} lux，${isDaytime ? '白天' : '晚上'}`
      };
    }
    case 'get_air_quality': {
      const snap = latestSnapshot || {};
      const mq2 = snap.mq2_raw ?? 0;
      const quality = mq2 > 1200 ? '异常' : '正常';
      return {
        mq2_raw: mq2,
        quality: quality,
        message: `空气质量${quality}，MQ2读数 ${mq2}`
      };
    }
    case 'show_message_on_screen': {
      pendingCommands.push({
        type: 'show_screen_message',
        message: args.message || '',
        created_at: new Date().toISOString()
      });
      return { success: true, message: `已发送屏幕消息: ${args.message}` };
    }
    case 'beep_once': {
      pendingCommands.push({
        type: 'beep_once',
        created_at: new Date().toISOString()
      });
      return { success: true, message: '已发送蜂鸣指令' };
    }
    case 'set_timer_reminder': {
      const timer = addReminder(args.minutes, args.message);
      return {
        success: true,
        message: `已设置${args.minutes}分钟后提醒: ${args.message}`,
        fire_at: timer.fire_at
      };
    }
    case 'list_active_reminders': {
      const timers = getActiveTimers();
      return {
        count: timers.length,
        reminders: timers.map(t => ({
          id: t.id,
          message: t.message,
          fire_at: t.fire_at,
          remaining_minutes: Math.ceil((new Date(t.fire_at) - Date.now()) / 60000)
        })),
        message: timers.length > 0 ? `当前有${timers.length}个活跃提醒` : '当前没有活跃提醒'
      };
    }
    case 'cancel_reminder': {
      const index = activeTimers.findIndex(t => t.id === args.id);
      if (index === -1) {
        return { success: false, message: '未找到该提醒' };
      }
      activeTimers.splice(index, 1);
      return { success: true, message: '提醒已取消' };
    }
    case 'get_recent_alerts': {
      const recentAlerts = alerts.slice(0, 5);
      return {
        count: recentAlerts.length,
        alerts: recentAlerts.map(a => ({
          state: a.state,
          risk_level: a.risk_level,
          reason: a.reason,
          received_at: a.received_at
        })),
        message: recentAlerts.length > 0 ? `最近${recentAlerts.length}条异常事件` : '暂无异常事件'
      };
    }
    case 'get_motion_summary': {
      const snap = latestSnapshot || {};
      const presence = snap.ld2410b_presence ?? '未知';
      const moving = snap.ld2410b_moving_distance ?? 0;
      const stationary = snap.ld2410b_stationary_distance ?? 0;
      let status = '未知';
      if (presence === 'none') status = '无人';
      else if (moving > 0) status = '有活动';
      else if (stationary > 0) status = '静止';
      return {
        presence: presence,
        moving_distance: moving,
        stationary_distance: stationary,
        status: status,
        message: `毫米波检测：${status}`
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
  const secureAttribute = dashboardCookieSecure ? "; Secure" : "";
  res.setHeader(
    "Set-Cookie",
    `elder_alert_session=${encodeURIComponent(token)}; HttpOnly; SameSite=Lax; Path=/; Max-Age=43200${secureAttribute}`
  );
}

function clearDashboardSessionCookie(res) {
  const secureAttribute = dashboardCookieSecure ? "; Secure" : "";
  res.setHeader(
    "Set-Cookie",
    `elder_alert_session=; HttpOnly; SameSite=Lax; Path=/; Max-Age=0${secureAttribute}`
  );
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
  if (normalizedMode === "CARE" && /天气|新闻|外面|路线|导航|电话|微信|支付/.test(text)) {
    return "这个功能我暂时不会。";
  }
  if (/听到|听见|听清|能听/.test(text)) {
    return "听见了，您可以继续说。";
  }
  if (/干嘛|在吗|你好|喂/.test(text)) {
    return "我在帮您留意家里安全。";
  }
  if (normalizedMode === "INTERACT") {
    return "嗯，我在听，您可以继续说。";
  }
  return "这个我暂时不会，请换个问题。";
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

  if (normalizedMode === "INTERACT") {
    return (
      "你是独居老人家里的自然对话语音助手。回答要像正常聊天，简短、亲切、有上下文感，最多60个汉字。" +
      "可以陪聊、问候、解释简单问题，也可以结合设备状态做温和提醒。" +
      "你可以使用工具定闹钟、查数据、关提醒。危险求救请提醒按SOS键。" +
      "医疗、用药、诊断问题要建议咨询医生或家属。" + timerInfo
    );
  }

  return (
    "你是独居老人家里的安全看护语音助手。直接回答老人问题，最多40个汉字。" +
    "优先关注安全、风险解释。可以使用工具定闹钟、查数据、关提醒。" +
    "危险求救请提醒按SOS键。" +
    "不要反复说我听到了，不要复述用户原话，不要列设备状态，不要输出分析过程。" + timerInfo
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

function extractMinimaxTtsAudio(data) {
  const providerStatus = Number(data?.base_resp?.status_code || 0);
  if (providerStatus !== 0) {
    const error = new Error(`MiniMax TTS API failed with status ${providerStatus}`);
    error.statusCode = 502;
    throw error;
  }

  const audioHex = String(data?.data?.audio || "").trim();
  if (!audioHex || audioHex.length % 2 !== 0 || !/^[0-9a-f]+$/i.test(audioHex)) {
    return null;
  }
  return Buffer.from(audioHex, "hex");
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

function sendPublicTtsError(res, error, logPrefix) {
  console.warn(`[${logPrefix}] failed: ${describeError(error)}`);
  const isInvalidText = error?.statusCode === 400;
  const isNotConfigured = error?.statusCode === 503;
  const statusCode = isInvalidText ? 400 : isNotConfigured ? 503 : 502;
  const code = isInvalidText ? "tts_text_invalid" : isNotConfigured ? "tts_not_configured" : "tts_unavailable";
  const message = isInvalidText
    ? "语音文本无效"
    : isNotConfigured
      ? "语音生成服务未配置"
      : "语音生成服务暂不可用，请稍后重试";

  res.set("Cache-Control", "no-store");
  return res.status(statusCode).json({ ok: false, code, message });
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
    for (let round = 0; round < 3; round++) {
      const payload = {
        model: aiReplyConfig.model,
        messages,
        temperature: 0.5,
        max_tokens: 150,
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
  const isMinimax = ttsConfig.provider === "minimax";
  const endpoint = isMinimax
    ? `${ttsConfig.baseUrl}/t2a_v2`
    : `${ttsConfig.baseUrl}/chat/completions`;
  const payload = isMinimax
    ? {
        model: ttsConfig.model,
        text: cleanText,
        stream: false,
        language_boost: "auto",
        output_format: "hex",
        voice_setting: {
          voice_id: ttsConfig.voice,
          speed: 1,
          vol: 1,
          pitch: 0,
        },
        audio_setting: {
          sample_rate: Number.isFinite(ttsConfig.sampleRate) ? ttsConfig.sampleRate : 16000,
          bitrate: 128000,
          format: ttsConfig.format,
          channel: 1,
        },
      }
    : {
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

  try {
    const response = await fetch(endpoint, {
      method: "POST",
      headers: {
        Authorization: `Bearer ${ttsConfig.apiKey}`,
        "Content-Type": "application/json",
      },
      body: JSON.stringify(payload),
      signal: controller.signal,
    });

    if (!response.ok) {
      const error = new Error(`TTS provider request failed with HTTP ${response.status}`);
      error.statusCode = 502;
      error.upstreamStatusCode = response.status;
      throw error;
    }

    let audioBuffer;
    if (isMinimax) {
      const data = await response.json();
      audioBuffer = extractMinimaxTtsAudio(data);
    } else {
      const contentType = response.headers.get("content-type") || "";
      audioBuffer = Buffer.from(await response.arrayBuffer());
      const responseText = audioBuffer.toString("utf8");
      const looksLikeJson = /^[\s]*[\[{]/.test(responseText);
      if (contentType.includes("application/json") || looksLikeJson) {
        const data = JSON.parse(responseText);
        audioBuffer = extractTtsAudio(data);
      }
    }
    if (!audioBuffer || audioBuffer.length === 0) {
      const error = new Error("TTS audio was empty");
      error.statusCode = 502;
      throw error;
    }
    if (
      ttsConfig.format === "wav" &&
      (audioBuffer.length < 12 || audioBuffer.toString("ascii", 0, 4) !== "RIFF" || audioBuffer.toString("ascii", 8, 12) !== "WAVE")
    ) {
      const error = new Error("TTS response was not a valid WAV file");
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
    };
    fs.writeFileSync(storePath, JSON.stringify(payload, null, 2), "utf8");
  } catch (error) {
    console.warn(`[store] failed to save ${storePath}: ${error.message}`);
  }
}

// ---- Agent 配置加载和保存 ----
function loadAgentConfig() {
  try {
    ensureDataDir();
    if (!fs.existsSync(agentConfigPath)) {
      agentConfig = {
        enabledTools: agentTools.map(t => t.function.name)
      };
      return;
    }
    const raw = fs.readFileSync(agentConfigPath, "utf8");
    const parsed = JSON.parse(raw);
    agentConfig = {
      enabledTools: Array.isArray(parsed.enabledTools) ? parsed.enabledTools : []
    };
    console.log(`[agent-config] loaded ${agentConfig.enabledTools.length} enabled tools`);
  } catch (error) {
    console.warn(`[agent-config] failed to load ${agentConfigPath}: ${error.message}`);
    agentConfig = {
      enabledTools: agentTools.map(t => t.function.name)
    };
  }
}

function saveAgentConfig() {
  try {
    ensureDataDir();
    fs.writeFileSync(agentConfigPath, JSON.stringify(agentConfig, null, 2), "utf8");
    console.log(`[agent-config] saved ${agentConfig.enabledTools.length} enabled tools`);
  } catch (error) {
    console.warn(`[agent-config] failed to save ${agentConfigPath}: ${error.message}`);
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
  preferredRunMode = normalizeRunMode(req.body?.mode);
  saveStore();
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

// ---- Agent 配置 API ----
app.get("/api/agent-config", requireDashboardAuth, (req, res) => {
  res.json({
    ok: true,
    enabledTools: agentConfig.enabledTools,
    availableTools: agentTools.map(t => ({
      name: t.function.name,
      description: t.function.description
    }))
  });
});

app.post("/api/agent-config", requireDashboardAuth, (req, res) => {
  const enabledTools = Array.isArray(req.body?.enabledTools) ? req.body.enabledTools : null;
  console.log('[agent-config] POST request body:', JSON.stringify(req.body));
  if (!enabledTools) {
    return res.status(400).json({ ok: false, message: "enabledTools is required" });
  }
  const validToolNames = new Set(agentTools.map(t => t.function.name));
  for (const toolName of enabledTools) {
    if (!validToolNames.has(toolName)) {
      return res.status(400).json({ ok: false, message: `invalid tool name: ${toolName}` });
    }
  }
  agentConfig.enabledTools = enabledTools;
  saveAgentConfig();
  res.json({
    ok: true,
    enabledTools: agentConfig.enabledTools
  });
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
      "X-TTS-Provider": ttsConfig.provider,
      "X-Voice-Prompt-Key": promptItem.event_key,
    });
    return res.status(200).send(audioBuffer);
  } catch (error) {
    return sendPublicTtsError(res, error, "voice-prompt-tts");
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

    const audioBuffer = await generateReplyAudio(text);
    res.set({
      "Content-Type": mimeTypeForAudioFormat(ttsConfig.format),
      "Content-Length": audioBuffer.length,
      "Cache-Control": "no-store",
      "X-TTS-Model": ttsConfig.model,
      "X-TTS-Format": ttsConfig.format,
      "X-TTS-Provider": ttsConfig.provider,
    });
    return res.status(200).send(audioBuffer);
  } catch (error) {
    return sendPublicTtsError(res, error, "speech-tts");
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
      "X-TTS-Provider": ttsConfig.provider,
    });
    return res.status(200).send(audioBuffer);
  } catch (error) {
    return sendPublicTtsError(res, error, "agent-tts");
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
    recentActions: recentAgentActions
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

  const pendingCmds = takePendingCommands();
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
      const aiWaitMs = Number.isFinite(speechTranscribeInlineAiWaitMs)
        ? speechTranscribeInlineAiWaitMs
        : 1000;
      const aiReplyTask = runAiReplyGeneration(record, transcript);
      const replyReady = await waitForBoolean(aiReplyTask, aiWaitMs);

      console.log(
        `[speech] format=${record.voice_format} bytes=${record.bytes} duration_ms=${record.audio_duration_ms} result=${record.result} reply_ready=${replyReady ? "true" : "false"}`
      );

      return res.status(200).json({
        ok: true,
        speech: record,
        reply_ready: Boolean(replyReady),
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
loadAgentConfig();

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
});
