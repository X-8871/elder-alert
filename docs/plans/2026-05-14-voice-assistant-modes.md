# Voice Assistant Modes Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add three webpage-controlled voice assistant modes: care, chat, and offline fallback.

**Architecture:** Keep ESP32 as the recorder/player and keep mode selection in the cloud dashboard first. `server.js` owns the current voice mode, applies mode-specific AI reply behavior after ASR, and exposes mode APIs for the webpage. ESP32 does not need to know the mode in phase 1; it uploads audio and plays cloud TTS when available, then falls back to existing local voice prompt if cloud playback fails.

**Tech Stack:** ESP-IDF on ESP32-S3, Node.js + Express cloud dashboard, vanilla HTML/CSS/JS frontend, Tencent ASR, OpenAI-compatible chat/TTS APIs, MAX98357A playback, INMP441 recording.

---

## Confirmed Product Decisions

- Mode switching is implemented on the webpage first.
- `CARE` is the default mode.
- `CHAT` can depend on cloud AI.
- `OFFLINE` in phase 1 means fixed local fallback and server-side no-AI behavior, not local ASR or local LLM.

## Offline Mode Clarification

There are two different offline meanings:

- Fixed local fallback: no cloud understanding. ESP32 plays built-in prompts such as `我听到了` or future fixed phrases like `网络不可用，请按SOS键`. The current project already supports this class through `VoicePrompt`.
- True offline voice assistant: local wake/record, local speech recognition, local intent/LLM, local TTS. ESP32-S3 in this project does not currently support this, and it would require a separate architecture and much more memory/storage/compute.

Phase 1 should use fixed local fallback only.

## Mode Semantics

### CARE

Default mode. Replies are short and safety-oriented.

Behavior:

- Explain current state and risk in plain language.
- For emergency words such as `救命`、`摔倒`、`头晕`、`不舒服`、`危险`, tell the user to press SOS or contact family.
- For medical or medication questions, do not diagnose; advise contacting a doctor or family.
- Do not claim that family has been notified unless the system really sends a notification.

### CHAT

Light companionship mode.

Behavior:

- Allow simple greeting, time, lightweight chat, and emotional reassurance.
- Keep emergency and medical safety boundaries.
- Do not trigger alarm state, clear buzzer, or change device control behavior.
- Keep replies short enough for speaker playback.

### OFFLINE

Fallback-oriented mode.

Behavior:

- On the server, do not call cloud AI/TTS for open-ended chat.
- Reply text should be a fixed safe sentence, for example `当前为离线模式，如需帮助请按SOS键。`
- On ESP32, existing cloud playback failure still falls back to `VoicePrompt_PlayUploadOk()`.
- Future enhancement may add multiple built-in fixed prompts, but phase 1 should not implement local ASR/LLM.

---

## Task 1: Add Server-Side Voice Mode State

**Files:**

- Modify: `server.js`

**Step 1: Add constants and state**

Add near the existing config constants:

```js
const voiceModes = new Set(["CARE", "CHAT", "OFFLINE"]);
let currentVoiceMode = "CARE";
```

**Step 2: Add helper**

```js
function normalizeVoiceMode(value) {
  const mode = String(value || "").trim().toUpperCase();
  return voiceModes.has(mode) ? mode : "CARE";
}
```

**Step 3: Add APIs**

Add protected dashboard APIs:

```js
app.get("/api/voice-mode", requireDashboardAuth, (req, res) => {
  res.json({ ok: true, mode: currentVoiceMode });
});

app.post("/api/voice-mode", requireDashboardAuth, (req, res) => {
  currentVoiceMode = normalizeVoiceMode(req.body?.mode);
  res.json({ ok: true, mode: currentVoiceMode });
});
```

**Step 4: Include mode in status**

Add to `/api/status` response:

```js
voiceMode: currentVoiceMode,
```

**Step 5: Verify syntax**

Run:

```powershell
node --check server.js
```

Expected: no output and exit code 0.

---

## Task 2: Add Mode-Specific AI Reply Policy

**Files:**

- Modify: `server.js`

**Step 1: Add prompt builder**

Add:

```js
function buildAiSystemPrompt(mode) {
  if (mode === "CHAT") {
    return (
      "你是独居老人家里的陪伴语音助手。回答要自然、温和、简短，最多40个汉字。" +
      "可以闲聊和问候，但不要假装能完成你不能完成的事。" +
      "医疗、用药、诊断问题要建议咨询医生或家属。" +
      "危险、求救、不舒服或紧急情况，要提醒按SOS键或联系家属。"
    );
  }

  return (
    "你是独居老人家里的安全看护语音助手。直接回答，最多40个汉字。" +
    "优先关注安全、风险解释、确认键和SOS求助。" +
    "不要闲聊太多，不要复述用户原话，不要输出分析过程。" +
    "医疗、用药、诊断问题要建议咨询医生或家属。" +
    "危险、求救、不舒服或紧急情况，要提醒按SOS键或联系家属。"
  );
}
```

**Step 2: Add offline reply helper**

```js
function buildOfflineModeReply() {
  return "当前为离线模式，如需帮助请按SOS键。";
}
```

**Step 3: Pass mode into AI generation**

Change `generateAiReply(transcript)` to accept mode:

```js
async function generateAiReply(transcript, mode = currentVoiceMode) {
```

At the start of the function:

```js
const normalizedMode = normalizeVoiceMode(mode);
if (normalizedMode === "OFFLINE") {
  return buildOfflineModeReply();
}
```

Use `buildAiSystemPrompt(normalizedMode)` for the system message.

**Step 4: Store mode in speech record**

When creating the speech record, include:

```js
voice_mode: currentVoiceMode,
```

**Step 5: Use record mode during background generation**

Change `runAiReplyGeneration(record, transcript)` so it calls:

```js
const reply = await generateAiReply(transcript, record.voice_mode);
```

**Step 6: Verify syntax**

Run:

```powershell
node --check server.js
```

Expected: no output and exit code 0.

---

## Task 3: Add Webpage Mode Switch UI

**Files:**

- Modify: `public/index.html`
- Modify: `public/app.js`
- Modify: `public/styles.css`

**Step 1: Add controls to debug panel**

In `public/index.html`, inside `debug-panel` before `.speech-upload`, add a compact segmented control:

```html
<div class="voice-mode-card" aria-label="语音助手模式">
  <span class="section-label">语音助手模式</span>
  <div class="voice-mode-options" role="group" aria-label="语音助手模式">
    <button type="button" data-voice-mode="CARE">看护</button>
    <button type="button" data-voice-mode="CHAT">陪聊</button>
    <button type="button" data-voice-mode="OFFLINE">离线</button>
  </div>
  <p id="voice-mode-note">看护模式会优先回答安全提醒和风险解释。</p>
</div>
```

**Step 2: Add refs**

In `public/app.js` refs:

```js
voiceModeButtons: Array.from(document.querySelectorAll("[data-voice-mode]")),
voiceModeNote: document.getElementById("voice-mode-note"),
```

**Step 3: Add labels**

```js
const VOICE_MODE_NOTES = {
  CARE: "看护模式会优先回答安全提醒和风险解释。",
  CHAT: "陪聊模式可做轻量聊天，但仍保留安全边界。",
  OFFLINE: "离线模式不依赖云端智能回答，优先提示按SOS求助。",
};
```

**Step 4: Render mode**

```js
function renderVoiceMode(mode) {
  const normalized = ["CARE", "CHAT", "OFFLINE"].includes(mode) ? mode : "CARE";
  for (const button of refs.voiceModeButtons) {
    button.classList.toggle("active", button.dataset.voiceMode === normalized);
  }
  if (refs.voiceModeNote) {
    refs.voiceModeNote.textContent = VOICE_MODE_NOTES[normalized];
  }
}
```

**Step 5: Load mode**

```js
async function refreshVoiceMode() {
  const response = await fetch("/api/voice-mode");
  if (!response.ok) {
    throw new Error("voice mode unavailable");
  }
  const data = await response.json();
  renderVoiceMode(data.mode);
}
```

Call it from the normal refresh flow.

**Step 6: Save mode**

```js
async function setVoiceMode(mode) {
  const response = await fetch("/api/voice-mode", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ mode }),
  });
  if (!response.ok) {
    throw new Error("voice mode update failed");
  }
  const data = await response.json();
  renderVoiceMode(data.mode);
}
```

Hook buttons:

```js
for (const button of refs.voiceModeButtons) {
  button.addEventListener("click", () => setVoiceMode(button.dataset.voiceMode));
}
```

**Step 7: Add CSS**

Add compact styles in `public/styles.css`:

```css
.voice-mode-card {
  display: grid;
  gap: 10px;
  padding: 14px;
  border: 1px solid var(--line);
  border-radius: 18px;
  background: var(--surface-soft);
}

.voice-mode-options {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 8px;
}

.voice-mode-options button.active {
  border-color: #38bdf8;
  background: #e0f2fe;
  color: #075985;
}
```

Adapt variable names to existing CSS if needed.

**Step 8: Verify syntax**

Run:

```powershell
node --check public/app.js
node --check server.js
```

Expected: both pass.

---

## Task 4: Local Web Flow Verification

**Files:**

- No source changes unless bugs are found.

**Step 1: Start local server**

Run:

```powershell
$env:PORT='3100'; node server.js
```

Expected:

- Server starts.
- Logs show dashboard auth and speech config status.

**Step 2: Verify unauthenticated protection**

Request:

```powershell
Invoke-WebRequest http://127.0.0.1:3100/ -MaximumRedirection 0
```

Expected:

- Redirect to `/login.html`, or equivalent protected response.

**Step 3: Login**

Use browser or HTTP cookie flow with:

- username: `olderalert`
- password: `88888888`

Expected:

- Login returns 200.
- Dashboard loads.

**Step 4: Switch all modes**

In the dashboard debug view:

- Click `看护`.
- Click `陪聊`.
- Click `离线`.

Expected:

- Active button changes.
- Note text changes.
- `/api/status` returns matching `voiceMode`.

**Step 5: Confirm existing APIs still behave**

Expected:

- Logged-in dashboard can still fetch `/api/status`, `/api/latest`, `/api/alerts`, `/api/speech/latest`.
- `POST /api/alert` behavior is unchanged and still governed by device token logic only.

---

## Task 5: ESP-IDF Build Check

**Files:**

- No source changes expected in ESP32 firmware for phase 1.

**Step 1: Build firmware only if ESP32 files were touched**

If no `main/` or `components/` file changed, skip firmware build and record why.

If touched, run:

```powershell
C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe C:\Users\22061\esp\v5.5.2\tools\idf.py build
```

Expected:

- Build passes.
- `build/hello_world.bin` is generated.

---

## Task 6: Context Refresh

**Files:**

- Modify: `docs/ai_context/PROJECT_STATE.md`
- Modify: `docs/ai_context/SESSION_LOG.md`
- Modify: `docs/ai_context/OPEN_QUESTIONS.md`
- Modify: `docs/ai_context/CONTEXT_PACK.md`
- Modify if source map changes: `docs/ai_context/SOURCE_INDEX.md`
- Modify if roadmap status changes: `MVP作品实施路线图.md`

**Step 1: Record verified facts**

Add only verified implementation and test facts to `PROJECT_STATE.md`.

**Step 2: Record unresolved questions**

Keep these in `OPEN_QUESTIONS.md` until confirmed:

- Whether voice mode should later be switchable from ESP32 buttons.
- Whether to add multiple local fixed prompts for offline fallback.
- Whether dashboard login credentials should move from fixed demo values to environment variables.
- Whether online deployment should expose a read-only judging link.

**Step 3: Compress handoff**

Update `CONTEXT_PACK.md` with one concise paragraph about the mode feature and next verification state.

---

## Phase 1 Done Criteria

- Webpage can switch `CARE / CHAT / OFFLINE`.
- `CARE` is default.
- `server.js` stores the current mode and returns it in `/api/status`.
- ASR records include `voice_mode`.
- AI reply behavior differs between care and chat modes.
- Offline mode does not call cloud AI for open-ended reply generation.
- Existing ESP32 recording/playback path remains unchanged.
- `node --check server.js` and `node --check public/app.js` pass.
- Context md files are updated.

