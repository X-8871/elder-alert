const express = require("express");
const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

const app = express();
const port = process.env.PORT || 3000;
const alertPath = "/api/alert";
const latestPath = "/api/latest";
const speechTranscribePath = "/api/speech/transcribe";
const speechLatestPath = "/api/speech/latest";
const maxAlerts = 50;
const maxSpeechRecords = 10;
const maxSpeechAudioBytes = 600 * 1024;
const deviceToken = (process.env.ELDER_ALERT_TOKEN || "").trim();
const tencentAsrConfig = {
  secretId: (process.env.TENCENTCLOUD_SECRET_ID || "").trim(),
  secretKey: (process.env.TENCENTCLOUD_SECRET_KEY || "").trim(),
  appId: (process.env.TENCENTCLOUD_ASR_APPID || "").trim(),
  engine: (process.env.TENCENTCLOUD_ASR_ENGINE || "16k_zh").trim(),
  region: (process.env.TENCENTCLOUD_ASR_REGION || "ap-shanghai").trim(),
};
const dataDir = path.join(__dirname, "data");
const storePath = path.join(dataDir, "alerts-store.json");

const alerts = [];
const speechRecords = [];
let latestSnapshot = null;
let latestSpeech = null;

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

function tokenMatches(providedToken, expectedToken) {
  if (typeof providedToken !== "string" || providedToken.length === 0) {
    return false;
  }

  const provided = Buffer.from(providedToken);
  const expected = Buffer.from(expectedToken);
  return provided.length === expected.length && crypto.timingSafeEqual(provided, expected);
}

function hasTencentAsrConfig() {
  return Boolean(tencentAsrConfig.secretId && tencentAsrConfig.secretKey);
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
    speechTranscribePath,
    speechLatestPath,
    speechAsrConfigured: hasTencentAsrConfig(),
    speechAsrEngine: tencentAsrConfig.engine || "16k_zh",
    totalAlerts: alerts.length,
    totalSpeechRecords: speechRecords.length,
    lastReceivedAt: latestSnapshot ? latestSnapshot.received_at : null,
    lastSpeechAt: latestSpeech ? latestSpeech.received_at : null,
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

app.get(speechLatestPath, (req, res) => {
  res.json({
    ok: true,
    latest: latestSpeech,
    records: speechRecords,
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

  return res.status(200).json({
    ok: true,
    message: "telemetry received",
    count: alerts.length,
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
      const record = {
        result: asrResponse.Result || "",
        audio_duration_ms: asrResponse.AudioDuration ?? null,
        word_size: asrResponse.WordSize ?? null,
        voice_format: voiceFormat,
        engine: tencentAsrConfig.engine || "16k_zh",
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

      console.log(
        `[speech] format=${record.voice_format} bytes=${record.bytes} duration_ms=${record.audio_duration_ms} result=${record.result}`
      );

      return res.status(200).json({
        ok: true,
        speech: record,
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
  console.log(`[store] using ${storePath}`);
  console.log(`[auth] device token ${deviceToken ? "enabled" : "disabled"}`);
  console.log(`[speech] Tencent ASR ${hasTencentAsrConfig() ? "configured" : "not configured"}`);
});
