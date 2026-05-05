const express = require("express");
const fs = require("fs");
const path = require("path");

const app = express();
const port = process.env.PORT || 3000;
const alertPath = "/api/alert";
const latestPath = "/api/latest";
const maxAlerts = 50;
const dataDir = path.join(__dirname, "data");
const storePath = path.join(dataDir, "alerts-store.json");

const alerts = [];
let latestSnapshot = null;

const requiredFields = [
  "device_id",
  "state",
  "risk_level",
  "reason",
  "temperature",
  "humidity",
  "lux",
  "mq2_raw",
  "motion_detected",
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

function ensureDataDir() {
  fs.mkdirSync(dataDir, { recursive: true });
}

function loadStore() {
  try {
    ensureDataDir();
    if (!fs.existsSync(storePath)) {
      return;
    }

    const raw = fs.readFileSync(storePath, "utf8");
    if (!raw.trim()) {
      return;
    }

    const parsed = JSON.parse(raw);
    const loadedAlerts = Array.isArray(parsed.alerts) ? parsed.alerts.slice(0, maxAlerts) : [];
    alerts.splice(0, alerts.length, ...loadedAlerts);
    latestSnapshot = parsed.latestSnapshot && typeof parsed.latestSnapshot === "object"
      ? parsed.latestSnapshot
      : null;
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
    };
    fs.writeFileSync(storePath, JSON.stringify(payload, null, 2), "utf8");
  } catch (error) {
    console.warn(`[store] failed to save ${storePath}: ${error.message}`);
  }
}

app.use(express.json());
app.use(express.static(path.join(__dirname, "public")));

app.get("/api/status", (req, res) => {
  res.json({
    ok: true,
    service: "elder-alert-dashboard",
    alertPath,
    latestPath,
    totalAlerts: alerts.length,
    lastReceivedAt: latestSnapshot ? latestSnapshot.received_at : null,
  });
});

app.get(latestPath, (req, res) => {
  res.json({
    ok: true,
    latest: latestSnapshot,
  });
});

app.get("/api/alerts", (req, res) => {
  res.json({
    ok: true,
    count: alerts.length,
    alerts,
  });
});

app.post(alertPath, (req, res) => {
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

  return res.status(200).json({
    ok: true,
    message: "telemetry received",
    count: alerts.length,
  });
});

loadStore();

app.listen(port, () => {
  console.log(`Local dashboard running at http://localhost:${port}`);
  console.log(`POST alerts to http://localhost:${port}${alertPath}`);
  console.log(`[store] using ${storePath}`);
});
