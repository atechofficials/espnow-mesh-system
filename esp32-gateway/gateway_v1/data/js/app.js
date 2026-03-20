/**
 * ESP32 Mesh Gateway Web Interface Client v3.9
 *
 * WS Inbound:  { type:"meta"|"update"|"discovered"|"pair_timeout"|"ap_config_ack"|
 *                     "gw_portal_starting"|"gw_factory_reset"|"gw_rebooting"|
 *                     "auth_required"|"auth_ok"|"auth_fail"|"session_expired"|
 *                     "web_creds_ack"|"node_settings"|"node_sensor_schema"|
 *                     "relay_labels_ack" }
 * WS Outbound: { type:"auth"|"actuator_cmd"|"pair_cmd"|"unpair_cmd"|"rename_node"|
 *                     "reboot_gw"|"reboot_node"|"set_ap_config"|"set_web_credentials"|
 *                     "start_wifi_portal"|"factory_reset"|"node_settings_get"|
 *                     "node_settings_set"|"node_sensor_schema_get"|
 *                     "relay_labels_set"|"ping" }
 */
"use strict";

// ── Auth state ────────────────────────────────────────────────────────────────
let authToken        = null;  // session token (in-memory, cleared on page reload)
let authRemToken     = null;  // remember-me token (persisted in localStorage)
let webCredsJustSet  = false; // flag: user just set new web credentials

// ── State ─────────────────────────────────────────────────────────────────────
const nodes      = new Map();   // nodeId → NodeRecord
const discovered = new Map();   // mac → DiscoveredNode
const nodeSettings     = new Map(); // nodeId → SettingDef[]   (schema from node)
const nodeSensorSchemas = new Map(); // nodeId → SensorDef[]   (schema from node)
const RELAY_CHANNEL_COUNT     = 4;
let   ws         = null;
let   serverUptime   = 0;
let   uptimeSyncedAt = 0;
const pendingMacs    = new Set();
let   currentSection = "dashboard";
let   fwVersion      = "—";

// ── DOM ───────────────────────────────────────────────────────────────────────
const $ = id => document.getElementById(id);
const $nodeGrid   = $("node-grid");
const $dashEmpty  = $("dash-empty");
const $nodesTbody = $("nodes-tbody");
const $nodesEmpty = $("nodes-empty-row");
const $availList  = $("avail-list");
const $availEmpty = $("avail-empty");
const $mIp  = $("m-ip"); const $mMac = $("m-mac");
const $mCh  = $("m-ch"); const $mUp  = $("m-up"); const $mNd = $("m-nd");
const $wsPill   = $("ws-pill");
const $wsLabel  = $("ws-label");
const $nbNodes  = $("nb-nodes");
const $nbAvail  = $("nb-avail");
const $modal    = $("modal-overlay");
const $modalTitle = $("modal-title");
const $modalBody  = $("modal-body");
const $themeBtn = $("theme-btn");
const $toast    = $("toast");
const $gwReboot  = $("gw-reboot-btn");
const $gwFactory = $("gw-factory-btn");
const $gwLedToggle = $("gw-led-toggle");
const $logoutBtn = $("logout-btn");

// ── Node Settings Modal ────────────────────────────────────────────────────────
const $nsOverlay = $("nsettings-overlay");
const $nsTitle   = $("nsettings-title");
const $nsBody    = $("nsettings-body");
let   nsCurrentNodeId = 0;
let   pendingSettingsFetches = 0; // how many node_settings responses still outstanding
const relayLabelDrafts = new Map(); // nodeId -> string[]

// ── Settings ──────────────────────────────────────────────────────────────────
const $apSsidInput  = $("ap-ssid-input");
const $apPassInput  = $("ap-pass-input");
const $saveApBtn    = $("save-ap-btn");
const $apSaveNote   = $("ap-save-note");
const $wifiPortalBtn = $("wifi-portal-btn");
const $gwOtaFileInput = $("gw-ota-file");
const $gwOtaBtn = $("gw-ota-btn");
const $gwOtaNote = $("gw-ota-note");
const $gwOtaProgress = $("gw-ota-progress");
const $gwOtaProgressFill = $("gw-ota-progress-fill");
const $gwOtaProgressText = $("gw-ota-progress-text");
const $gwOtaCurrent = $("gw-ota-current");
const $gwOtaSlot = $("gw-ota-slot");
const $gwOtaProject = $("gw-ota-project");
const $nodeOtaNode = $("node-ota-node");
const $nodeOtaFileInput = $("node-ota-file");
const $nodeOtaBtn = $("node-ota-btn");
const $nodeOtaNote = $("node-ota-note");
const $nodeOtaProgress = $("node-ota-progress");
const $nodeOtaProgressFill = $("node-ota-progress-fill");
const $nodeOtaProgressText = $("node-ota-progress-text");
const $nodeOtaHelper = $("node-ota-helper");
const $nodeOtaTarget = $("node-ota-target");
const $nodeOtaHost = $("node-ota-host");

// ── Confirm modal ─────────────────────────────────────────────────────────────
const $confirmOverlay  = $("confirm-overlay");
const $confirmTitle    = $("confirm-title");
const $confirmBody     = $("confirm-body");
const $confirmOkBtn    = $("confirm-ok-btn");
const $confirmCancelBtn = $("confirm-cancel-btn");

// ── Theme ─────────────────────────────────────────────────────────────────────
let theme = localStorage.getItem("gwTheme") || "dark";
document.documentElement.dataset.theme = theme;
let gatewayOtaSupported = false;
let gatewayOtaBusy = false;
let gatewayOtaMaxBytes = 0;
let gatewayOtaProject = "";
let gatewayOtaUploading = false;
let nodeOtaBusy = false;
let nodeOtaUploading = false;
let nodeOtaHelperOnline = false;
let nodeOtaHost = "192.168.4.1";
let nodeOtaState = {
  active: false,
  node_id: 0,
  stage: 0,
  progress: 0,
  message: "",
  error: "",
  version: "",
  node_name: ""
};

$themeBtn.addEventListener("click", () => {
  theme = theme === "dark" ? "light" : "dark";
  document.documentElement.dataset.theme = theme;
  localStorage.setItem("gwTheme", theme);
});

// ── Navigation ────────────────────────────────────────────────────────────────
function navigate(section) {
  currentSection = section;

  document.querySelectorAll(".sb-btn").forEach(btn => {
    btn.classList.toggle("active", btn.dataset.section === section);
  });
  document.querySelectorAll(".sec").forEach(sec => {
    sec.classList.toggle("active", sec.id === "sec-" + section);
  });

  // Close mobile sidebar
  document.getElementById("sidebar").classList.remove("open");
  document.getElementById("sb-overlay").classList.remove("open");
}

document.querySelectorAll(".sb-btn[data-section]").forEach(btn => {
  btn.addEventListener("click", () => navigate(btn.dataset.section));
});

// Mobile menu toggle
$("menu-btn").addEventListener("click", () => {
  document.getElementById("sidebar").classList.toggle("open");
  document.getElementById("sb-overlay").classList.toggle("open");
});

$("sb-overlay").addEventListener("click", () => {
  document.getElementById("sidebar").classList.remove("open");
  $("sb-overlay").classList.remove("open");
});

// ── Auth ──────────────────────────────────────────────────────────────────────
function showLoginScreen(infoMsg) {
  const $overlay = $("login-overlay");
  $overlay.style.display = "flex";
  setLoginError("");
  if (infoMsg) setLoginInfo(infoMsg);
  // Focus username if empty, else password
  const uEl = $("login-username");
  const pEl = $("login-password");
  setTimeout(() => (uEl.value ? pEl.focus() : uEl.focus()), 80);
}

function hideLoginScreen() {
  $("login-overlay").style.display = "none";
  setLoginError("");
  setLoginInfo("");
}

function setLoginError(msg) {
  const el = $("login-error");
  el.textContent = msg;
  el.style.display = msg ? "" : "none";
}

function setLoginInfo(msg) {
  const el = $("login-info");
  el.textContent = msg;
  el.style.display = msg ? "" : "none";
}

async function doLogin() {
  const username = $("login-username").value.trim();
  const password = $("login-password").value;
  const remember = $("login-remember").checked;

  if (!username || !password) {
    setLoginError("Please enter your username and password.");
    return;
  }

  const btn = $("login-btn");
  btn.disabled = true;
  btn.textContent = "Signing in…";

  try {
    const resp = await fetch("/api/login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ username, password, remember })
    });
    const data = await resp.json();

    if (data.ok) {
      authToken = data.token;
      if (remember && data.remember_token) {
        authRemToken = data.remember_token;
        localStorage.setItem("gwRemToken", data.remember_token);
      } else if (!remember) {
        authRemToken = null;
        localStorage.removeItem("gwRemToken");
      }
      setLoginError("");
      $("login-password").value = "";
      hideLoginScreen();
      // If WS is already open send auth, else connect fresh
      if (ws && ws.readyState === WebSocket.OPEN) {
        send({ type: "auth", token: authToken });
      } else {
        connect();
      }
    } else {
      setLoginError(data.error || "Login failed. Please try again.");
    }
  } catch (e) {
    setLoginError("Connection error. Please try again.");
  } finally {
    btn.disabled = false;
    btn.innerHTML = `<span class="icon login-btn-icon"></span> Sign In`;
  }
}

async function doLogout() {
  try { await fetch("/api/logout", { method: "POST" }); } catch (e) { /* ignore */ }
  authToken    = null;
  authRemToken = null;
  localStorage.removeItem("gwRemToken");
  if (ws) { ws.close(); ws = null; }
  showLoginScreen();
}

// Called once on page load. Checks auth state and decides whether to show login
// or connect the WebSocket.
async function initAuth() {
  // Load any stored remember-me token
  const stored = localStorage.getItem("gwRemToken");
  if (stored) authRemToken = stored;

  try {
    const resp = await fetch("/api/auth_check");
    const data = await resp.json();

    if (!data.credentials_set) {
      // No credentials configured — open access
      $logoutBtn.style.display = "none";
      connect();
      return;
    }

    // Credentials are configured
    $logoutBtn.style.display = "";

    if (data.authenticated) {
      // Browser has a valid cookie; still need a token for WS auth.
      // Use the remember token from localStorage if we have it.
      if (!authToken && authRemToken) authToken = authRemToken;
      connect();
    } else if (authRemToken) {
      // Cookie may have expired but we have a remember token in localStorage — try it.
      authToken = authRemToken;
      connect();
    } else {
      // Not authenticated, no fallback token → show login
      showLoginScreen();
    }
  } catch (e) {
    // Network error — still try connecting, WS will handle auth_required
    connect();
  }
}

// Set web credentials note helper
function setWebCredsSaveNote(msg, cls) {
  const el = $("web-creds-note");
  if (!el) return;
  el.textContent = msg;
  el.className = "setting-save-note" + (cls ? " " + cls : "");
}

// ── WebSocket ─────────────────────────────────────────────────────────────────
let authSentThisConnection = false;  // prevents double-auth when proactive send races auth_required

function connect() {
  setWs("connecting");
  ws = new WebSocket(`ws://${location.host}/ws`);

  ws.addEventListener("open", () => {
    setWs("live");
    authSentThisConnection = false;
    // Send token proactively so auth completes before the server's auth_required arrives.
    if (authToken) {
      send({ type: "auth", token: authToken });
      authSentThisConnection = true;
    }
  });
  ws.addEventListener("close", () => {
    setWs("error");
    authSentThisConnection = false;
    setTimeout(connect, 3000);
  });
  ws.addEventListener("error", () => ws.close());

  ws.addEventListener("message", ({ data }) => {
    let msg;
    try { msg = JSON.parse(data); } catch { return; }

    switch (msg.type) {
      // ── Auth messages ────────────────────────────────────────────────────
      case "auth_required":
        // Guard: if we already sent auth proactively this connection, the
        // server's auth_required is just the racing connect-event message —
        // ignore it to avoid authenticating the same client twice.
        if (authSentThisConnection) break;
        if (authToken) {
          send({ type: "auth", token: authToken });
          authSentThisConnection = true;
        } else if (authRemToken) {
          authToken = authRemToken;
          send({ type: "auth", token: authToken });
          authSentThisConnection = true;
        } else {
          showLoginScreen();
        }
        break;

      case "auth_ok":
        // Successfully authenticated — UI is already visible, nothing more needed
        hideLoginScreen();
        break;

      case "auth_fail":
        // Token rejected — clear stored tokens and force login
        authToken    = null;
        authRemToken = null;
        localStorage.removeItem("gwRemToken");
        showLoginScreen("Session expired or invalid. Please log in again.");
        break;

      case "session_expired":
        // Server invalidated all sessions (e.g., new credentials were set or logout)
        authToken    = null;
        authRemToken = null;
        localStorage.removeItem("gwRemToken");
        if (webCredsJustSet) {
          webCredsJustSet = false;
          showLoginScreen("Credentials saved! Please log in with your new credentials.");
        } else {
          showLoginScreen("Your session has ended. Please log in again.");
        }
        break;

      case "web_creds_ack":
        if (msg.ok) {
          webCredsJustSet = true;
          setWebCredsSaveNote("✓ Credentials saved. Redirecting to login…", "ok");
          showToast("✓ Web interface is now password-protected.", "success");
          // Clear the form
          const wu = $("web-user-input"), wp = $("web-pass-input"), wc = $("web-pass-confirm");
          if (wu) wu.value = ""; if (wp) wp.value = ""; if (wc) wc.value = "";
        } else {
          setWebCredsSaveNote("✗ " + (msg.err || "Error saving credentials"), "err");
          showToast("✗ " + (msg.err || "Could not save credentials"), "error");
        }
        const scBtn = $("save-web-creds-btn");
        if (scBtn) scBtn.disabled = false;
        break;

      case "node_settings":
        nodeSettings.set(msg.node_id, msg.settings || []);
        if (pendingSettingsFetches > 0) pendingSettingsFetches--;
        if (nsCurrentNodeId === msg.node_id) renderNodeSettings(msg.node_id);
        // Refresh table so ⚙ button state updates
        renderConnectedNodes();
        break;
      case "node_sensor_schema":
        // Cache the schema and re-render the dashboard so labels/units appear.
        nodeSensorSchemas.set(msg.node_id, msg.sensors || []);
        renderDashboard();
        break;
      case "relay_labels_ack": {
        const nodeId = Number(msg.node_id || 0);
        if (msg.ok) {
          const labels = setRelayLabelsOnNode(nodeId, msg.labels || []);
          clearRelayLabelDraft(nodeId);
          if (labels) {
            collectRelayLabelInputs(nodeId).forEach((input, index) => {
              input.value = labels[index];
            });
            renderDashboard();
          }
          setRelayLabelSaveNote(nodeId, "Saved on gateway.", "ok");
          const n = nodes.get(nodeId);
          showToast(`Relay labels saved for ${n?.name || `Node #${nodeId}`}.`, "success");
        } else {
          setRelayLabelSaveNote(nodeId, msg.err || "Could not save relay labels.", "err");
          showToast(msg.err || "Could not save relay labels.", "error");
        }
        break;
      }
      case "node_ota_state":
        applyNodeOtaState(msg);
        break;
      case "update":
        const prevModalNode = nsCurrentNodeId ? nodes.get(nsCurrentNodeId) : null;
        const nextNodes = msg.nodes || [];

        nodes.clear();
        nextNodes.forEach(n => nodes.set(n.id, n));

        const activeIds = new Set(nextNodes.map(n => n.id));
        [...nodeSettings.keys()].forEach(id => {
          if (!activeIds.has(id)) nodeSettings.delete(id);
        });
        [...nodeSensorSchemas.keys()].forEach(id => {
          if (!activeIds.has(id)) nodeSensorSchemas.delete(id);
        });

        if (nsCurrentNodeId && !nodes.has(nsCurrentNodeId)) {
          const reboundId = prevModalNode?.mac ? findNodeIdByMac(prevModalNode.mac) : 0;
          if (reboundId) {
            const draft = getRelayLabelDraft(nsCurrentNodeId);
            if (draft) {
              relayLabelDrafts.set(reboundId, draft);
              relayLabelDrafts.delete(nsCurrentNodeId);
            }
            nsCurrentNodeId = reboundId;
          }
          else closeNodeSettings();
        }

        nextNodes.forEach(n => {
          // Settings schema — fetch if not yet cached (drives the settings panel).
          if (n.settings_ready && !nodeSettings.has(n.id)) {
            pendingSettingsFetches++;
            send({ type: "node_settings_get", node_id: n.id });
          }
          // Sensor schema — fetch if not yet cached (drives dashboard reading labels).
          // Rendering is NOT deferred: the dashboard shows a loading placeholder
          // immediately and updates itself when node_sensor_schema arrives.
          if (n.sensor_schema_ready && !nodeSensorSchemas.has(n.id)) {
            send({ type: "node_sensor_schema_get", node_id: n.id });
          }
        });

        renderDashboard();
        renderConnectedNodes();
        refreshNodeOtaUi();
        if (nsCurrentNodeId && $nsOverlay.style.display !== "none" &&
            !isEditingRelayLabel(nsCurrentNodeId)) {
          renderNodeSettings(nsCurrentNodeId);
        }
        updateBadges();
        break;
      case "meta":
        applyMeta(msg);
        break;
      case "discovered":
        discovered.clear();
        (msg.nodes || []).forEach(n => discovered.set(n.mac, n));
        renderAvailable();
        updateBadges();
        break;
      case "pair_timeout":
        pendingMacs.clear();
        renderAvailable();
        showToast("⚠ Pairing timed out — node may have exited pairing mode.", "warn");
        break;
      case "ap_config_ack":
        if (msg.ok) {
          setApSaveNote("✓ Saved — takes effect next time the setup portal runs.", "ok");
          showToast("✓ AP config saved.", "success");
        } else {
          setApSaveNote("✗ " + (msg.err || "Error"), "err");
          showToast("✗ " + (msg.err || "Could not save AP config"), "error");
        }
        $saveApBtn.disabled = false;
        break;
      case "gw_portal_starting":
        showToast("📶 Gateway restarting into WiFi setup portal…", "warn");
        break;
      case "gw_rebooting":
        if (gatewayOtaUploading || $gwOtaProgress?.style.display !== "none") {
          gatewayOtaUploading = false;
          gatewayOtaBusy = true;
          setGatewayOtaProgress(100, "Gateway rebooting into the updated firmware...");
          setGatewayOtaNote("Gateway rebooting into the updated firmware...", "ok");
          refreshGatewayOtaUi();
        }
        showToast("Gateway rebooting...", "warn");
        break;
      case "gw_factory_reset":
        showToast("🗑 Factory reset in progress — gateway restarting…", "warn");
        break;
    }
  });
}

function send(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
}

function setWs(state) {
  $wsPill.className = "ws-pill " + state;
  $wsLabel.textContent =
    state === "live"       ? "Live" :
    state === "error"      ? "Reconnecting..." : "Connecting";
}

// ── Meta ──────────────────────────────────────────────────────────────────────
function applyMeta(m) {
  $mIp.textContent  = m.ip      ?? "—";
  $mMac.textContent = m.mac     ?? "—";
  $mCh.textContent  = m.channel ?? "—";
  serverUptime   = m.uptime ?? 0;
  uptimeSyncedAt = performance.now();
  $mUp.textContent = fmtUptime(serverUptime);
  if (m.fw_version) {
    fwVersion = m.fw_version;
    const sbFw = $("sb-fw");
    if (sbFw) sbFw.textContent = "Firmware v" + fwVersion;
  }
  // Show/hide logout button based on whether credentials are configured
  if (m.credentials_set !== undefined) {
    $logoutBtn.style.display = m.credentials_set ? "" : "none";
  }
  // Populate AP SSID field only when user isn't actively editing it
  if (m.ap_ssid && document.activeElement !== $apSsidInput) {
    $apSsidInput.value = m.ap_ssid;
  }
  // Sync gateway LED toggle state
  if ($gwLedToggle && m.gw_led_enabled !== undefined) {
    // $gwLedToggle.checked = m.gw_led_enabled;
    $gwLedToggle.checked = !!m.gw_led_enabled;
  }
  gatewayOtaSupported = !!m.ota_supported;
  gatewayOtaBusy = !!m.ota_busy;
  gatewayOtaMaxBytes = Number(m.ota_max_bytes || 0);
  gatewayOtaProject = m.ota_project || "";
  nodeOtaBusy = !!m.node_ota_busy;
  nodeOtaHelperOnline = !!m.node_ota_helper_online;
  nodeOtaHost = m.node_ota_host || "192.168.4.1";
  if (!gatewayOtaUploading && !gatewayOtaBusy) {
    resetGatewayOtaProgress();
  }
  refreshGatewayOtaUi();
  refreshNodeOtaUi();
}

// ── Badges ────────────────────────────────────────────────────────────────────
function updateBadges() {
  const nc = nodes.size;
  const dc = discovered.size;
  $nbNodes.textContent = nc;
  $mNd.textContent     = nc;
  if (dc > 0) {
    $nbAvail.textContent = dc;
    $nbAvail.style.display = "";
  } else {
    $nbAvail.style.display = "none";
  }
}

// ── Format helpers ─────────────────────────────────────────────────────────────
function fmtUptime(s) {
  s = Math.floor(s);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sc = s % 60;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${sc}s`;
  return `${sc}s`;
}

function fmtSince(s) {
  if (s < 5)  return "just now";
  if (s < 60) return `${s}s ago`;
  return `${Math.floor(s / 60)}m ago`;
}

function fmtBytes(bytes) {
  const value = Number(bytes || 0);
  if (!Number.isFinite(value) || value <= 0) return "-";
  if (value >= 1024 * 1024) return `${(value / (1024 * 1024)).toFixed(2)} MB`;
  if (value >= 1024) return `${Math.round(value / 1024)} KB`;
  return `${value} B`;
}

function esc(str) {
  return String(str)
    .replace(/&/g, "&amp;").replace(/</g, "&lt;")
    .replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}

function normalizeMac(mac) {
  return String(mac || "").trim().toUpperCase();
}

function defaultRelayLabel(index) {
  return `Relay ${index + 1}`;
}

function sanitizeRelayLabel(label, index) {
  const compact = String(label || "").trim().replace(/\s+/g, " ");
  return compact ? compact.slice(0, 24) : defaultRelayLabel(index);
}

function findNodeIdByMac(mac) {
  const target = normalizeMac(mac);
  if (!target) return 0;
  for (const n of nodes.values()) {
    if (normalizeMac(n.mac) === target) return n.id;
  }
  return 0;
}

function getRelayLabelsForNode(node) {
  const labels = Array.isArray(node?.relay_labels) ? node.relay_labels : [];
  return Array.from({ length: RELAY_CHANNEL_COUNT }, (_, i) => (
    sanitizeRelayLabel(labels[i], i)
  ));
}

function setRelayLabelsOnNode(nodeId, labels) {
  const n = nodes.get(nodeId);
  if (!n) return null;
  const normalized = Array.from({ length: RELAY_CHANNEL_COUNT }, (_, i) => (
    sanitizeRelayLabel(labels[i], i)
  ));
  n.relay_labels = normalized;
  return normalized;
}

function getRelayLabelDraft(nodeId) {
  return relayLabelDrafts.get(nodeId) || null;
}

function setRelayLabelDraft(nodeId, relayIndex, value) {
  const draft = relayLabelDrafts.get(nodeId) || getRelayLabelsForNode(nodes.get(nodeId));
  draft[relayIndex] = value;
  relayLabelDrafts.set(nodeId, draft);
}

function replaceRelayLabelDraft(nodeId, labels) {
  relayLabelDrafts.set(nodeId, Array.from(
    { length: RELAY_CHANNEL_COUNT },
    (_, i) => sanitizeRelayLabel(labels[i], i)
  ));
}

function clearRelayLabelDraft(nodeId) {
  relayLabelDrafts.delete(nodeId);
}

function getActuatorMask(n) {
  return Number(n.actuator_mask ?? n.relay_mask ?? 0);
}

// ── Dashboard ─────────────────────────────────────────────────────────────────
function renderDashboard() {
  const list = [...nodes.values()];

  if (list.length === 0) {
    // Remove all cards
    $nodeGrid.querySelectorAll(".node-card").forEach(c => c.remove());
    $dashEmpty.style.display = "";
    return;
  }

  $dashEmpty.style.display = "none";
  const rendered = new Set();

  list.forEach(n => {
    rendered.add(n.id);
    const existing = $nodeGrid.querySelector(`.node-card[data-id="${n.id}"]`);
    if (existing) {
      patchCard(existing, n);
    } else {
      $nodeGrid.insertAdjacentHTML("beforeend", buildCard(n));
    }
  });

  $nodeGrid.querySelectorAll(".node-card[data-id]").forEach(el => {
    if (!rendered.has(parseInt(el.dataset.id, 10))) el.remove();
  });
}

function buildCard(n) {
  const online  = n.online;
  const typeCls = n.type === 1 ? "sensor" : "relay";
  const typeLbl = n.type === 1 ? "SENSOR"  : "RELAY";
  const body    = n.type === 1 ? buildSensorRows(n) : buildRelayGrid(n);

  return `
  <div class="node-card ${online ? "" : "offline"}" data-id="${n.id}">
    <div class="card-hdr">
      <span class="card-dot ${online ? "online" : "offline"}"></span>
      <span class="card-name">${esc(n.name || `Node #${n.id}`)}</span>
      <span class="card-badge ${typeCls}">${typeLbl}</span>
      <button class="card-info-btn" data-node-id="${n.id}" title="Node info"><span class="icon icon-sm card-info-btn-icon"></span></button>
    </div>
    <div class="card-rows">${body}</div>
  </div>`;
}

// Build sensor card rows entirely from the schema the node self-reported.
// The gateway has no knowledge of what sensors are present — labels, units,
// and precision all come from the SensorDef array received via MSG_SENSOR_SCHEMA.
//
// n.sensor_readings   — array of { id, value } from the latest MSG_SENSOR_DATA
// nodeSensorSchemas   — Map<nodeId, SensorDef[]> populated by MSG_SENSOR_SCHEMA
function buildSensorRows(n) {
  const schema   = nodeSensorSchemas.get(n.id);
  const readings = n.sensor_readings || [];

  // Schema not yet received — show a lightweight loading placeholder.
  // The node_sensor_schema handler will call renderDashboard() when it arrives.
  if (!schema || schema.length === 0) {
    return `
      <div class="card-row sensor-loading">
        <span class="lbl">Sensors</span>
        <span class="val val-muted">waiting for schema…</span>
      </div>
      <div class="card-row">
        <span class="lbl">Uptime</span>
        <span class="val val-uptime">${fmtUptime(n.uptime || 0)}</span>
      </div>`;
  }

  // Build a quick id→value lookup from the readings array.
  const byId = {};
  readings.forEach(r => { byId[r.id] = r.value; });

  // Render one row per sensor using only the schema for labels / units / precision.
  const rows = schema.map(s => {
    const raw = byId[s.id];
    const val = raw != null
      ? `${raw.toFixed(s.precision ?? 1)} ${s.unit}`
      : "—";
    return `
      <div class="card-row">
        <span class="lbl">${esc(s.label)}</span>
        <span class="val">${val}</span>
      </div>`;
  }).join("");

  return rows + `
    <div class="card-row">
      <span class="lbl">Uptime</span>
      <span class="val val-uptime">${fmtUptime(n.uptime || 0)}</span>
    </div>`;
}

function buildRelayGrid(n) {
  const mask = getActuatorMask(n);
  const labels = getRelayLabelsForNode(n);
  const btns = Array.from({ length: RELAY_CHANNEL_COUNT }, (_, i) => {
    const on = !!((mask >> i) & 1);
    return `<button class="relay-btn ${on ? "on" : "off"}"
      data-node="${n.id}" data-relay="${i}" data-state="${on ? "1" : "0"}">
      ${esc(labels[i])}<br>${on ? "● ON" : "○ OFF"}</button>`;
  }).join("");
  return `<div class="relay-grid">${btns}</div>`;
}

function patchCard(el, n) {
  const online = n.online;
  el.className = `node-card ${online ? "" : "offline"}`;
  const dot = el.querySelector(".card-dot");
  if (dot) dot.className = `card-dot ${online ? "online" : "offline"}`;

  const nameEl = el.querySelector(".card-name");
  if (nameEl) nameEl.textContent = n.name || `Node #${n.id}`;

  if (n.type === 1) {
    // Rebuild sensor rows via innerHTML so row count automatically adapts when
    // the sensor schema changes (e.g. a new firmware adds or removes a sensor).
    // Safe: sensor card rows have no inline event listeners (all delegation is
    // handled at the document level via .card-rows, .relay-btn, etc.).
    const rowsEl = el.querySelector(".card-rows");
    if (rowsEl) rowsEl.innerHTML = buildSensorRows(n);
  } else {
    const mask = getActuatorMask(n);
    const labels = getRelayLabelsForNode(n);
    el.querySelectorAll(".relay-btn").forEach(btn => {
      const i  = parseInt(btn.dataset.relay, 10);
      const on = !!((mask >> i) & 1);
      btn.className     = `relay-btn ${on ? "on" : "off"}`;
      btn.dataset.state = on ? "1" : "0";
      btn.innerHTML     = `${esc(labels[i])}<br>${on ? "● ON" : "○ OFF"}`;
    });
  }
}

// ── Connected nodes table ─────────────────────────────────────────────────────
function renderConnectedNodes() {
  const list = [...nodes.values()];

  if (list.length === 0) {
    $nodesEmpty.style.display = "";
    $nodesTbody.querySelectorAll("tr[data-nid]").forEach(r => r.remove());
    return;
  }

  $nodesEmpty.style.display = "none";
  const rendered = new Set();

  list.forEach(n => {
    rendered.add(n.id);
    const existing = $nodesTbody.querySelector(`tr[data-nid="${n.id}"]`);
    if (existing) {
      patchRow(existing, n);
    } else {
      $nodesTbody.insertAdjacentHTML("beforeend", buildRow(n));
    }
  });

  $nodesTbody.querySelectorAll("tr[data-nid]").forEach(row => {
    if (!rendered.has(parseInt(row.dataset.nid, 10))) row.remove();
  });
}

function buildRow(n) {
  const online  = n.online;
  const typeCls = n.type === 1 ? "sensor" : "relay";
  const typeLbl = n.type === 1 ? "Sensor" : "Relay";
  return `
  <tr data-nid="${n.id}">
    <td>
      <div class="tbl-status">
        <span class="tbl-dot ${online ? "online" : "offline"}"></span>
        ${online ? "Online" : "Offline"}
      </div>
    </td>
    <td class="row-name">${esc(n.name || `Node #${n.id}`)}</td>
    <td><span class="type-badge ${typeCls}">${typeLbl}</span></td>
    <td class="tbl-mac">${esc(n.mac || "—")}</td>
    <td class="row-since" data-since="${n.last_seen || 0}">${fmtSince(n.last_seen || 0)}</td>
    <td>
      <div class="tbl-actions">
        <button class="tbl-btn info"     data-node-id="${n.id}" title="Node info"><span class="icon icon-sm tbl-btn-info-icon"></span> Info</button>
        <button class="tbl-btn reboot"   data-node-id="${n.id}"
                title="${online ? "Reboot node" : "Node is offline"}"
                ${online ? "" : "disabled"}><span class="icon icon-sm tbl-btn-reboot-icon"></span> Reboot</button>
        <button class="tbl-btn settings" data-node-id="${n.id}" title="Node settings">
          <span class="icon icon-sm tbl-btn-settings-icon"></span> Settings</button>
        <button class="tbl-btn disc"     data-node-id="${n.id}" title="Disconnect"><span class="icon icon-sm tbl-btn-disc-icon"></span> Disconnect</button>
      </div>
    </td>
  </tr>`;
}

function patchRow(row, n) {
  const online = n.online;
  const dot = row.querySelector(".tbl-dot");
  if (dot) dot.className = `tbl-dot ${online ? "online" : "offline"}`;
  const nameEl = row.querySelector(".row-name");
  if (nameEl) nameEl.textContent = n.name || `Node #${n.id}`;
  const since = row.querySelector(".row-since");
  if (since) {
    since.dataset.since = n.last_seen || 0;
    since.textContent = fmtSince(n.last_seen || 0);
  }
  // Keep reboot button in sync with online state
  const rebootBtn = row.querySelector(".tbl-btn.reboot");
  if (rebootBtn) {
    rebootBtn.disabled = !online;
    rebootBtn.title = online ? "Reboot node" : "Node is offline";
  }
}

// ── Available to pair ─────────────────────────────────────────────────────────
function renderAvailable() {
  const list = [...discovered.values()];

  if (list.length === 0) {
    $availList.querySelectorAll(".avail-card").forEach(c => c.remove());
    $availEmpty.style.display = "";
    return;
  }

  $availEmpty.style.display = "none";
  const rendered = new Set();

  list.forEach(n => {
    rendered.add(n.mac);
    const existing = $availList.querySelector(`.avail-card[data-mac="${n.mac}"]`);
    if (!existing) {
      $availList.insertAdjacentHTML("beforeend", buildAvailCard(n));
    } else {
      // Update connect button state
      const btn = existing.querySelector(".connect-btn");
      if (btn) {
        const pending = pendingMacs.has(n.mac);
        btn.textContent = pending ? "Connecting..." : "Connect";
        btn.classList.toggle("pending", pending);
      }
    }
  });

  $availList.querySelectorAll(".avail-card[data-mac]").forEach(el => {
    if (!rendered.has(el.dataset.mac)) el.remove();
  });
}

function buildAvailCard(n) {
  const typeCls = n.type === 1 ? "sensor" : "relay";
  const typeLbl = n.type === 1 ? "Sensor" : "Relay";
  const pending = pendingMacs.has(n.mac);
  return `
  <div class="avail-card" data-mac="${esc(n.mac)}">
    <div class="avail-info">
      <span class="avail-pulse"></span>
      <span class="avail-name">${esc(n.name || n.mac)}</span>
      <span class="avail-mac">${esc(n.mac)}</span>
      <span class="type-badge ${typeCls}">${typeLbl}</span>
    </div>
    <button class="connect-btn ${pending ? "pending" : ""}"
            data-mac="${esc(n.mac)}">
      ${pending ? "Connecting..." : "Connect"}
    </button>
  </div>`;
}

// ── Info modal ────────────────────────────────────────────────────────────────
function showInfoModal(nodeId) {
  const n = nodes.get(nodeId);
  if (!n) return;

  $modalTitle.textContent = "Node Details";

  const typeLbl = n.type === 1 ? "Sensor" : "Relay";

  $modalBody.innerHTML = `
    <div class="modal-row">
      <span class="mlbl">Name</span>
      <div class="modal-name-wrap">
        <input class="modal-name-input" id="rename-input"
               value="${esc(n.name || "")}" maxlength="15"
               placeholder="Node name">
        <button class="modal-save-btn" id="rename-save" data-node-id="${nodeId}">Save</button>
      </div>
    </div>
    <div class="modal-row">
      <span class="mlbl">Node ID</span>
      <span class="mval">#${n.id}</span>
    </div>
    <div class="modal-row">
      <span class="mlbl">Type</span>
      <span class="mval">${typeLbl}</span>
    </div>
    <div class="modal-row">
      <span class="mlbl">MAC Address</span>
      <span class="mval">${esc(n.mac || "—")}</span>
    </div>
    <div class="modal-row">
      <span class="mlbl">Last Seen</span>
      <span class="mval">${fmtSince(n.last_seen || 0)}</span>
    </div>
    <div class="modal-row">
      <span class="mlbl">Node Uptime</span>
      <span class="mval">${fmtUptime(n.uptime || 0)}</span>
    </div>
    <div class="modal-row">
      <span class="mlbl">Status</span>
      <span class="mval">${n.online ? "Online" : "Offline"}</span>
    </div>
    <div class="modal-row">
      <span class="mlbl">FW Version</span>
      <span class="mval">${n.fw_version ? "v" + esc(n.fw_version) : "—"}</span>
    </div>`;

  $modal.style.display = "flex";
}

function closeModal() {
  $modal.style.display = "none";
}

$("modal-close").addEventListener("click", closeModal);
$modal.addEventListener("click", e => {
  if (e.target === $modal) closeModal();
});

// ── Rename ────────────────────────────────────────────────────────────────────
function sendRename(nodeId, name) {
  name = name.trim().substring(0, 15);
  if (!name) return;
  send({ type: "rename_node", node_id: nodeId, name });
  closeModal();
  showToast(`Node #${nodeId} renamed to "${name}"`, "success");
}

// ── Pair / Unpair ─────────────────────────────────────────────────────────────
function sendPairCmd(mac) {
  if (pendingMacs.has(mac)) return;
  pendingMacs.add(mac);
  send({ type: "pair_cmd", mac });
  renderAvailable();
}

function sendUnpairCmd(nodeId) {
  if (!confirm(`Disconnect node #${nodeId}?`)) return;
  send({ type: "unpair_cmd", node_id: nodeId });
}

// ── Reboot ────────────────────────────────────────────────────────────────────
function sendGatewayReboot() {
  if (!confirm("Reboot the ESP32-S3 Gateway?\nThe dashboard will reconnect automatically.")) return;
  send({ type: "reboot_gw" });
  showToast("Gateway rebooting — reconnecting...", "warn");
  // Disable the button briefly so it can't be clicked again mid-reboot
  $gwReboot.disabled = true;
  setTimeout(() => { $gwReboot.disabled = false; }, 12000);
}

function sendNodeReboot(nodeId) {
  const n = nodes.get(nodeId);
  if (!n) return;
  const label = n.name || `Node #${nodeId}`;
  if (!confirm(`Reboot "${label}"?\nThe node will reconnect automatically.`)) return;
  send({ type: "reboot_node", node_id: nodeId });
  showToast(`Node "${label}" rebooting...`, "warn");
}

// ── Relay ─────────────────────────────────────────────────────────────────────
function sendRelayCmd(nodeId, relayIndex, currentState) {

  const newState = currentState ? 0 : 1;

  send({
    type: "actuator_cmd",
    node_id: nodeId,
    actuator_id: relayIndex,
    state: newState
  });

}

// ── Confirm modal (destructive actions) ───────────────────────────────────────
let _confirmCallback = null;
function showConfirm({ title, body, okLabel = "Confirm", okClass = "", callback }) {
  $confirmTitle.textContent = title;
  $confirmBody.innerHTML    = body;   // supports HTML for bold/line-breaks in warning text
  $confirmOkBtn.textContent = okLabel;
  $confirmOkBtn.className   = "confirm-ok-btn" + (okClass ? " " + okClass : "");
  _confirmCallback          = callback;
  $confirmOverlay.style.display = "";
  $confirmCancelBtn.focus();
}
function closeConfirm() {
  $confirmOverlay.style.display = "none";
  _confirmCallback = null;
}
$confirmCancelBtn.addEventListener("click", closeConfirm);
$confirmOkBtn.addEventListener("click", () => {
  const cb = _confirmCallback;
  closeConfirm();
  if (cb) cb();
});
// Close on backdrop click
$confirmOverlay.addEventListener("click", e => {
  if (e.target === $confirmOverlay) closeConfirm();
});
// Close on Escape
document.addEventListener("keydown", e => {
  if (e.key === "Escape" && $confirmOverlay.style.display !== "none") closeConfirm();
});

// ── AP config save ────────────────────────────────────────────────────────────
function setApSaveNote(msg, type = "") {
  $apSaveNote.textContent = msg;
  $apSaveNote.className   = "setting-save-note" + (type ? " " + type : "");
  if (msg) setTimeout(() => { if ($apSaveNote.textContent === msg) $apSaveNote.textContent = ""; }, 5000);
}

function saveApConfig() {
  const ssid = $apSsidInput.value.trim();
  const pass = $apPassInput.value;           // don't trim — spaces technically valid (bad idea but valid)
  if (ssid.length < 2 || ssid.length > 32) {
    setApSaveNote("✗ SSID must be 2–32 characters.", "err");
    $apSsidInput.focus();
    return;
  }
  if (pass.length > 0 && pass.length < 8) {
    setApSaveNote("✗ Password must be 8+ chars or blank.", "err");
    $apPassInput.focus();
    return;
  }
  $saveApBtn.disabled = true;
  setApSaveNote("Saving…");
  send({ type: "set_ap_config", ssid, password: pass });
}

$saveApBtn.addEventListener("click", saveApConfig);

function setGatewayOtaNote(msg, type = "") {
  if (!$gwOtaNote) return;
  $gwOtaNote.textContent = msg;
  $gwOtaNote.className = "setting-save-note" + (type ? " " + type : "");
}

function setGatewayOtaProgress(percent, text) {
  if (!$gwOtaProgress || !$gwOtaProgressFill || !$gwOtaProgressText) return;
  $gwOtaProgress.style.display = "";
  $gwOtaProgressFill.style.width = `${Math.max(0, Math.min(100, percent))}%`;
  $gwOtaProgressText.textContent = text;
}

function resetGatewayOtaProgress() {
  if (!$gwOtaProgress || !$gwOtaProgressFill || !$gwOtaProgressText) return;
  $gwOtaProgress.style.display = "none";
  $gwOtaProgressFill.style.width = "0%";
  $gwOtaProgressText.textContent = "Idle";
}

function refreshGatewayOtaUi() {
  if ($gwOtaCurrent) $gwOtaCurrent.textContent = fwVersion && fwVersion !== "—" ? `v${fwVersion}` : "v-";
  if ($gwOtaSlot) $gwOtaSlot.textContent = gatewayOtaSupported ? fmtBytes(gatewayOtaMaxBytes) : "Not available";
  if ($gwOtaProject) $gwOtaProject.textContent = gatewayOtaProject || "-";

  if (!$gwOtaBtn || !$gwOtaFileInput) return;

  const hasFile = !!$gwOtaFileInput.files?.length;
  const disabled = gatewayOtaUploading || gatewayOtaBusy || !gatewayOtaSupported || !hasFile;
  $gwOtaBtn.disabled = disabled;
  $gwOtaFileInput.disabled = gatewayOtaUploading || gatewayOtaBusy || !gatewayOtaSupported;

  if (gatewayOtaUploading) {
    $gwOtaBtn.textContent = "Uploading...";
    return;
  }

  $gwOtaBtn.textContent = gatewayOtaBusy ? "OTA Busy" : "Upload Gateway Firmware";

  if (!gatewayOtaSupported) {
    setGatewayOtaNote("OTA is unavailable until the gateway is flashed with the OTA partition layout.", "err");
  } else if (gatewayOtaBusy) {
    setGatewayOtaNote("Another firmware update is already in progress or the gateway is rebooting.", "err");
  } else if (!hasFile) {
    setGatewayOtaNote("");
  }
}

function uploadGatewayFirmwareNow(file) {
  const xhr = new XMLHttpRequest();
  const formData = new FormData();
  formData.append("firmware", file, file.name);

  gatewayOtaUploading = true;
  setGatewayOtaNote("Uploading firmware image...", "");
  setGatewayOtaProgress(0, `Preparing ${file.name} (${fmtBytes(file.size)})`);
  refreshGatewayOtaUi();

  xhr.open("POST", "/api/gateway/ota", true);
  xhr.timeout = 180000;
  if (authToken) xhr.setRequestHeader("X-GW-Token", authToken);
  xhr.setRequestHeader("X-Firmware-Size", String(file.size));

  xhr.upload.addEventListener("progress", event => {
    if (!event.lengthComputable) return;
    const rawPercent = event.total > 0 ? (event.loaded / event.total) : 0;
    const displayPercent = Math.min(96, Math.round(rawPercent * 96));
    const text = rawPercent >= 1
      ? `Upload complete, validating and flashing ${file.name}...`
      : `Uploading ${file.name}: ${Math.round(rawPercent * 100)}%`;
    setGatewayOtaProgress(displayPercent, text);
  });

  xhr.addEventListener("load", () => {
    let response = xhr.response;
    if (!response || typeof response !== "object") {
      try { response = JSON.parse(xhr.responseText || "{}"); } catch { response = {}; }
    }

    if (xhr.status === 200 && response.ok) {
      gatewayOtaBusy = true;
      gatewayOtaUploading = false;
      const flashedVersion = response.version ? ` v${response.version}` : "";
      setGatewayOtaProgress(100, response.message || `Firmware${flashedVersion} update complete. Gateway rebooting...`);
      setGatewayOtaNote(`Firmware${flashedVersion} flashed successfully. Gateway is rebooting...`, "ok");
      showToast(response.message || `Gateway firmware${flashedVersion} update complete. Rebooting...`, "warn");
      $gwOtaFileInput.value = "";
      refreshGatewayOtaUi();
      return;
    }

    gatewayOtaUploading = false;
    const err = response.error || `Upload failed (${xhr.status || "network error"}).`;
    setGatewayOtaProgress(100, "Firmware update failed.");
    setGatewayOtaNote(err, "err");
    showToast(err, "error");
    if (xhr.status === 401) {
      authToken = null;
      showLoginScreen("Session expired. Please log in again.");
    }
    refreshGatewayOtaUi();
  });

  xhr.addEventListener("error", () => {
    gatewayOtaUploading = false;
    setGatewayOtaProgress(100, "Firmware upload failed.");
    setGatewayOtaNote("Network error while uploading firmware.", "err");
    showToast("Network error while uploading firmware.", "error");
    refreshGatewayOtaUi();
  });

  xhr.addEventListener("timeout", () => {
    gatewayOtaUploading = false;
    setGatewayOtaProgress(100, "Firmware upload timed out.");
    setGatewayOtaNote("Upload timed out before the gateway finished flashing.", "err");
    showToast("Firmware upload timed out.", "error");
    refreshGatewayOtaUi();
  });

  xhr.send(formData);
}

function uploadGatewayFirmware() {
  if (!$gwOtaFileInput?.files?.length) {
    setGatewayOtaNote("Select a .bin firmware file first.", "err");
    return;
  }
  if (!gatewayOtaSupported) {
    setGatewayOtaNote("OTA is not available on the current gateway partition layout.", "err");
    return;
  }
  if (gatewayOtaBusy || gatewayOtaUploading) {
    setGatewayOtaNote("Gateway OTA is already busy.", "err");
    return;
  }

  const file = $gwOtaFileInput.files[0];
  if (!/\.bin$/i.test(file.name)) {
    setGatewayOtaNote("Select a compiled .bin firmware file.", "err");
    return;
  }
  if (gatewayOtaMaxBytes > 0 && file.size > gatewayOtaMaxBytes) {
    setGatewayOtaNote(`Firmware is too large for the OTA slot (${fmtBytes(file.size)} > ${fmtBytes(gatewayOtaMaxBytes)}).`, "err");
    return;
  }

  showConfirm({
    title: "Flash Gateway Firmware?",
    body: `You are about to flash <strong>${esc(file.name)}</strong> (${fmtBytes(file.size)}).<br><br>
           The gateway will validate the image, write it to the inactive OTA slot, and then
           <strong>reboot automatically</strong>. Do not power off the gateway during this process.`,
    okLabel: "Upload and Flash",
    okClass: "warn",
    callback: () => uploadGatewayFirmwareNow(file)
  });
}

if ($gwOtaFileInput) {
  $gwOtaFileInput.addEventListener("change", () => {
    if ($gwOtaFileInput.files?.length) {
      const file = $gwOtaFileInput.files[0];
      setGatewayOtaNote(`Ready: ${file.name} (${fmtBytes(file.size)})`, "");
    } else {
      setGatewayOtaNote("");
      resetGatewayOtaProgress();
    }
    refreshGatewayOtaUi();
  });
}

if ($gwOtaBtn) {
  $gwOtaBtn.addEventListener("click", uploadGatewayFirmware);
}

// ── Web interface credentials ─────────────────────────────────────────────────
function setNodeOtaNote(msg, type = "") {
  if (!$nodeOtaNote) return;
  $nodeOtaNote.textContent = msg;
  $nodeOtaNote.className = "setting-save-note" + (type ? " " + type : "");
}

function setNodeOtaProgress(percent, text) {
  if (!$nodeOtaProgress || !$nodeOtaProgressFill || !$nodeOtaProgressText) return;
  $nodeOtaProgress.style.display = "";
  $nodeOtaProgressFill.style.width = `${Math.max(0, Math.min(100, percent))}%`;
  $nodeOtaProgressText.textContent = text;
}

function resetNodeOtaProgress() {
  if (!$nodeOtaProgress || !$nodeOtaProgressFill || !$nodeOtaProgressText) return;
  $nodeOtaProgress.style.display = "none";
  $nodeOtaProgressFill.style.width = "0%";
  $nodeOtaProgressText.textContent = "Idle";
}

function populateNodeOtaTargets() {
  if (!$nodeOtaNode) return;
  const selected = $nodeOtaNode.value;
  const pinnedId = Number(selected || nodeOtaState.node_id || 0);
  const list = [...nodes.values()]
    .filter(n => n.online || n.id === pinnedId)
    .sort((a, b) => (a.name || "").localeCompare(b.name || ""));

  $nodeOtaNode.innerHTML = `<option value="">Select a paired online node</option>`;
  list.forEach(n => {
    const typeLabel = n.type === 1 ? "Sensor" : "Relay";
    const opt = document.createElement("option");
    opt.value = String(n.id);
    opt.textContent = `${n.name || `Node #${n.id}`} (${n.mac}) • ${typeLabel}`;
    $nodeOtaNode.appendChild(opt);
  });

  if (selected && list.some(n => String(n.id) === selected)) {
    $nodeOtaNode.value = selected;
  }
}

function applyNodeOtaState(msg) {
  nodeOtaState = {
    active: !!msg.active,
    node_id: Number(msg.node_id || 0),
    stage: Number(msg.stage || 0),
    progress: Number(msg.progress || 0),
    message: msg.message || "",
    error: msg.error || "",
    version: msg.version || "",
    node_name: msg.node_name || ""
  };
  nodeOtaBusy = !!msg.active || !!msg.upload_busy;
  nodeOtaHelperOnline = msg.helper_online !== undefined ? !!msg.helper_online : nodeOtaHelperOnline;

  if (nodeOtaState.active) {
    const text = nodeOtaState.error || nodeOtaState.message || "Node OTA in progress...";
    setNodeOtaProgress(nodeOtaState.progress, text);
    setNodeOtaNote(text, nodeOtaState.error ? "err" : "ok");
  } else if (!nodeOtaUploading) {
    if (nodeOtaState.error) {
      setNodeOtaProgress(100, nodeOtaState.error);
      setNodeOtaNote(nodeOtaState.error, "err");
    } else if (nodeOtaState.message) {
      setNodeOtaProgress(nodeOtaState.progress || 100, nodeOtaState.message);
      setNodeOtaNote(nodeOtaState.message, "ok");
    } else {
      resetNodeOtaProgress();
    }
  }

  refreshNodeOtaUi();
}

function refreshNodeOtaUi() {
  populateNodeOtaTargets();
  if ($nodeOtaHelper) $nodeOtaHelper.textContent = nodeOtaHelperOnline ? "Online" : "Offline";
  if ($nodeOtaHost) $nodeOtaHost.textContent = nodeOtaHost || "192.168.4.1";

  const selectedId = Number($nodeOtaNode?.value || 0);
  const selectedNode = selectedId ? nodes.get(selectedId) : null;
  if ($nodeOtaTarget) {
    $nodeOtaTarget.textContent = selectedNode
      ? `${selectedNode.name || `Node #${selectedNode.id}`} (${selectedNode.mac})`
      : "-";
  }

  const hasFile = !!$nodeOtaFileInput?.files?.length;
  const disabled = nodeOtaUploading || nodeOtaBusy || !selectedNode || !hasFile;
  if ($nodeOtaBtn) $nodeOtaBtn.disabled = disabled;
  if ($nodeOtaFileInput) $nodeOtaFileInput.disabled = nodeOtaUploading || nodeOtaBusy;
  if ($nodeOtaNode) $nodeOtaNode.disabled = nodeOtaUploading || nodeOtaBusy;

  if ($nodeOtaBtn) {
    $nodeOtaBtn.textContent = nodeOtaUploading
      ? "Uploading..."
      : nodeOtaBusy
        ? "Node OTA Busy"
        : "Upload Node Firmware";
  }

  if (nodeOtaUploading) return;
  if (nodeOtaBusy && !nodeOtaState.error) {
    setNodeOtaNote(nodeOtaState.message || "Node OTA is in progress.", "ok");
  } else if (!selectedNode && !$nodeOtaNode?.value) {
    setNodeOtaNote("");
  }
}

function uploadNodeFirmwareNow(file, nodeId) {
  const xhr = new XMLHttpRequest();
  const formData = new FormData();
  formData.append("firmware", file, file.name);

  nodeOtaUploading = true;
  nodeOtaBusy = true;
  setNodeOtaNote("Uploading node firmware image...", "");
  setNodeOtaProgress(0, `Preparing ${file.name} (${fmtBytes(file.size)})`);
  refreshNodeOtaUi();

  xhr.open("POST", "/api/node/ota", true);
  xhr.timeout = 240000;
  if (authToken) xhr.setRequestHeader("X-GW-Token", authToken);
  xhr.setRequestHeader("X-Firmware-Size", String(file.size));
  xhr.setRequestHeader("X-Node-Id", String(nodeId));

  xhr.upload.addEventListener("progress", event => {
    if (!event.lengthComputable) return;
    const rawPercent = event.total > 0 ? (event.loaded / event.total) : 0;
    const displayPercent = Math.min(25, Math.round(rawPercent * 25));
    setNodeOtaProgress(displayPercent, `Uploading ${file.name}: ${Math.round(rawPercent * 100)}%`);
  });

  xhr.addEventListener("load", () => {
    let response = xhr.response;
    if (!response || typeof response !== "object") {
      try { response = JSON.parse(xhr.responseText || "{}"); } catch { response = {}; }
    }

    nodeOtaUploading = false;
    if (xhr.status === 200 && response.ok) {
      setNodeOtaProgress(25, `Firmware uploaded. Preparing OTA session${response.version ? ` v${response.version}` : ""}...`);
      setNodeOtaNote(`Firmware accepted${response.version ? ` (v${response.version})` : ""}. Waiting for OTA helper...`, "ok");
      showToast("Node firmware uploaded. OTA preparation started.", "warn");
      refreshNodeOtaUi();
      return;
    }

    nodeOtaBusy = false;
    const err = response.error || `Upload failed (${xhr.status || "network error"}).`;
    setNodeOtaProgress(100, err);
    setNodeOtaNote(err, "err");
    showToast(err, "error");
    if (xhr.status === 401) {
      authToken = null;
      showLoginScreen("Session expired. Please log in again.");
    }
    refreshNodeOtaUi();
  });

  xhr.addEventListener("error", () => {
    nodeOtaUploading = false;
    nodeOtaBusy = false;
    setNodeOtaProgress(100, "Node firmware upload failed.");
    setNodeOtaNote("Network error while uploading node firmware.", "err");
    showToast("Network error while uploading node firmware.", "error");
    refreshNodeOtaUi();
  });

  xhr.addEventListener("timeout", () => {
    nodeOtaUploading = false;
    nodeOtaBusy = false;
    setNodeOtaProgress(100, "Node firmware upload timed out.");
    setNodeOtaNote("Upload timed out before the gateway finished staging the node firmware.", "err");
    showToast("Node firmware upload timed out.", "error");
    refreshNodeOtaUi();
  });

  xhr.send(formData);
}

function uploadNodeFirmware() {
  const nodeId = Number($nodeOtaNode?.value || 0);
  const node = nodeId ? nodes.get(nodeId) : null;
  if (!node) {
    setNodeOtaNote("Choose an online paired node first.", "err");
    return;
  }
  if (!$nodeOtaFileInput?.files?.length) {
    setNodeOtaNote("Select a .bin firmware file first.", "err");
    return;
  }
  if (nodeOtaBusy || nodeOtaUploading) {
    setNodeOtaNote("Another node OTA session is already running.", "err");
    return;
  }

  const file = $nodeOtaFileInput.files[0];
  if (!/\.bin$/i.test(file.name)) {
    setNodeOtaNote("Select a compiled .bin firmware file.", "err");
    return;
  }

  showConfirm({
    title: "Flash Node Firmware?",
    body: `You are about to update <strong>${esc(node.name || `Node #${node.id}`)}</strong> with <strong>${esc(file.name)}</strong> (${fmtBytes(file.size)}).<br><br>
           The gateway will validate the firmware, prepare the OTA helper, tell the node to switch into OTA mode, and then wait for the node to reboot and reconnect.`,
    okLabel: "Upload and Update",
    okClass: "warn",
    callback: () => uploadNodeFirmwareNow(file, nodeId)
  });
}

if ($nodeOtaFileInput) {
  $nodeOtaFileInput.addEventListener("change", () => {
    if ($nodeOtaFileInput.files?.length) {
      const file = $nodeOtaFileInput.files[0];
      setNodeOtaNote(`Ready: ${file.name} (${fmtBytes(file.size)})`, "");
    } else {
      setNodeOtaNote("");
      resetNodeOtaProgress();
    }
    refreshNodeOtaUi();
  });
}

if ($nodeOtaNode) {
  $nodeOtaNode.addEventListener("change", refreshNodeOtaUi);
}

if ($nodeOtaBtn) {
  $nodeOtaBtn.addEventListener("click", uploadNodeFirmware);
}

function saveWebCredentials() {
  const user    = ($("web-user-input")    || {}).value?.trim()  || "";
  const pass    = ($("web-pass-input")    || {}).value           || "";
  const confirm = ($("web-pass-confirm")  || {}).value           || "";

  if (user.length < 1 || user.length > 32) {
    setWebCredsSaveNote("✗ Username must be 1–32 characters.", "err");
    return;
  }
  if (pass.length < 4) {
    setWebCredsSaveNote("✗ Password must be at least 4 characters.", "err");
    return;
  }
  if (pass !== confirm) {
    setWebCredsSaveNote("✗ Passwords do not match.", "err");
    return;
  }

  showConfirm({
    title: "Set Web Interface Credentials?",
    body: `You are about to lock the web interface with a password.<br><br>
           <strong>Username:</strong> ${esc(user)}<br><br>
           Once set, the interface will lock immediately and you will need to
           log in with these new credentials. <strong>There is no way to recover
           a forgotten password</strong> — you would need to factory reset the
           Gateway to regain access.`,
    okLabel: "Set Credentials",
    okClass: "confirm-ok-danger",
    callback: () => {
      const btn = $("save-web-creds-btn");
      if (btn) btn.disabled = true;
      setWebCredsSaveNote("Saving…");
      send({ type: "set_web_credentials", username: user, password: pass });
    }
  });
}

const $saveWebCredsBtn = $("save-web-creds-btn");
if ($saveWebCredsBtn) $saveWebCredsBtn.addEventListener("click", saveWebCredentials);

// ── WiFi portal (change home-router network) ──────────────────────────────────
function triggerWifiPortal() {
  showConfirm({
    title: "Change Wi-Fi Network?",
    body: `The gateway will <strong>restart</strong> and open a temporary Wi-Fi hotspot
           (<em>${$apSsidInput.value || "ESP32-Mesh-Setup"}</em>).<br><br>
           Connect your phone or PC to that hotspot and follow the captive portal
           to select your new router. The dashboard will be <strong>temporarily unavailable</strong>
           until the gateway reconnects.`,
    okLabel: "Restart & Open Portal",
    okClass: "warn",
    callback() {
      send({ type: "start_wifi_portal" });
      showToast("📶 Gateway restarting into WiFi setup portal…", "warn");
      $wifiPortalBtn.disabled = true;
    }
  });
}

$wifiPortalBtn.addEventListener("click", triggerWifiPortal);

// ── Factory reset ─────────────────────────────────────────────────────────────
function factoryReset() {
  showConfirm({
    title: "Factory Reset Gateway?",
    body: `<strong>This action is irreversible.</strong><br><br>
           The following will be permanently erased:
           <ul class="confirm-list">
             <li>Saved Wi-Fi / router credentials</li>
             <li>Custom AP name &amp; password</li>
             <li>All paired node assignments</li>
           </ul>
           The gateway will restart into <strong>setup mode</strong>. You will need to
           reconnect it to your router and re-pair your nodes.`,
    okLabel: "Yes, Factory Reset",
    okClass: "danger",
    callback() {
      send({ type: "factory_reset" });
      showToast("🗑 Factory reset initiated — gateway restarting…", "warn");
      $gwFactory.disabled = true;
    }
  });
}

$gwFactory.addEventListener("click", factoryReset);

// ── Node Settings Modal ────────────────────────────────────────────────────────
function openNodeSettings(nodeId) {
  const n = nodes.get(nodeId);
  if (!n) return;

  nsCurrentNodeId = nodeId;
  $nsTitle.textContent = `${esc(n.name || `Node #${nodeId}`)} — Settings`;
  $nsOverlay.style.display = "flex";

  const schema = nodeSettings.get(nodeId);
  if (!schema) {
    // Show spinner and request schema
    $nsBody.innerHTML = `<div class="nsettings-spinner"><span class="spin-ring"></span>Loading settings…</div>`;
    send({ type: "node_settings_get", node_id: nodeId });
  } else {
    renderNodeSettings(nodeId);
  }
}

function openRelayAwareNodeSettings(nodeId) {
  const n = nodes.get(nodeId);
  if (!n) return;

  nsCurrentNodeId = nodeId;
  $nsTitle.textContent = `${n.name || `Node #${nodeId}`} - Settings`;
  $nsOverlay.style.display = "flex";
  renderNodeSettings(nodeId);

  if (!nodeSettings.has(nodeId)) {
    send({ type: "node_settings_get", node_id: nodeId });
  }
}

function closeNodeSettings() {
  if (nsCurrentNodeId) clearRelayLabelDraft(nsCurrentNodeId);
  $nsOverlay.style.display = "none";
  nsCurrentNodeId = 0;
}

$("nsettings-close").addEventListener("click", closeNodeSettings);
$nsOverlay.addEventListener("click", e => {
  if (e.target === $nsOverlay) closeNodeSettings();
});

function captureNodeSettingsFocus(nodeId) {
  const active = document.activeElement;
  const relayInput = active?.closest?.(".relay-label-input");
  if (!relayInput) return null;
  if (parseInt(relayInput.dataset.node, 10) !== nodeId) return null;

  return {
    relayIndex: parseInt(relayInput.dataset.relay, 10),
    selectionStart: relayInput.selectionStart,
    selectionEnd: relayInput.selectionEnd
  };
}

function restoreNodeSettingsFocus(nodeId, snapshot) {
  if (!snapshot) return;
  const input = $nsBody.querySelector(
    `.relay-label-input[data-node="${nodeId}"][data-relay="${snapshot.relayIndex}"]`
  );
  if (!input) return;

  input.focus({ preventScroll: true });
  if (typeof snapshot.selectionStart === "number" && typeof input.setSelectionRange === "function") {
    input.setSelectionRange(snapshot.selectionStart, snapshot.selectionEnd ?? snapshot.selectionStart);
  }
}

function isEditingRelayLabel(nodeId) {
  const active = document.activeElement;
  const relayInput = active?.closest?.(".relay-label-input");
  if (!relayInput) return false;
  return parseInt(relayInput.dataset.node, 10) === nodeId;
}

function renderNodeSettings(nodeId) {
  const n      = nodes.get(nodeId);
  const schema = nodeSettings.get(nodeId);

  if (!n) { closeNodeSettings(); return; }

  const focusSnapshot = captureNodeSettingsFocus(nodeId);
  const prevScrollTop = $nsBody.scrollTop;

  // Update title in case node was renamed
  $nsTitle.textContent = `${esc(n.name || `Node #${nodeId}`)} — Settings`;

  // Offline notice (non-blocking — user can still see current values)
  const offlineHtml = n.online ? "" : `
    <div class="nsettings-offline">
      ⚠ Node is offline — firmware setting changes will apply after it reconnects.
    </div>`;

  const sections = [];

  if (n.type !== 1) sections.push(buildRelayLabelSection(n));

  if (!schema) {
    sections.push(`
      <div class="nsettings-section">
        <div class="nsettings-section-hdr">Node Settings</div>
        <div class="nsettings-empty">
          <div class="nsettings-spinner"><span class="spin-ring"></span>Waiting for node…</div>
        </div>
      </div>`);
  } else if (schema.length > 0) {
    const rows = schema.map(s => buildSettingRow(s, nodeId, n.online)).join("");
    sections.push(`
      <div class="nsettings-section">
        <div class="nsettings-section-hdr">Node Settings</div>
        ${rows}
      </div>`);
  } else if (n.type === 1) {
    sections.push(`
      <div class="nsettings-section">
        <div class="nsettings-section-hdr">Node Settings</div>
        <div class="nsettings-empty">This node has no configurable settings.</div>
      </div>`);
  } else {
    sections.push(`
      <div class="nsettings-section">
        <div class="nsettings-section-hdr">Node Settings</div>
        <div class="nsettings-empty">This relay node does not expose any firmware settings.</div>
      </div>`);
  }

  $nsBody.innerHTML = offlineHtml + sections.join("");
  $nsBody.scrollTop = prevScrollTop;
  restoreNodeSettingsFocus(nodeId, focusSnapshot);
}

function buildSettingRow(s, nodeId, online) {
  const typeLabels = ["Toggle", "Select", "Number"];
  const typeLabel  = typeLabels[s.type] ?? "Setting";
  let control = "";

  if (s.type === 0) {
    // BOOL — toggle switch
    const checked = s.current ? "checked" : "";
    control = `
      <label class="ns-toggle" data-node="${nodeId}" data-sid="${s.id}" data-type="0">
        <input type="checkbox" ${checked} ${online ? "" : "disabled"}>
        <span class="ns-toggle-track"></span>
        <span class="ns-toggle-thumb"></span>
      </label>`;
  } else if (s.type === 1) {
    // ENUM — select
    const opts = (s.opts || []).slice(0, s.opt_count)
      .map((o, i) => `<option value="${i}" ${i === s.current ? "selected" : ""}>${esc(o)}</option>`)
      .join("");
    control = `<select class="ns-select" data-node="${nodeId}" data-sid="${s.id}" data-type="1"
                       ${online ? "" : "disabled"}>${opts}</select>`;
  } else if (s.type === 2) {
    // INT — stepper
    const atMin = s.current <= s.i_min;
    const atMax = s.current >= s.i_max;
    control = `
      <div class="ns-stepper">
        <button class="ns-stepper-btn" data-node="${nodeId}" data-sid="${s.id}" data-dir="-1"
                data-min="${s.i_min}" data-max="${s.i_max}" data-step="${s.i_step || 1}"
                ${atMin || !online ? "disabled" : ""}>−</button>
        <span class="ns-stepper-val" id="ns-val-${nodeId}-${s.id}">${s.current}${s.id === 1 ? "s" : ""}</span>
        <button class="ns-stepper-btn" data-node="${nodeId}" data-sid="${s.id}" data-dir="1"
                data-min="${s.i_min}" data-max="${s.i_max}" data-step="${s.i_step || 1}"
                ${atMax || !online ? "disabled" : ""}>+</button>
      </div>`;
  }

  return `
    <div class="nsetting-row">
      <div class="nsetting-info">
        <div class="nsetting-label">${esc(s.label)}</div>
        <div class="nsetting-type">${typeLabel}</div>
      </div>
      ${control}
    </div>`;
}

function buildRelayLabelSection(n) {
  const labels = getRelayLabelDraft(n.id) || getRelayLabelsForNode(n);
  const rows = labels.map((label, i) => `
    <div class="nsetting-row relay-label-row">
      <div class="nsetting-info">
        <div class="nsetting-label">Relay ${i + 1} Label</div>
        <div class="nsetting-type">Stored on gateway for MAC ${esc(n.mac || "—")}</div>
      </div>
      <input type="text"
             class="setting-input relay-label-input"
             data-node="${n.id}"
             data-relay="${i}"
             value="${esc(label)}"
             maxlength="24"
             placeholder="${defaultRelayLabel(i)}">
    </div>`).join("");

  return `
    <div class="nsettings-section">
      <div class="nsettings-section-hdr">Relay Labels</div>
      <div class="nsettings-section-desc">
        These labels are stored in the gateway and pinned to this node's MAC address, so they stay
        with the correct relay node after reboots and reconnects.
      </div>
      ${rows}
      <div class="setting-save-row relay-label-actions">
        <button class="settings-save-btn relay-label-save-btn" data-node-id="${n.id}" type="button">
          Save Labels
        </button>
        <button class="tbl-btn relay-label-reset-btn" data-node-id="${n.id}" type="button">
          Reset Defaults
        </button>
        <span class="setting-save-note" id="relay-label-note-${n.id}"></span>
      </div>
    </div>`;
}

function setRelayLabelSaveNote(nodeId, msg, type = "") {
  const el = document.getElementById(`relay-label-note-${nodeId}`);
  if (!el) return;
  el.textContent = msg;
  el.className = "setting-save-note" + (type ? " " + type : "");
}

function collectRelayLabelInputs(nodeId) {
  return [...document.querySelectorAll(`.relay-label-input[data-node="${nodeId}"]`)];
}

function saveRelayLabels(nodeId) {
  const n = nodes.get(nodeId);
  if (!n || n.type === 1) return;

  const inputs = collectRelayLabelInputs(nodeId);
  if (inputs.length === 0) return;

  const labels = inputs.map((input, index) => sanitizeRelayLabel(input.value, index));
  replaceRelayLabelDraft(nodeId, labels);
  inputs.forEach((input, index) => { input.value = labels[index]; });
  setRelayLabelSaveNote(nodeId, "Saving to gateway...", "");
  send({ type: "relay_labels_set", node_id: nodeId, labels });
}

function resetRelayLabels(nodeId) {
  const n = nodes.get(nodeId);
  if (!n || n.type === 1) return;

  const defaults = Array.from({ length: RELAY_CHANNEL_COUNT }, (_, i) => defaultRelayLabel(i));
  replaceRelayLabelDraft(nodeId, defaults);

  collectRelayLabelInputs(nodeId).forEach((input, index) => {
    input.value = defaults[index];
  });
  setRelayLabelSaveNote(nodeId, "Saving default labels to gateway...", "");
  send({ type: "relay_labels_set", node_id: nodeId, labels: defaults });
}

// ── Toast ─────────────────────────────────────────────────────────────────────
let toastTimer = null;
function showToast(msg, type = "success") {
  $toast.textContent = msg;
  $toast.className = `toast ${type}`;
  $toast.style.display = "";
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { $toast.style.display = "none"; }, 4000);
}

$gwReboot.addEventListener("click", sendGatewayReboot);

// ── Login form ────────────────────────────────────────────────────────────────
$("login-btn").addEventListener("click", doLogin);

["login-username", "login-password"].forEach(id => {
  $(id).addEventListener("keydown", e => { if (e.key === "Enter") doLogin(); });
});

// ── Logout button ─────────────────────────────────────────────────────────────
$logoutBtn.addEventListener("click", () => {
  doLogout();
});

// ── Delegated events ──────────────────────────────────────────────────────────
document.addEventListener("click", e => {
  // Node settings stepper buttons
  const stepBtn = e.target.closest(".ns-stepper-btn");
  if (stepBtn && !stepBtn.disabled) {
    const nodeId  = parseInt(stepBtn.dataset.node, 10);
    const sid     = parseInt(stepBtn.dataset.sid, 10);
    const dir     = parseInt(stepBtn.dataset.dir, 10);
    const step    = parseInt(stepBtn.dataset.step, 10) || 1;
    const min     = parseInt(stepBtn.dataset.min, 10);
    const max     = parseInt(stepBtn.dataset.max, 10);

    const schema  = nodeSettings.get(nodeId);
    if (!schema) return;
    const setting = schema.find(s => s.id === sid);
    if (!setting) return;

    const newVal  = Math.max(min, Math.min(max, setting.current + dir * step));
    if (newVal === setting.current) return;

    setting.current = newVal;
    // Optimistic UI update
    const valEl = document.getElementById(`ns-val-${nodeId}-${sid}`);
    if (valEl) valEl.textContent = newVal + (sid === 1 ? "s" : "");
    // Disable buttons at limits
    const parent = stepBtn.closest(".ns-stepper");
    if (parent) {
      parent.querySelectorAll(".ns-stepper-btn").forEach(b => {
        const d = parseInt(b.dataset.dir, 10);
        b.disabled = (d === -1 && newVal <= min) || (d === 1 && newVal >= max);
      });
    }
    send({ type: "node_settings_set", node_id: nodeId, setting_id: sid, value: newVal });
    return;
  }

  // Relay buttons
  const relayBtn = e.target.closest(".relay-btn");
  if (relayBtn && !relayBtn.disabled) {
    sendRelayCmd(
      parseInt(relayBtn.dataset.node,  10),
      parseInt(relayBtn.dataset.relay, 10),
      parseInt(relayBtn.dataset.state, 10)
    );
    return;
  }

  const relaySaveBtn = e.target.closest(".relay-label-save-btn");
  if (relaySaveBtn) {
    saveRelayLabels(parseInt(relaySaveBtn.dataset.nodeId, 10));
    return;
  }

  const relayResetBtn = e.target.closest(".relay-label-reset-btn");
  if (relayResetBtn) {
    resetRelayLabels(parseInt(relayResetBtn.dataset.nodeId, 10));
    return;
  }

  // Card info button
  const cardInfo = e.target.closest(".card-info-btn");
  if (cardInfo) {
    showInfoModal(parseInt(cardInfo.dataset.nodeId, 10));
    return;
  }

  // Connect button (available list)
  const connectBtn = e.target.closest(".connect-btn");
  if (connectBtn && !connectBtn.classList.contains("pending")) {
    sendPairCmd(connectBtn.dataset.mac);
    return;
  }

  // Table: settings button
  const tblSettings = e.target.closest(".tbl-btn.settings");
  if (tblSettings) {
    openRelayAwareNodeSettings(parseInt(tblSettings.dataset.nodeId, 10));
    return;
  }

  // Table: info button
  const tblInfo = e.target.closest(".tbl-btn.info");
  if (tblInfo) {
    showInfoModal(parseInt(tblInfo.dataset.nodeId, 10));
    return;
  }

  // Table: reboot button
  const tblReboot = e.target.closest(".tbl-btn.reboot");
  if (tblReboot) {
    sendNodeReboot(parseInt(tblReboot.dataset.nodeId, 10));
    return;
  }

  // Table: disconnect button
  const tblDisc = e.target.closest(".tbl-btn.disc");
  if (tblDisc) {
    sendUnpairCmd(parseInt(tblDisc.dataset.nodeId, 10));
    return;
  }

  // Modal: save rename
  const savBtn = e.target.closest("#rename-save");
  if (savBtn) {
    const inp = $("rename-input");
    if (inp) sendRename(parseInt(savBtn.dataset.nodeId, 10), inp.value);
    return;
  }
});

// Enter key in rename input
document.addEventListener("keydown", e => {
  const relayInput = e.target.closest(".relay-label-input");
  if (e.key === "Enter" && relayInput) {
    e.preventDefault();
    saveRelayLabels(parseInt(relayInput.dataset.node, 10));
    return;
  }

  if (e.key === "Enter") {
    const inp = $("rename-input");
    const sav = $("rename-save");
    if (inp && sav && document.activeElement === inp) {
      sendRename(parseInt(sav.dataset.nodeId, 10), inp.value);
    }
  }
  if (e.key === "Escape") {
    closeModal();
    closeNodeSettings();
  }
});

// Settings modal: toggle and select controls
document.addEventListener("change", e => {
  // Bool toggle
  const toggle = e.target.closest(".ns-toggle input");
  if (toggle) {
    const wrap    = toggle.closest(".ns-toggle");
    const nodeId  = parseInt(wrap.dataset.node, 10);
    const sid     = parseInt(wrap.dataset.sid, 10);
    const newVal  = toggle.checked ? 1 : 0;
    const schema  = nodeSettings.get(nodeId);
    if (schema) {
      const s = schema.find(s => s.id === sid);
      if (s) s.current = newVal;
    }
    send({ type: "node_settings_set", node_id: nodeId, setting_id: sid, value: newVal });
    return;
  }

  // Enum select
  const sel = e.target.closest(".ns-select");
  if (sel) {
    const nodeId  = parseInt(sel.dataset.node, 10);
    const sid     = parseInt(sel.dataset.sid, 10);
    const newVal  = parseInt(sel.value, 10);
    const schema  = nodeSettings.get(nodeId);
    if (schema) {
      const s = schema.find(s => s.id === sid);
      if (s) s.current = newVal;
    }
    send({ type: "node_settings_set", node_id: nodeId, setting_id: sid, value: newVal });
    return;
  }
});

document.addEventListener("input", e => {
  const relayInput = e.target.closest(".relay-label-input");
  if (!relayInput) return;
  setRelayLabelDraft(
    parseInt(relayInput.dataset.node, 10),
    parseInt(relayInput.dataset.relay, 10),
    relayInput.value
  );
  setRelayLabelSaveNote(parseInt(relayInput.dataset.node, 10), "");
});

// Gateway Status LED toggle functionality
if ($gwLedToggle) {
  $gwLedToggle.addEventListener("change", () => {

    const enabled = $gwLedToggle.checked;

    send({
      type: "gw_led_toggle",
      state: enabled ? 1 : 0
    });

  });
}

// ── Timers ────────────────────────────────────────────────────────────────────
// Gateway uptime interpolation
setInterval(() => {
  const elapsed = (performance.now() - uptimeSyncedAt) / 1000;
  $mUp.textContent = fmtUptime(Math.floor(serverUptime + elapsed));
}, 1000);

// Last-seen counters
setInterval(() => {
  document.querySelectorAll(".row-since[data-since]").forEach(el => {
    const s = parseInt(el.dataset.since, 10) + 5;
    el.dataset.since = s;
    el.textContent   = fmtSince(s);
  });
}, 5000);

// Keep-alive ping
setInterval(() => send({ type: "ping" }), 20000);

// ── Boot ──────────────────────────────────────────────────────────────────────
initAuth();
