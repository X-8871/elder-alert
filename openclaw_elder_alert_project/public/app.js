const refs = {
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
  snapshotEmpty: document.getElementById("snapshot-empty"),
  snapshotGrid: document.getElementById("snapshot-grid"),
  metricState: document.getElementById("metric-state"),
  metricTemperature: document.getElementById("metric-temperature"),
  metricHumidity: document.getElementById("metric-humidity"),
  metricLux: document.getElementById("metric-lux"),
  metricMq2: document.getElementById("metric-mq2"),
  metricMotion: document.getElementById("metric-motion"),
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
};

const AUTO_REFRESH_MS = 3000;
const ALERTS_PAGE_SIZE = 6;
const STALE_AFTER_MS = 18000;
const OFFLINE_AFTER_MS = 45000;

let currentAlertsPage = 1;
let latestAlerts = [];
let currentAlertStateFilter = "ALL";

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
    return "Detected";
  }
  if (value === false || value === "false" || value === 0 || value === "0") {
    return "None";
  }
  return formatValue(value);
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
    return `${seconds}s ago`;
  }

  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) {
    return `${minutes}m ${seconds % 60}s ago`;
  }

  const hours = Math.floor(minutes / 60);
  return `${hours}h ${minutes % 60}m ago`;
}

function normalizeState(state) {
  return formatValue(state).toUpperCase();
}

function friendlyReason(reason) {
  const value = formatValue(reason);
  if (value === "-") {
    return "-";
  }

  return value
    .replace(/_/g, " ")
    .replace(/\b\w/g, (letter) => letter.toUpperCase());
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
    idle: "WAITING",
    live: "LIVE",
    stale: "STALE",
    offline: "OFFLINE",
  };
  return labels[freshness] || "WAITING";
}

function stateMark(state, freshness = "live") {
  if (freshness === "offline") {
    return "!";
  }
  if (freshness === "stale") {
    return "...";
  }

  const normalized = normalizeState(state);
  return normalized === "-" ? "N" : normalized.slice(0, 3);
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
      title: "Manual SOS is active",
      detail: "设备检测到主动求助或紧急触发，应作为最高优先级事件处理。",
    };
  }
  if (reason.includes("mq2") || reason.includes("gas") || reason.includes("smoke")) {
    return {
      label: "环境异常",
      title: "Possible environmental risk",
      detail: "气体 / 烟雾相关读数触发风险判断，需要结合现场情况确认。",
    };
  }
  if (reason.includes("motion") || reason.includes("inactive") || reason.includes("activity")) {
    return {
      label: "活动异常",
      title: "Activity pattern needs attention",
      detail: "人体活动或长时间无响应相关条件触发提醒，适合用于独居场景看护。",
    };
  }
  if (state === "ALARM") {
    return {
      label: "高风险告警",
      title: "Alarm state requires intervention",
      detail: "系统状态机已进入 ALARM，建议现场核验老人状态与环境情况。",
    };
  }
  if (state === "REMIND") {
    return {
      label: "主动提醒",
      title: "Reminder state is active",
      detail: "系统处于提醒阶段，优先通过本地声光和屏幕提示完成闭环。",
    };
  }
  if (state === "NORMAL") {
    return {
      label: "安全监测",
      title: "Current condition is stable",
      detail: "当前上报状态为 NORMAL，传感器与状态机仍在持续监测。",
    };
  }
  return {
    label: "等待数据",
    title: "No active assessment",
    detail: "Waiting for current state, risk level, and reason from the device.",
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
    refs.heroCard.className = "status-hero state-idle";
    refs.heroLiveBadge.textContent = "WAITING";
    refs.heroStateMark.textContent = "N";
    refs.heroState.textContent = "NO DATA";
    refs.heroRisk.textContent = "Risk Level: -";
    refs.heroReason.textContent = "Waiting for the first telemetry packet.";
    refs.heroDevice.textContent = "-";
    refs.heroLastSeen.textContent = "-";
    refs.heroReasonCode.textContent = "-";
    return;
  }

  const freshness = getFreshness(item.received_at);
  const displayState = freshness === "offline" ? "OFFLINE" : normalizeState(item.state);
  const riskPrefix =
    freshness === "offline" ? "DATA TIMEOUT" : freshness === "stale" ? "STALE / " : "";

  refs.heroCard.className = `status-hero ${stateClass(item.state, freshness)}`;
  refs.heroLiveBadge.textContent = freshnessLabel(freshness);
  refs.heroStateMark.textContent = stateMark(item.state, freshness);
  refs.heroState.textContent = displayState;
  refs.heroRisk.textContent = `Risk Level: ${riskPrefix}${formatValue(item.risk_level)}`;
  refs.heroReason.textContent =
    freshness === "offline"
      ? "No recent telemetry. Check device power, Wi-Fi, hotspot, or cloud reachability."
      : freshness === "stale"
        ? "Telemetry is delayed. The latest known state is shown below."
        : friendlyReason(item.reason);
  refs.heroDevice.textContent = formatValue(item.device_id);
  refs.heroLastSeen.textContent = `${formatTimestamp(item.received_at)} (${formatAge(item.received_at)})`;
  refs.heroReasonCode.textContent = formatValue(item.reason);
}

function renderLatestSnapshot(item) {
  if (!item) {
    refs.snapshotGrid.hidden = true;
    refs.snapshotEmpty.hidden = false;
    refs.rawPayloadBox.textContent = "Waiting for data...";
    refs.metricReason.textContent = "-";
    refs.riskCategory.textContent = "-";
    return;
  }

  refs.snapshotEmpty.hidden = true;
  refs.snapshotGrid.hidden = false;
  refs.metricState.textContent = `${normalizeState(item.state)} / ${formatValue(item.risk_level)}`;
  refs.metricTemperature.textContent = formatNumber(item.temperature, "C");
  refs.metricHumidity.textContent = formatNumber(item.humidity, "%");
  refs.metricLux.textContent = formatNumber(item.lux, "lx");
  refs.metricMq2.textContent = formatNumber(item.mq2_raw);
  refs.metricMotion.textContent = formatBoolean(item.motion_detected);
  refs.metricTick.textContent = `Tick ${formatValue(item.timestamp_ms)}`;
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
        ? "No abnormal event received yet."
        : `No ${currentAlertStateFilter} event found.`;
    refs.alertsPageInfo.textContent =
      currentAlertStateFilter === "ALL" ? "Newest event first" : `Filtered by ${currentAlertStateFilter}`;
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
      ? `${filteredItems.length} abnormal event(s), newest first`
      : `${filteredItems.length} ${currentAlertStateFilter} event(s)`;
  refs.alertsPaginationInfo.textContent = `Page ${currentAlertsPage} / ${totalPages}`;
  refs.alertsPrevButton.disabled = currentAlertsPage <= 1;
  refs.alertsNextButton.disabled = currentAlertsPage >= totalPages;

  for (const item of pageItems) {
    const article = document.createElement("article");
    article.className = `event-card ${stateClass(item.state)}`;

    const badge = document.createElement("div");
    badge.className = "event-badge";
    badge.textContent = normalizeState(item.state);

    const body = document.createElement("div");
    body.className = "event-body";

    const topLine = document.createElement("div");
    topLine.className = "event-topline";

    const title = document.createElement("h3");
    title.textContent = `${formatValue(item.risk_level)} · ${friendlyReason(item.reason)}`;

    const time = document.createElement("time");
    time.textContent = formatTimestamp(item.received_at);

    topLine.append(title, time);

    const fields = document.createElement("div");
    fields.className = "event-fields";
    fields.append(
      createTextCell("Device", item.device_id),
      createTextCell("Temp", formatNumber(item.temperature, "C")),
      createTextCell("Humidity", formatNumber(item.humidity, "%")),
      createTextCell("Lux", formatNumber(item.lux, "lx")),
      createTextCell("MQ2", item.mq2_raw),
      createTextCell("Motion", formatBoolean(item.motion_detected)),
    );

    body.append(topLine, fields);
    article.append(badge, body);
    refs.alertsList.appendChild(article);
  }
}

function renderServiceStatus(data) {
  const freshness = getFreshness(data.lastReceivedAt);
  const stateText =
    !data.ok ? "Unavailable" : freshness === "offline" ? "Offline" : freshness === "stale" ? "Stale" : "Running";

  refs.serviceState.textContent = stateText;
  refs.serviceState.className = `status-pill ${freshness === "live" ? "state-normal" : stateClass(null, freshness)}`;
  refs.serviceCount.textContent = formatValue(data.totalAlerts);
  refs.serviceLastReceived.textContent = formatTimestamp(data.lastReceivedAt);
  refs.serviceAge.textContent = formatAge(data.lastReceivedAt);
  refs.endpointBox.textContent = `POST ${data.alertPath} / GET ${data.latestPath} / GET /api/status / GET /api/alerts`;
}

async function loadServiceStatus() {
  const response = await fetch("/api/status");
  if (!response.ok) {
    throw new Error("Failed to load service status");
  }

  const data = await response.json();
  renderServiceStatus(data);
}

async function loadLatestSnapshot() {
  const response = await fetch("/api/latest");
  if (!response.ok) {
    throw new Error("Failed to load latest snapshot");
  }

  const data = await response.json();
  const item = data.latest || null;
  renderHero(item);
  renderLatestSnapshot(item);
}

async function loadAlerts() {
  const response = await fetch("/api/alerts");
  if (!response.ok) {
    throw new Error("Failed to load alerts");
  }

  const data = await response.json();
  latestAlerts = Array.isArray(data.alerts) ? data.alerts : [];
  renderAlerts(latestAlerts);
}

async function refreshAll() {
  refs.refreshButton.disabled = true;
  try {
    await Promise.all([loadServiceStatus(), loadLatestSnapshot(), loadAlerts()]);
  } catch (error) {
    refs.serviceState.textContent = "Error";
    refs.serviceState.className = "status-pill state-offline";
    refs.serviceCount.textContent = "-";
    refs.serviceLastReceived.textContent = "-";
    refs.serviceAge.textContent = error.message;
  } finally {
    refs.refreshButton.disabled = false;
  }
}

refs.refreshButton.addEventListener("click", refreshAll);
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

refs.autoRefreshState.textContent = `Every ${AUTO_REFRESH_MS / 1000}s`;
refreshAll();
setInterval(refreshAll, AUTO_REFRESH_MS);
