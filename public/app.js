const refs = {
  appFrame: document.querySelector(".app-frame"),
  navItems: Array.from(document.querySelectorAll("[data-view-link]")),
  viewBlocks: Array.from(document.querySelectorAll("[data-view]")),
  lowerGrid: document.getElementById("lower-grid"),
  serviceState: document.getElementById("service-state"),
  serviceCount: document.getElementById("service-count"),
  serviceLastReceived: document.getElementById("service-last-received"),
  serviceAge: document.getElementById("service-age"),
  endpointBox: document.getElementById("endpoint-box"),
  emptyState: document.getElementById("empty-state"),
  alertsList: document.getElementById("alerts-list"),
  alertsPageInfo: document.getElementById("alerts-page-info"),
  alertsPagination: document.getElementById("alerts-pagination"),
  alertsPaginationInfo: document.getElementById("alerts-pagination-info"),
  alertsPrevButton: document.getElementById("alerts-prev-button"),
  alertsNextButton: document.getElementById("alerts-next-button"),
  alertsStateFilter: document.getElementById("alerts-state-filter"),
  refreshButton: document.getElementById("refresh-button"),
  logoutButton: document.getElementById("logout-button"),
  snapshotEmpty: document.getElementById("snapshot-empty"),
  snapshotGrid: document.getElementById("snapshot-grid"),
  metricState: document.getElementById("metric-state"),
  metricTemperature: document.getElementById("metric-temperature"),
  metricTemperatureBar: document.getElementById("metric-temperature-bar"),
  metricTemperatureThreshold: document.getElementById("metric-temperature-threshold"),
  metricHumidity: document.getElementById("metric-humidity"),
  metricHumidityBar: document.getElementById("metric-humidity-bar"),
  metricHumidityThreshold: document.getElementById("metric-humidity-threshold"),
  metricLux: document.getElementById("metric-lux"),
  metricLuxBar: document.getElementById("metric-lux-bar"),
  metricLuxThreshold: document.getElementById("metric-lux-threshold"),
  metricMq2: document.getElementById("metric-mq2"),
  metricMq2Bar: document.getElementById("metric-mq2-bar"),
  metricMq2Threshold: document.getElementById("metric-mq2-threshold"),
  metricLightScene: document.getElementById("metric-light-scene"),
  metricLightSceneNote: document.getElementById("metric-light-scene-note"),
  metricMmwave: document.getElementById("metric-mmwave"),
  metricMmwaveDistance: document.getElementById("metric-mmwave-distance"),
  miniChartNote: document.getElementById("mini-chart-note"),
  chartTemp: document.getElementById("chart-temp"),
  chartTempValue: document.getElementById("chart-temp-value"),
  chartHumidity: document.getElementById("chart-humidity"),
  chartHumidityValue: document.getElementById("chart-humidity-value"),
  chartLux: document.getElementById("chart-lux"),
  chartLuxValue: document.getElementById("chart-lux-value"),
  chartMq2: document.getElementById("chart-mq2"),
  chartMq2Value: document.getElementById("chart-mq2-value"),
  mmwaveHealth: document.getElementById("mmwave-health"),
  mmwaveBodyState: document.getElementById("mmwave-body-state"),
  mmwaveFusion: document.getElementById("mmwave-fusion"),
  mmwaveMoving: document.getElementById("mmwave-moving"),
  mmwaveStationary: document.getElementById("mmwave-stationary"),
  mmwaveDetectDistance: document.getElementById("mmwave-detect-distance"),
  mmwaveMovingEnergy: document.getElementById("mmwave-moving-energy"),
  mmwaveStationaryEnergy: document.getElementById("mmwave-stationary-energy"),
  mmwaveBoundary: document.getElementById("mmwave-boundary"),
  healthSummary: document.getElementById("health-summary"),
  healthGrid: document.getElementById("health-grid"),
  modeCurrent: document.getElementById("mode-current"),
  modeCurrentDetail: document.getElementById("mode-current-detail"),
  modeNoMotion: document.getElementById("mode-no-motion"),
  modeDemoTimeout: document.getElementById("mode-demo-timeout"),
  modeRealTimeout: document.getElementById("mode-real-timeout"),
  modeThresholdTemperature: document.getElementById("mode-threshold-temperature"),
  modeThresholdLux: document.getElementById("mode-threshold-lux"),
  modeThresholdMq2: document.getElementById("mode-threshold-mq2"),
  modeThresholdEscalation: document.getElementById("mode-threshold-escalation"),
  metricTick: document.getElementById("metric-tick"),
  metricReason: document.getElementById("metric-reason"),
  metricStateCard: document.getElementById("metric-state-card"),
  riskSummary: document.getElementById("risk-summary"),
  riskCategory: document.getElementById("risk-category"),
  heroCard: document.getElementById("hero-card"),
  heroLiveBadge: document.getElementById("hero-live-badge"),
  heroStateMark: document.getElementById("hero-state-mark"),
  heroState: document.getElementById("hero-state"),
  heroRisk: document.getElementById("hero-risk"),
  heroReason: document.getElementById("hero-reason"),
  heroDevice: document.getElementById("hero-device"),
  heroLastSeen: document.getElementById("hero-last-seen"),
  heroReasonCode: document.getElementById("hero-reason-code"),
  rawPayloadBox: document.getElementById("raw-payload-box"),
  autoRefreshState: document.getElementById("auto-refresh-state"),
  speechAsrState: document.getElementById("speech-asr-state"),
  speechFileInput: document.getElementById("speech-file-input"),
  speechUploadButton: document.getElementById("speech-upload-button"),
  speechResultText: document.getElementById("speech-result-text"),
  speechResultMeta: document.getElementById("speech-result-meta"),
  speechAiReply: document.getElementById("speech-ai-reply"),
  speechAiMeta: document.getElementById("speech-ai-meta"),
  speechAiAudio: document.getElementById("speech-ai-audio"),
  voiceModeTitle: document.getElementById("voice-mode-title"),
  voiceModeButtons: Array.from(document.querySelectorAll("[data-voice-mode]")),
  voiceModeNote: document.getElementById("voice-mode-note"),
  voicePromptsStatus: document.getElementById("voice-prompts-status"),
  voicePromptsList: document.getElementById("voice-prompts-list"),
  voicePromptsSaveButton: document.getElementById("voice-prompts-save-button"),
};

const AUTO_REFRESH_MS = 3000;
const ALERTS_PAGE_SIZE = 6;
const STALE_AFTER_MS = 18000;
const OFFLINE_AFTER_MS = 45000;
const RISK_THRESHOLDS = {
  humidityDisplayPercent: 100,
};
const RULE_PROFILES = {
  REAL: {
    modeLabel: "真实模式",
    noMotionRemindMs: 5 * 60 * 1000,
    remindEscalationMs: 30 * 60 * 1000,
    temperatureHighC: 32,
    temperatureDurationText: "持续 5 分钟",
    temperatureCooldownText: "OK 后冷却 10 分钟",
    enterRestLux: 20,
    exitRestLux: 50,
    mq2RemindRaw: 1200,
    mq2RemindDurationText: "持续 10 秒",
    mq2AlarmRaw: 1500,
    mq2AlarmDurationText: "持续 20 秒",
    mq2RecoverText: "回落并稳定 5 分钟",
    escalationText: "无活动 / MQ2 轻度提醒 30 分钟无人响应升级 ALERT",
  },
  DEMO: {
    modeLabel: "演示模式",
    noMotionRemindMs: 30 * 1000,
    remindEscalationMs: 15 * 1000,
    temperatureHighC: 30,
    temperatureDurationText: "持续 30 秒",
    temperatureCooldownText: "OK 后冷却 1 分钟",
    enterRestLux: 30,
    exitRestLux: 60,
    mq2RemindRaw: 1000,
    mq2RemindDurationText: "持续 3 秒",
    mq2AlarmRaw: 1300,
    mq2AlarmDurationText: "持续 5 秒",
    mq2RecoverText: "回落并稳定 10 秒",
    escalationText: "无活动 / MQ2 轻度提醒 15 秒无人响应升级 ALERT",
  },
};
const VOICE_MODE_META = {
  CARE: {
    title: "看护模式",
    note: "看护模式会优先回答安全提醒和风险解释。",
  },
  INTERACT: {
    title: "互动模式",
    note: "互动模式允许更自然地聊天，但仍保留紧急求助和医疗边界。",
  },
  OFFLINE: {
    title: "离线模式",
    note: "已接收到信息。如有紧急情况，请按红色按钮。网络暂不可用。",
  },
};

history.scrollRestoration = "manual";

let currentAlertsPage = 1;
let latestAlerts = [];
let currentAlertStateFilter = "ALL";
let currentView = "";
let voicePromptItems = [];
const VIEW_BY_HASH = {
  "#hero-card": "status",
  "#snapshot-panel": "detect",
  "#alerts-panel": "events",
  "#mode-panel": "mode",
  "#debug-panel": "debug",
};

function viewFromHash() {
  return VIEW_BY_HASH[window.location.hash] || "";
}

function sensorBarPercent(value, threshold) {
  const numeric = Number(value);
  if (!Number.isFinite(numeric)) {
    return 0;
  }
  return Math.max(4, Math.min(100, (numeric / (threshold / 0.8)) * 100));
}

function setSensorBar(bar, value, threshold) {
  if (!bar) {
    return;
  }
  bar.style.width = `${sensorBarPercent(value, threshold)}%`;
}

function activateView(view) {
  currentView = view;
  document.body.classList.toggle("view-focused", Boolean(view));
  if (refs.appFrame) {
    refs.appFrame.scrollTop = 0;
  }

  for (const item of refs.navItems) {
    item.classList.toggle("active", item.dataset.viewLink === view);
  }

  for (const block of refs.viewBlocks) {
    const isLowerGrid = block === refs.lowerGrid;
    const shouldShow =
      !view ||
      block.dataset.view === view ||
      (isLowerGrid && ["detect", "events", "mode", "debug"].includes(view));
    block.classList.toggle("is-view-active", shouldShow);
  }
}

function formatValue(value) {
  return value === null || value === undefined || value === "" ? "-" : String(value);
}

function formatNumber(value, unit = "") {
  if (value === null || value === undefined || value === "") {
    return "-";
  }

  const numeric = Number(value);
  if (!Number.isFinite(numeric)) {
    return `${value}${unit ? ` ${unit}` : ""}`;
  }

  const formatted = Number.isInteger(numeric) ? numeric.toString() : numeric.toFixed(1);
  return `${formatted}${unit ? ` ${unit}` : ""}`;
}

function formatBoolean(value) {
  if (value === true || value === "true" || value === 1 || value === "1") {
    return "检测到";
  }
  if (value === false || value === "false" || value === 0 || value === "0") {
    return "未检测到";
  }
  return formatValue(value);
}

function hasOwnValue(item, key) {
  return item && Object.prototype.hasOwnProperty.call(item, key);
}

function formatMmwaveState(item) {
  if (!hasOwnValue(item, "ld2410b_ok")) {
    return "未上报";
  }
  if (!item.ld2410b_ok) {
    return "模块异常";
  }
  if (!item.ld2410b_presence) {
    return "无人";
  }
  if (item.ld2410b_moving_target && item.ld2410b_stationary_target) {
    return "有人活动 + 静止目标";
  }
  if (item.ld2410b_moving_target) {
    return "有人活动";
  }
  if (item.ld2410b_stationary_target) {
    return "有人静止";
  }
  return "有人存在";
}

function formatMmwaveDistance(item) {
  if (!hasOwnValue(item, "ld2410b_ok")) {
    return "未上报";
  }
  if (!item.ld2410b_ok) {
    return "模块异常";
  }

  const distance = formatNumber(item.ld2410b_detection_distance_cm, "厘米");
  const movingEnergy = formatValue(item.ld2410b_moving_energy);
  const stationaryEnergy = formatValue(item.ld2410b_stationary_energy);
  return `${distance} / 动${movingEnergy} 静${stationaryEnergy}`;
}

function formatMs(value) {
  const numeric = Number(value);
  if (!Number.isFinite(numeric)) {
    return "-";
  }
  if (numeric < 1000) {
    return `${numeric} 毫秒`;
  }
  if (numeric < 60000) {
    return `${Math.round(numeric / 1000)} 秒`;
  }
  return `${Math.round(numeric / 60000)} 分钟`;
}

function stateLabel(state) {
  switch (normalizeState(state)) {
    case "NORMAL":
      return "正常";
    case "REMIND":
      return "提醒";
    case "ALARM":
      return "告警";
    case "SOS":
      return "主动求助";
    case "OFFLINE":
      return "离线";
    default:
      return "暂无数据";
  }
}

function riskLabel(level) {
  switch (formatValue(level).toUpperCase()) {
    case "NORMAL":
      return "正常";
    case "REMIND":
      return "提醒";
    case "WARNING":
      return "警告";
    case "EMERGENCY":
      return "紧急";
    default:
      return formatValue(level);
  }
}

function reasonText(reason) {
  const value = formatValue(reason);
  const lower = value.toLowerCase();
  if (value === "-") {
    return "-";
  }
  if (lower === "state_unknown") {
    return "当前无明确异常原因";
  }
  if (lower.includes("sos") || lower.includes("manual")) {
    return "用户主动触发 SOS 求助";
  }
  if (lower.includes("mq2") || lower.includes("gas") || lower.includes("smoke") || value.includes("烟雾") || value.includes("气体")) {
    return "检测到烟雾或可燃气体异常";
  }
  if (lower.includes("timeout") || value.includes("超时")) {
    return "提醒后未及时确认，已升级处理";
  }
  if (lower.includes("motion") || lower.includes("inactive") || lower.includes("activity") || value.includes("静止") || value.includes("活动")) {
    return "长时间未检测到明显活动";
  }
  return value.replace(/_/g, " ");
}

function fusionText(item) {
  if (!hasOwnValue(item, "ld2410b_ok")) {
    return "未上报融合状态";
  }
  if (item.ld2410b_ok) {
    return "毫米波有效，优先使用人体存在/静止判断";
  }
  return "毫米波不可用，当前无法提供人体存在融合判断";
}

function healthValue(item, key) {
  if (!hasOwnValue(item, key)) {
    return null;
  }
  return Boolean(item[key]);
}

function healthLabel(value) {
  if (value === null) {
    return "未上报";
  }
  return value ? "正常" : "异常";
}

function healthClass(value) {
  if (value === null) {
    return "state-idle";
  }
  return value ? "state-normal" : "state-alarm";
}

function formatTimestamp(value) {
  if (!value) {
    return "-";
  }

  const parsed = new Date(value);
  if (Number.isNaN(parsed.getTime())) {
    return String(value);
  }

  return parsed.toLocaleString("zh-CN", { hour12: false });
}

function formatAge(value) {
  if (!value) {
    return "-";
  }

  const parsed = new Date(value);
  if (Number.isNaN(parsed.getTime())) {
    return "-";
  }

  const deltaMs = Date.now() - parsed.getTime();
  const seconds = Math.max(0, Math.floor(deltaMs / 1000));
  if (seconds < 60) {
    return `${seconds} 秒前`;
  }

  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) {
    return `${minutes} 分 ${seconds % 60} 秒前`;
  }

  const hours = Math.floor(minutes / 60);
  return `${hours} 小时 ${minutes % 60} 分前`;
}

function formatBytes(value) {
  const numeric = Number(value);
  if (!Number.isFinite(numeric) || numeric < 0) {
    return "-";
  }
  if (numeric < 1024) {
    return `${numeric} B`;
  }
  if (numeric < 1024 * 1024) {
    return `${(numeric / 1024).toFixed(1)} KB`;
  }
  return `${(numeric / 1024 / 1024).toFixed(2)} MB`;
}

function inferAudioFormat(file) {
  const extension = file.name.split(".").pop()?.toLowerCase();
  if (["wav", "mp3", "m4a", "pcm", "opus", "ogg"].includes(extension)) {
    return extension;
  }

  if (file.type.includes("mpeg")) {
    return "mp3";
  }
  if (file.type.includes("mp4") || file.type.includes("m4a")) {
    return "m4a";
  }
  if (file.type.includes("opus")) {
    return "opus";
  }
  return "wav";
}

function normalizeState(state) {
  return formatValue(state).toUpperCase();
}

function normalizeVoiceMode(mode) {
  return VOICE_MODE_META[mode] ? mode : "CARE";
}

function getRuleProfile(runMode) {
  return String(runMode || "").toUpperCase() === "REAL" ? RULE_PROFILES.REAL : RULE_PROFILES.DEMO;
}

function lightSceneInfo(luxValue, runMode) {
  const profile = getRuleProfile(runMode);
  const lux = Number(luxValue);
  if (!Number.isFinite(lux)) {
    return {
      title: "等待光照数据",
      note: "需要设备上报 lux 后，才能判断白天活动或夜晚睡眠场景。",
    };
  }

  if (lux <= profile.enterRestLux) {
    return {
      title: "夜晚睡眠场景",
      note: `当前光照较低（<= ${profile.enterRestLux} lx），系统会按休息场景抑制长时间无活动误报。`,
    };
  }

  if (lux >= profile.exitRestLux) {
    return {
      title: "白天活动场景",
      note: `当前光照充足（>= ${profile.exitRestLux} lx），系统按日间活动场景正常监测无活动风险。`,
    };
  }

  return {
    title: "过渡光照场景",
    note: `当前光照位于 ${profile.enterRestLux}-${profile.exitRestLux} lx 之间，系统会结合持续时间和活动变化判断是否进入或退出休息场景。`,
  };
}

function renderVoiceMode(mode, offlineReply = "", selectedMode = mode, deviceOffline = false) {
  const normalized = normalizeVoiceMode(mode);
  const meta = VOICE_MODE_META[normalized];
  refs.voiceModeTitle.textContent = meta.title;
  refs.voiceModeNote.textContent = normalized === "OFFLINE" && offlineReply
    ? offlineReply
    : deviceOffline
      ? "设备离线时自动使用离线模式，恢复上报后回到网页选择的模式。"
      : meta.note;

  for (const button of refs.voiceModeButtons) {
    button.classList.toggle("active", button.dataset.voiceMode === selectedMode);
  }
}

function createVoicePromptRow(item) {
  const article = document.createElement("article");
  article.className = "voice-prompt-item";
  article.dataset.eventKey = item.event_key;

  const header = document.createElement("div");
  header.className = "voice-prompt-head";

  const titleBox = document.createElement("div");
  const key = document.createElement("strong");
  key.textContent = item.event_key;
  const label = document.createElement("p");
  label.textContent = item.label;
  titleBox.append(key, label);

  const actions = document.createElement("div");
  actions.className = "voice-prompt-actions";

  const toggleLabel = document.createElement("label");
  toggleLabel.className = "voice-prompt-toggle";
  const toggle = document.createElement("input");
  toggle.type = "checkbox";
  toggle.checked = Boolean(item.enabled);
  toggle.dataset.field = "enabled";
  const toggleText = document.createElement("span");
  toggleText.textContent = "启用";
  toggleLabel.append(toggle, toggleText);

  const previewButton = document.createElement("button");
  previewButton.type = "button";
  previewButton.textContent = "试听";
  previewButton.dataset.action = "preview";
  previewButton.dataset.eventKey = item.event_key;

  actions.append(toggleLabel, previewButton);
  header.append(titleBox, actions);

  const textArea = document.createElement("textarea");
  textArea.className = "voice-prompt-text";
  textArea.rows = 2;
  textArea.value = item.tts_text || "";
  textArea.dataset.field = "tts_text";

  const footer = document.createElement("div");
  footer.className = "voice-prompt-foot";

  const cooldownLabel = document.createElement("label");
  cooldownLabel.className = "voice-prompt-cooldown";
  const cooldownTitle = document.createElement("span");
  cooldownTitle.textContent = "冷却毫秒";
  const cooldownInput = document.createElement("input");
  cooldownInput.type = "number";
  cooldownInput.min = "0";
  cooldownInput.step = "1000";
  cooldownInput.value = String(item.cooldown_ms ?? 0);
  cooldownInput.dataset.field = "cooldown_ms";
  cooldownLabel.append(cooldownTitle, cooldownInput);

  const updated = document.createElement("small");
  updated.textContent = `最近更新：${formatTimestamp(item.updated_at)}`;

  footer.append(cooldownLabel, updated);
  article.append(header, textArea, footer);
  return article;
}

function renderVoicePrompts(items) {
  voicePromptItems = Array.isArray(items) ? items : [];
  refs.voicePromptsList.innerHTML = "";

  if (!voicePromptItems.length) {
    refs.voicePromptsStatus.textContent = "暂无状态播报配置。";
    return;
  }

  refs.voicePromptsStatus.textContent = `共 ${voicePromptItems.length} 条状态播报配置。`;
  for (const item of voicePromptItems) {
    refs.voicePromptsList.appendChild(createVoicePromptRow(item));
  }
}

function collectVoicePromptUpdates() {
  return Array.from(refs.voicePromptsList.querySelectorAll(".voice-prompt-item")).map((node) => ({
    event_key: node.dataset.eventKey,
    enabled: node.querySelector('[data-field="enabled"]').checked,
    tts_text: node.querySelector('[data-field="tts_text"]').value.trim(),
    cooldown_ms: Number(node.querySelector('[data-field="cooldown_ms"]').value || 0),
  }));
}

function friendlyReason(reason) {
  return reasonText(reason);
}

function getFreshness(receivedAt) {
  if (!receivedAt) {
    return "idle";
  }

  const parsed = new Date(receivedAt);
  if (Number.isNaN(parsed.getTime())) {
    return "idle";
  }

  const deltaMs = Date.now() - parsed.getTime();
  if (deltaMs >= OFFLINE_AFTER_MS) {
    return "offline";
  }
  if (deltaMs >= STALE_AFTER_MS) {
    return "stale";
  }
  return "live";
}

function freshnessLabel(freshness) {
  const labels = {
    idle: "等待数据",
    live: "在线",
    stale: "延迟",
    offline: "离线",
  };
  return labels[freshness] || "等待数据";
}

function stateMark(state, freshness = "live") {
  if (freshness === "offline") {
    return "离";
  }
  if (freshness === "stale") {
    return "延";
  }

  switch (normalizeState(state)) {
    case "NORMAL":
      return "正";
    case "REMIND":
      return "提";
    case "ALARM":
      return "告";
    case "SOS":
      return "求";
    default:
      return "等";
  }
}

function stateClass(state, freshness = "live") {
  if (freshness === "offline") {
    return "state-offline";
  }
  if (freshness === "stale") {
    return "state-stale";
  }

  switch (normalizeState(state)) {
    case "SOS":
      return "state-sos";
    case "ALARM":
      return "state-alarm";
    case "REMIND":
      return "state-remind";
    case "NORMAL":
      return "state-normal";
    default:
      return "state-idle";
  }
}

function reasonCategory(item) {
  const state = normalizeState(item?.state);
  const reason = formatValue(item?.reason).toLowerCase();

  if (state === "SOS" || reason.includes("sos") || reason.includes("manual")) {
    return {
      label: "主动求助",
      title: "用户主动触发 SOS",
      detail: "为什么提醒：用户主动求助。老人应保持在设备附近等待帮助；家属应优先电话确认或现场查看。",
    };
  }
  if (reason.includes("mq2") || reason.includes("gas") || reason.includes("smoke")) {
    return {
      label: "环境异常",
      title: "环境传感器检测到异常",
      detail: "为什么提醒：烟雾或可燃气体读数异常。老人应远离风险源；家属应确认厨房、燃气和通风情况。",
    };
  }
  if (
    reason.includes("motion") ||
    reason.includes("inactive") ||
    reason.includes("activity") ||
    reason.includes("ld2410") ||
    reason.includes("presence") ||
    reason.includes("stationary") ||
    reason.includes("毫米波") ||
    reason.includes("静止") ||
    reason.includes("活动")
  ) {
    return {
      label: "活动异常",
      title: "活动状态需要关注",
      detail: "为什么提醒：长时间未检测到明显活动或毫米波判断有人静止。老人可按确认键解除；家属应关注是否需要协助。",
    };
  }
  if (state === "ALARM") {
    return {
      label: "高风险告警",
      title: "系统已进入告警状态",
      detail: "为什么提醒：提醒阶段未确认或风险较高。老人可补按确认键解除；家属应尽快核验老人状态。",
    };
  }
  if (state === "REMIND") {
    return {
      label: "主动提醒",
      title: "系统正在等待老人确认",
      detail: "为什么提醒：设备发现可能异常，正在先提醒老人。老人按确认键可解除；未确认会升级为告警。",
    };
  }
  if (state === "NORMAL") {
    return {
      label: "安全监测",
      title: "当前状态稳定",
      detail: "当前没有触发异常状态，设备仍在持续监测环境、活动和毫米波人体存在信息。",
    };
  }
  return {
    label: "等待数据",
    title: "暂无风险判断",
    detail: "等待设备上报当前状态、风险等级和原因。",
  };
}

function setText(element, value) {
  element.textContent = formatValue(value);
}

function createTextCell(label, value) {
  const node = document.createElement("div");
  node.className = "event-field";

  const key = document.createElement("span");
  key.textContent = label;

  const val = document.createElement("strong");
  val.textContent = formatValue(value);

  node.append(key, val);
  return node;
}

function renderHero(item) {
  if (!item) {
    refs.heroCard.className =
      `status-hero view-block state-idle ${!currentView || currentView === "status" ? "is-view-active" : ""}`;
    refs.heroLiveBadge.textContent = "等待数据";
    refs.heroStateMark.textContent = "等";
    refs.heroState.textContent = "暂无数据";
    refs.heroRisk.textContent = "风险等级：-";
    refs.heroReason.textContent = "等待设备上报第一帧遥测数据。";
    refs.heroDevice.textContent = "-";
    refs.heroLastSeen.textContent = "-";
    refs.heroReasonCode.textContent = "-";
    return;
  }

  const freshness = getFreshness(item.received_at);
  const displayState = freshness === "offline" ? "离线" : stateLabel(item.state);
  const riskPrefix =
    freshness === "offline" ? "数据超时 / " : freshness === "stale" ? "数据延迟 / " : "";

  refs.heroCard.className =
    `status-hero view-block ${stateClass(item.state, freshness)} ${!currentView || currentView === "status" ? "is-view-active" : ""}`;
  refs.heroLiveBadge.textContent = freshnessLabel(freshness);
  refs.heroStateMark.textContent = stateMark(item.state, freshness);
  refs.heroState.textContent = displayState;
  refs.heroRisk.textContent = `风险等级：${riskPrefix}${riskLabel(item.risk_level)}`;
  refs.heroReason.textContent =
    freshness === "offline"
      ? "近期没有收到遥测数据，请检查设备供电、Wi-Fi、热点或云端连接。"
      : freshness === "stale"
        ? "设备上报有延迟，下方显示最近一次已知状态。"
        : friendlyReason(item.reason);
  refs.heroDevice.textContent = formatValue(item.device_id);
  refs.heroLastSeen.textContent = `${formatTimestamp(item.received_at)} (${formatAge(item.received_at)})`;
  refs.heroReasonCode.textContent = friendlyReason(item.reason);
}

function renderLatestSnapshot(item) {
  if (!item) {
    refs.snapshotGrid.hidden = true;
    refs.snapshotEmpty.hidden = false;
    refs.rawPayloadBox.textContent = "等待数据...";
    refs.metricReason.textContent = "-";
    setSensorBar(refs.metricTemperatureBar, 0, RULE_PROFILES.DEMO.temperatureHighC);
    setSensorBar(refs.metricHumidityBar, 0, RISK_THRESHOLDS.humidityDisplayPercent);
    setSensorBar(refs.metricLuxBar, 0, RULE_PROFILES.DEMO.enterRestLux);
    setSensorBar(refs.metricMq2Bar, 0, RULE_PROFILES.DEMO.mq2RemindRaw);
    refs.metricTemperatureThreshold.textContent = "高温提醒 >= 32°C 持续 5 分钟";
    refs.metricHumidityThreshold.textContent = "当前仅遥测展示，不单独触发风险";
    refs.metricLuxThreshold.textContent = "休息场景 <= 20 lx；亮光退出 >= 50 lx";
    refs.metricMq2Threshold.textContent = "提醒 >= 1200；告警 >= 1500";
    refs.metricLightScene.textContent = "-";
    refs.metricLightSceneNote.textContent = "根据当前光照判断白天活动或夜晚睡眠场景";
    refs.riskCategory.textContent = "-";
    renderMmwavePanel(null);
    renderHealthPanel(null);
    renderModePanel(null);
    renderMiniChart(null);
    return;
  }

  refs.snapshotEmpty.hidden = true;
  refs.snapshotGrid.hidden = false;
  const profile = getRuleProfile(item.run_mode);
  const lightScene = lightSceneInfo(item.lux, item.run_mode);
  refs.metricState.textContent = `${stateLabel(item.state)}/${riskLabel(item.risk_level)}`;
  refs.metricTemperature.textContent = formatNumber(item.temperature, "°C");
  refs.metricHumidity.textContent = formatNumber(item.humidity, "%");
  refs.metricLux.textContent = formatNumber(item.lux, "lx");
  refs.metricMq2.textContent = formatNumber(item.mq2_raw);
  refs.metricTemperatureThreshold.textContent =
    `高温提醒 >= ${profile.temperatureHighC}°C ${profile.temperatureDurationText}`;
  refs.metricHumidityThreshold.textContent = "当前仅遥测展示，不单独触发风险";
  refs.metricLuxThreshold.textContent =
    `休息场景 <= ${profile.enterRestLux} lx；亮光退出 >= ${profile.exitRestLux} lx`;
  refs.metricMq2Threshold.textContent =
    `提醒 >= ${profile.mq2RemindRaw}；告警 >= ${profile.mq2AlarmRaw}`;
  refs.metricLightScene.textContent = lightScene.title;
  refs.metricLightSceneNote.textContent = lightScene.note;
  setSensorBar(refs.metricTemperatureBar, item.temperature, profile.temperatureHighC);
  setSensorBar(refs.metricHumidityBar, item.humidity, RISK_THRESHOLDS.humidityDisplayPercent);
  setSensorBar(refs.metricLuxBar, item.lux, profile.enterRestLux);
  setSensorBar(refs.metricMq2Bar, item.mq2_raw, profile.mq2RemindRaw);
  refs.metricMmwave.textContent = formatMmwaveState(item);
  refs.metricMmwaveDistance.textContent = formatMmwaveDistance(item);
  refs.metricTick.textContent = `设备运行 ${formatValue(item.timestamp_ms)} 毫秒`;
  refs.metricReason.textContent = friendlyReason(item.reason);
  refs.metricStateCard.className = `metric-card metric-card-state ${stateClass(item.state, getFreshness(item.received_at))}`;
  refs.rawPayloadBox.textContent = JSON.stringify(item, null, 2);

  const category = reasonCategory(item);
  refs.riskSummary.className = `risk-summary ${stateClass(item.state, getFreshness(item.received_at))}`;
  refs.riskSummary.innerHTML = "";

  const label = document.createElement("span");
  label.className = "reason-type";
  label.textContent = category.label;
  const title = document.createElement("h3");
  title.textContent = category.title;
  const detail = document.createElement("p");
  detail.textContent = category.detail;
  refs.riskSummary.append(label, title, detail);
  refs.riskCategory.textContent = category.label;
  renderMmwavePanel(item);
  renderHealthPanel(item);
  renderModePanel(item);
  renderMiniChart(item);
}

function clampPercent(value, min, max) {
  const numeric = Number(value);
  if (!Number.isFinite(numeric)) {
    return 0;
  }
  return Math.max(4, Math.min(100, ((numeric - min) / (max - min)) * 100));
}

function setBar(bar, label, value, percent) {
  bar.style.width = `${percent}%`;
  label.textContent = value;
}

function renderMiniChart(item) {
  if (!item) {
    refs.miniChartNote.textContent = "等待设备数据";
    setBar(refs.chartTemp, refs.chartTempValue, "-", 0);
    setBar(refs.chartHumidity, refs.chartHumidityValue, "-", 0);
    setBar(refs.chartLux, refs.chartLuxValue, "-", 0);
    setBar(refs.chartMq2, refs.chartMq2Value, "-", 0);
    return;
  }

  refs.miniChartNote.textContent = "按当前遥测值归一化显示";
  setBar(
    refs.chartTemp,
    refs.chartTempValue,
    formatNumber(item.temperature, "°C"),
    clampPercent(item.temperature, 0, 45),
  );
  setBar(
    refs.chartHumidity,
    refs.chartHumidityValue,
    formatNumber(item.humidity, "%"),
    clampPercent(item.humidity, 0, 100),
  );
  setBar(refs.chartLux, refs.chartLuxValue, formatNumber(item.lux, "lx"), clampPercent(item.lux, 0, 1200));
  setBar(refs.chartMq2, refs.chartMq2Value, formatNumber(item.mq2_raw), clampPercent(item.mq2_raw, 0, 4095));
}

function renderMmwavePanel(item) {
  if (!item) {
    refs.mmwaveHealth.textContent = "未上报";
    refs.mmwaveBodyState.textContent = "-";
    refs.mmwaveFusion.textContent = "-";
    refs.mmwaveMoving.textContent = "-";
    refs.mmwaveStationary.textContent = "-";
    refs.mmwaveDetectDistance.textContent = "-";
    refs.mmwaveMovingEnergy.textContent = "-";
    refs.mmwaveStationaryEnergy.textContent = "-";
    refs.mmwaveBoundary.textContent = "增强存在/静止判断，不宣称精准跌倒检测";
    return;
  }

  const health = healthValue(item, "ld2410b_ok");
  refs.mmwaveHealth.textContent = healthLabel(health);
  refs.mmwaveHealth.className = `quiet-pill ${healthClass(health)}`;
  refs.mmwaveBodyState.textContent = formatMmwaveState(item);
  refs.mmwaveFusion.textContent = fusionText(item);
  refs.mmwaveMoving.textContent =
    `${formatNumber(item.ld2410b_moving_distance_cm, "厘米")} / 能量 ${formatValue(item.ld2410b_moving_energy)}`;
  refs.mmwaveStationary.textContent =
    `${formatNumber(item.ld2410b_stationary_distance_cm, "厘米")} / 能量 ${formatValue(item.ld2410b_stationary_energy)}`;
  refs.mmwaveDetectDistance.textContent = formatNumber(item.ld2410b_detection_distance_cm, "厘米");
  refs.mmwaveMovingEnergy.textContent = formatValue(item.ld2410b_moving_energy);
  refs.mmwaveStationaryEnergy.textContent = formatValue(item.ld2410b_stationary_energy);
  refs.mmwaveBoundary.textContent = "用于增强人体存在/静止判断，不宣称精准跌倒检测";
}

function renderHealthPanel(item) {
  const sensors = [
    ["AHT20 温湿度", "sensor_aht20_ok"],
    ["BH1750 光照", "sensor_bh1750_ok"],
    ["MQ2 烟雾气体", "sensor_mq2_ok"],
    ["LD2410B 毫米波", "ld2410b_ok"],
  ];
  refs.healthGrid.innerHTML = "";

  let okCount = 0;
  let reportedCount = 0;
  for (const [label, key] of sensors) {
    const value = item ? healthValue(item, key) : null;
    if (value !== null) {
      reportedCount += 1;
      if (value) {
        okCount += 1;
      }
    }

    const node = document.createElement("div");
    node.className = `health-item ${healthClass(value)}`;
    const name = document.createElement("span");
    name.textContent = label;
    const state = document.createElement("strong");
    state.textContent = healthLabel(value);
    node.append(name, state);
    refs.healthGrid.appendChild(node);
  }

  refs.healthSummary.textContent = reportedCount === 0 ? "未上报" : `${okCount}/${reportedCount} 正常`;
  refs.healthSummary.className =
    `quiet-pill ${reportedCount > 0 && okCount === reportedCount ? "state-normal" : "state-remind"}`;
}

function renderModePanel(item) {
  const mode = formatValue(item?.run_mode);
  const profile = getRuleProfile(mode);
  const modeText = mode === "REAL" ? "真实模式" : mode === "DEMO" ? "演示模式" : "未上报";
  refs.modeCurrent.textContent = modeText;
  refs.modeCurrent.className = `quiet-pill ${mode === "REAL" ? "state-normal" : mode === "DEMO" ? "state-remind" : "state-idle"}`;
  refs.modeCurrentDetail.textContent = modeText;
  refs.modeNoMotion.textContent = formatMs(item?.no_motion_remind_ms ?? profile.noMotionRemindMs);
  refs.modeDemoTimeout.textContent = formatMs(item?.remind_confirm_timeout_demo_ms ?? 15000);
  refs.modeRealTimeout.textContent = formatMs(item?.remind_confirm_timeout_real_ms ?? 300000);
  refs.modeThresholdTemperature.textContent =
    `>= ${profile.temperatureHighC}°C ${profile.temperatureDurationText}；${profile.temperatureCooldownText}`;
  refs.modeThresholdLux.textContent =
    `进入休息 <= ${profile.enterRestLux} lx；退出 >= ${profile.exitRestLux} lx`;
  refs.modeThresholdMq2.textContent =
    `提醒 >= ${profile.mq2RemindRaw} ${profile.mq2RemindDurationText}；告警 >= ${profile.mq2AlarmRaw} ${profile.mq2AlarmDurationText}`;
  refs.modeThresholdEscalation.textContent =
    `${profile.escalationText}；MQ2 恢复需 ${profile.mq2RecoverText}`;
}

function renderAlerts(items) {
  refs.alertsList.innerHTML = "";

  const sortedItems = [...items].sort((a, b) => {
    const aTime = new Date(a.received_at || 0).getTime();
    const bTime = new Date(b.received_at || 0).getTime();
    return bTime - aTime;
  });
  const filteredItems =
    currentAlertStateFilter === "ALL"
      ? sortedItems
      : sortedItems.filter((item) => normalizeState(item.state) === currentAlertStateFilter);

  if (!filteredItems.length) {
    refs.alertsList.hidden = true;
    refs.emptyState.hidden = false;
    refs.alertsPagination.hidden = true;
    refs.emptyState.textContent =
      currentAlertStateFilter === "ALL"
        ? "暂无异常事件。"
        : `暂无 ${stateLabel(currentAlertStateFilter)} 事件。`;
    refs.alertsPageInfo.textContent =
      currentAlertStateFilter === "ALL" ? "按最新事件优先显示" : `已筛选：${stateLabel(currentAlertStateFilter)}`;
    return;
  }

  const totalPages = Math.max(1, Math.ceil(filteredItems.length / ALERTS_PAGE_SIZE));
  currentAlertsPage = Math.min(currentAlertsPage, totalPages);

  const startIndex = (currentAlertsPage - 1) * ALERTS_PAGE_SIZE;
  const pageItems = filteredItems.slice(startIndex, startIndex + ALERTS_PAGE_SIZE);

  refs.emptyState.hidden = true;
  refs.alertsList.hidden = false;
  refs.alertsPagination.hidden = totalPages <= 1;
  refs.alertsPageInfo.textContent =
    currentAlertStateFilter === "ALL"
      ? `共 ${filteredItems.length} 条异常事件，按最新优先显示`
      : `共 ${filteredItems.length} 条 ${stateLabel(currentAlertStateFilter)} 事件`;
  refs.alertsPaginationInfo.textContent = `第 ${currentAlertsPage} / ${totalPages} 页`;
  refs.alertsPrevButton.disabled = currentAlertsPage <= 1;
  refs.alertsNextButton.disabled = currentAlertsPage >= totalPages;

  for (const item of pageItems) {
    const article = document.createElement("article");
    article.className = `event-card ${stateClass(item.state)}`;

    const badge = document.createElement("div");
    badge.className = "event-badge";
    badge.textContent = stateLabel(item.state);

    const body = document.createElement("div");
    body.className = "event-body";

    const topLine = document.createElement("div");
    topLine.className = "event-topline";

    const title = document.createElement("h3");
    const category = reasonCategory(item);
    title.textContent = `${riskLabel(item.risk_level)} · ${friendlyReason(item.reason)}`;

    const time = document.createElement("time");
    time.textContent = formatTimestamp(item.received_at);

    topLine.append(title, time);

    const fields = document.createElement("div");
    fields.className = "event-fields";
    fields.append(
      createTextCell("设备", item.device_id),
      createTextCell("温度", formatNumber(item.temperature, "°C")),
      createTextCell("湿度", formatNumber(item.humidity, "%")),
      createTextCell("光照", formatNumber(item.lux, "lx")),
      createTextCell("烟雾气体", item.mq2_raw),
      createTextCell("毫米波", formatMmwaveState(item)),
      createTextCell("距离/能量", formatMmwaveDistance(item)),
    );

    const explanation = document.createElement("p");
    explanation.className = "event-explanation";
    explanation.textContent = category.detail;

    body.append(topLine, fields, explanation);
    article.append(badge, body);
    refs.alertsList.appendChild(article);
  }
}

function renderServiceStatus(data) {
  const freshness = getFreshness(data.lastReceivedAt);
  const stateText =
    !data.ok ? "不可用" : freshness === "offline" ? "离线" : freshness === "stale" ? "延迟" : "运行中";

  refs.serviceState.textContent = stateText;
  refs.serviceState.className = `status-pill ${freshness === "live" ? "state-normal" : stateClass(null, freshness)}`;
  refs.serviceCount.textContent = formatValue(data.totalAlerts);
  refs.serviceLastReceived.textContent = formatTimestamp(data.lastReceivedAt);
  refs.serviceAge.textContent = formatAge(data.lastReceivedAt);
  refs.endpointBox.textContent =
    `设备上报 ${data.alertPath} / 最新状态 ${data.latestPath} / 服务状态 /api/status / 异常事件 /api/alerts / 语音上传 ${data.speechTranscribePath} / 语音回复 ${data.speechReplyAudioPath || "/api/speech/reply-audio"} / 状态播报 ${data.voicePromptsPath || "/api/voice-prompts"}`;
  const speechState = data.aiReplyConfigured
    ? `语音识别 + 智能回复 · ${data.aiReplyModel || "模型"}`
    : data.speechAsrConfigured
      ? data.speechAsrEngine || "语音识别就绪"
      : "语音识别未配置";
  refs.speechAsrState.textContent = data.voiceMode === "OFFLINE" ? "离线兜底模式" : speechState;
  refs.speechAsrState.className = `quiet-pill ${data.speechAsrConfigured ? "state-normal" : "state-stale"}`;
  renderVoiceMode(data.voiceMode, data.offlineVoiceReply, data.selectedVoiceMode || data.voiceMode, data.deviceOffline);
}

async function loadServiceStatus() {
  const response = await fetch("/api/status", { cache: "no-store" });
  if (response.status === 401) {
    window.location.href = "/login.html";
    return;
  }
  if (!response.ok) {
    throw new Error("服务状态加载失败");
  }

  const data = await response.json();
  renderServiceStatus(data);
}

async function loadLatestSnapshot() {
  const response = await fetch("/api/latest", { cache: "no-store" });
  if (response.status === 401) {
    window.location.href = "/login.html";
    return;
  }
  if (!response.ok) {
    throw new Error("最新状态加载失败");
  }

  const data = await response.json();
  const item = data.latest || null;
  renderHero(item);
  renderLatestSnapshot(item);
}

async function loadAlerts() {
  const response = await fetch("/api/alerts", { cache: "no-store" });
  if (response.status === 401) {
    window.location.href = "/login.html";
    return;
  }
  if (!response.ok) {
    throw new Error("异常事件加载失败");
  }

  const data = await response.json();
  latestAlerts = Array.isArray(data.alerts) ? data.alerts : [];
  renderAlerts(latestAlerts);
}

function renderLatestSpeech(item) {
  if (!item) {
    refs.speechResultText.textContent = "暂无语音识别结果。";
    refs.speechResultMeta.textContent = "上传短音频可测试云端语音识别。";
    refs.speechAiReply.textContent = "暂无智能回复。";
    refs.speechAiMeta.textContent = "配置云端模型后生成简短回复。";
    refs.speechAiAudio.hidden = true;
    refs.speechAiAudio.removeAttribute("src");
    return;
  }

  refs.speechResultText.textContent = item.result || "识别结果为空";
  refs.speechResultMeta.textContent =
    `${formatTimestamp(item.received_at)} · ${formatBytes(item.bytes)} · ${formatValue(item.voice_format)} · ${formatValue(item.engine)} · ${formatValue(item.voice_mode_label || item.voice_mode)}`;
  refs.speechAiReply.textContent = item.ai_reply || "暂无智能回复。";
  refs.speechAiMeta.textContent = item.ai_error
    ? `智能回复失败：${item.ai_error}`
    : item.voice_mode === "OFFLINE"
      ? "离线模式固定回复。"
      : item.ai_model
      ? `模型：${item.ai_model}`
      : "智能回复模型未配置。";
  if (item.ai_reply && !item.ai_error) {
    refs.speechAiAudio.src = `/api/speech/reply-audio?t=${encodeURIComponent(item.received_at || Date.now())}`;
    refs.speechAiAudio.hidden = false;
  } else {
    refs.speechAiAudio.hidden = true;
    refs.speechAiAudio.removeAttribute("src");
  }
}

async function loadLatestSpeech() {
  const response = await fetch("/api/speech/latest", { cache: "no-store" });
  const contentType = response.headers.get("content-type") || "";
  if (response.status === 401 || contentType.includes("text/html")) {
    window.location.href = "/login.html";
    return;
  }
  if (!response.ok) {
    throw new Error("语音结果加载失败");
  }

  const data = await response.json();
  renderLatestSpeech(data.latest || null);
}

async function loadVoiceMode() {
  const response = await fetch("/api/voice-mode", { cache: "no-store" });
  if (response.status === 401) {
    window.location.href = "/login.html";
    return;
  }
  if (!response.ok) {
    throw new Error("语音模式加载失败");
  }

  const data = await response.json();
  renderVoiceMode(data.mode, data.offlineReply, data.selectedMode || data.mode, data.deviceOffline);
}

async function loadVoicePrompts() {
  const response = await fetch("/api/voice-prompts", { cache: "no-store" });
  if (response.status === 401) {
    window.location.href = "/login.html";
    return;
  }
  if (!response.ok) {
    throw new Error("状态播报配置加载失败");
  }

  const data = await response.json();
  renderVoicePrompts(data.items || []);
}

async function setVoiceMode(mode) {
  for (const button of refs.voiceModeButtons) {
    button.disabled = true;
  }

  try {
    const response = await fetch("/api/voice-mode", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ mode }),
    });
    const data = await response.json().catch(() => null);
    if (!response.ok || !data?.ok) {
      throw new Error(data?.message || "语音模式切换失败");
    }
    renderVoiceMode(data.mode, data.offlineReply, data.selectedMode || data.mode, data.deviceOffline);
    await loadServiceStatus();
  } catch (error) {
    refs.voiceModeNote.textContent = error.message;
  } finally {
    for (const button of refs.voiceModeButtons) {
      button.disabled = false;
    }
  }
}

async function saveVoicePrompts() {
  refs.voicePromptsSaveButton.disabled = true;
  refs.voicePromptsStatus.textContent = "正在保存状态播报配置...";

  try {
    const items = collectVoicePromptUpdates();
    const response = await fetch("/api/voice-prompts", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ items }),
    });
    const data = await response.json().catch(() => null);
    if (!response.ok || !data?.ok) {
      throw new Error(data?.message || "状态播报配置保存失败");
    }

    renderVoicePrompts(data.items || []);
    refs.voicePromptsStatus.textContent = "状态播报配置已保存。";
  } catch (error) {
    refs.voicePromptsStatus.textContent = error.message;
  } finally {
    refs.voicePromptsSaveButton.disabled = false;
  }
}

async function previewVoicePrompt(eventKey, button) {
  if (!eventKey) {
    return;
  }

  button.disabled = true;
  refs.voicePromptsStatus.textContent = `正在试听 ${eventKey}...`;

  try {
    const audio = new Audio(`/api/voice-prompts/audio?event_key=${encodeURIComponent(eventKey)}&t=${Date.now()}`);
    await audio.play();
    refs.voicePromptsStatus.textContent = `正在试听 ${eventKey}。`;
  } catch (error) {
    refs.voicePromptsStatus.textContent = `试听失败：${error.message}`;
  } finally {
    button.disabled = false;
  }
}

async function uploadSpeechFile() {
  const file = refs.speechFileInput.files[0];
  if (!file) {
    refs.speechResultMeta.textContent = "请先选择音频文件。";
    return;
  }

  refs.speechUploadButton.disabled = true;
  refs.speechResultMeta.textContent = "正在上传音频并等待识别...";
  refs.speechAiMeta.textContent = "如已配置模型，将继续生成智能回复...";
  try {
    const format = inferAudioFormat(file);
    const response = await fetch(`/api/speech/transcribe?format=${encodeURIComponent(format)}`, {
      method: "POST",
      headers: {
        "Content-Type": file.type || "application/octet-stream",
        "X-Audio-Format": format,
      },
      body: file,
    });
    const data = await response.json().catch(() => null);

    if (!response.ok || !data?.ok) {
      throw new Error(data?.message || "语音识别失败");
    }

    renderLatestSpeech(data.speech);
  } catch (error) {
    refs.speechResultText.textContent = "识别失败";
    refs.speechResultMeta.textContent = error.message;
  } finally {
    refs.speechUploadButton.disabled = false;
  }
}

async function refreshAll() {
  refs.refreshButton.disabled = true;

  const [serviceResult, snapshotResult, alertsResult, speechResult, voiceModeResult, voicePromptsResult] = await Promise.allSettled([
    loadServiceStatus(),
    loadLatestSnapshot(),
    loadAlerts(),
    loadLatestSpeech(),
    loadVoiceMode(),
    loadVoicePrompts(),
  ]);

  if (serviceResult.status === "rejected") {
    refs.serviceState.textContent = "错误";
    refs.serviceState.className = "status-pill state-offline";
    refs.serviceCount.textContent = "-";
    refs.serviceLastReceived.textContent = "-";
    refs.serviceAge.textContent = serviceResult.reason.message;
  }

  if (snapshotResult.status === "rejected") {
    refs.heroLiveBadge.textContent = "延迟";
  }

  if (alertsResult.status === "rejected") {
    refs.alertsPageInfo.textContent = alertsResult.reason.message;
  }

  if (speechResult.status === "rejected") {
    refs.speechResultMeta.textContent = `语音结果刷新失败：${speechResult.reason.message}`;
  }

  if (voiceModeResult.status === "rejected") {
    refs.voiceModeNote.textContent = `语音模式刷新失败：${voiceModeResult.reason.message}`;
  }

  if (voicePromptsResult.status === "rejected") {
    refs.voicePromptsStatus.textContent = `状态播报配置刷新失败：${voicePromptsResult.reason.message}`;
  }

  refs.refreshButton.disabled = false;
}

async function logout() {
  refs.logoutButton.disabled = true;
  try {
    await fetch("/api/logout", { method: "POST", cache: "no-store" });
  } finally {
    window.location.href = "/login.html";
  }
}

refs.refreshButton.addEventListener("click", refreshAll);
refs.logoutButton.addEventListener("click", logout);
refs.speechUploadButton.addEventListener("click", uploadSpeechFile);
refs.voicePromptsSaveButton.addEventListener("click", saveVoicePrompts);
refs.voicePromptsList.addEventListener("click", (event) => {
  const button = event.target.closest('[data-action="preview"]');
  if (!button) {
    return;
  }
  previewVoicePrompt(button.dataset.eventKey, button);
});
for (const button of refs.voiceModeButtons) {
  button.addEventListener("click", () => setVoiceMode(button.dataset.voiceMode));
}
for (const item of refs.navItems) {
  item.addEventListener("click", (event) => {
    event.preventDefault();
    activateView(item.dataset.viewLink);
    history.replaceState(null, "", item.getAttribute("href"));
    window.scrollTo({ top: 0, behavior: "smooth" });
  });
}
window.addEventListener("hashchange", () => {
  activateView(viewFromHash());
  window.scrollTo({ top: 0, behavior: "smooth" });
});
refs.alertsStateFilter.addEventListener("change", (event) => {
  currentAlertStateFilter = event.target.value;
  currentAlertsPage = 1;
  renderAlerts(latestAlerts);
});
refs.alertsPrevButton.addEventListener("click", () => {
  if (currentAlertsPage <= 1) {
    return;
  }
  currentAlertsPage -= 1;
  renderAlerts(latestAlerts);
});
refs.alertsNextButton.addEventListener("click", () => {
  const filteredCount =
    currentAlertStateFilter === "ALL"
      ? latestAlerts.length
      : latestAlerts.filter((item) => normalizeState(item.state) === currentAlertStateFilter).length;
  const totalPages = Math.max(1, Math.ceil(filteredCount / ALERTS_PAGE_SIZE));
  if (currentAlertsPage >= totalPages) {
    return;
  }
  currentAlertsPage += 1;
  renderAlerts(latestAlerts);
});

refs.autoRefreshState.textContent = `每 ${AUTO_REFRESH_MS / 1000} 秒`;
activateView(viewFromHash());
requestAnimationFrame(() => window.scrollTo(0, 0));
refreshAll();
setInterval(refreshAll, AUTO_REFRESH_MS);
