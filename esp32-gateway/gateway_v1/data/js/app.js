/**
 * ESP32 Mesh Gateway Web Interface Client v4.8.1
 *
 * WS Inbound:  { type:"meta"|"update"|"discovered"|"pair_timeout"|"pair_capacity_full"|"ap_config_ack"|
 *                     "offline_ap_config_ack"|
 *                     "gw_portal_starting"|"gw_factory_reset"|"gw_rebooting"|
 *                     "gw_network_notice"|
 *                     "auth_required"|"auth_ok"|"auth_fail"|"session_expired"|
 *                     "coproc_ota_state"|"mqtt_config_ack"|
 *                     "web_creds_ack"|"gw_builtin_sensor_settings_ack"|"node_settings"|"node_sensor_schema"|
 *                     "node_actuator_schema"|"node_rfid_config"|
 *                     "node_rfid_config_ack"|"rfid_scan_event"|
 *                     "relay_labels_ack" }
 * WS Outbound: { type:"auth"|"actuator_cmd"|"pair_cmd"|"unpair_cmd"|"rename_node"|
 *                     "reboot_gw"|"reboot_node"|"set_ap_config"|"set_offline_ap_config"|"set_web_credentials"|
 *                     "start_wifi_portal"|"factory_reset"|"gw_builtin_sensor_settings_set"|"mqtt_config_set"|"node_settings_get"|
 *                     "node_settings_set"|"node_sensor_schema_get"|
 *                     "node_actuator_schema_get"|"node_rfid_config_get"|
 *                     "node_rfid_config_set"|"relay_labels_set"|"ping" }
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
const nodeActuatorSchemas = new Map(); // nodeId → ActuatorDef[] (schema from node)
const nodeRfidConfigs = new Map(); // nodeId → RFID config snapshot
const rfidEditorState = new Map(); // nodeId → { selectedSlot, uid, relayMask }
const RELAY_CHANNEL_COUNT     = 4;
const RFID_SLOT_COUNT         = 8;
let   ws         = null;
let   serverUptime   = 0;
let   uptimeSyncedAt = 0;
const pendingMacs    = new Set();
let   currentSection = "dashboard";
let   fwVersion      = "—";

// ── DOM ───────────────────────────────────────────────────────────────────────
const $ = id => document.getElementById(id);
const $gwBuiltinSensorGrid = $("gw-builtin-sensor-grid");
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
const $gwRebootButtons = [$("gw-reboot-btn"), $("gw-reboot-btn-mobile")].filter(Boolean);
const $gwFactoryButtons = [$("gw-factory-btn"), $("gw-factory-btn-mobile")].filter(Boolean);
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
const $offlineSsidInput = $("offline-ssid-input");
const $offlinePassInput = $("offline-pass-input");
const $saveOfflineBtn = $("save-offline-btn");
const $offlineSaveNote = $("offline-save-note");
const $wifiPortalBtn = $("wifi-portal-btn");
const $gwNetMode = $("gw-net-mode");
const $gwAccessIp = $("gw-access-ip");
const $gwRouterLink = $("gw-router-link");
const $gwBuiltinSensorSettingsCard = $("gw-builtin-sensor-settings-card");
const $gwBuiltinSensorStatus = $("gw-builtin-sensor-status");
const $gwBuiltinSensorUnitView = $("gw-builtin-sensor-unit-view");
const $gwBuiltinSensorAltView = $("gw-builtin-sensor-alt-view");
const $gwBuiltinTempUnit = $("gw-builtin-temp-unit");
const $gwBuiltinAltitudeValue = $("gw-builtin-altitude-value");
const $gwBuiltinAltitudeUnit = $("gw-builtin-altitude-unit");
const $gwBuiltinSensorSaveBtn = $("gw-builtin-sensor-save-btn");
const $gwBuiltinSensorSaveNote = $("gw-builtin-sensor-save-note");
const $mqttSettingsCard = $("mqtt-settings-card");
const $mqttStatus = $("mqtt-status");
const $mqttPasswordStatus = $("mqtt-password-status");
const $mqttLastError = $("mqtt-last-error");
const $mqttEnabledToggle = $("mqtt-enabled-toggle");
const $mqttHostInput = $("mqtt-host-input");
const $mqttPortInput = $("mqtt-port-input");
const $mqttBaseTopicInput = $("mqtt-base-topic-input");
const $mqttUserInput = $("mqtt-user-input");
const $mqttPassInput = $("mqtt-pass-input");
const $mqttSaveBtn = $("save-mqtt-btn");
const $mqttSaveNote = $("mqtt-save-note");
const $gwOtaTarget = $("gw-ota-target");
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
let gatewayOtaTarget = "main";
const GW_NETWORK_SNAPSHOT_KEY = "gwNetworkSnapshot";
let gatewayNetworkMode = "Online (Router)";
let gatewayAccessIp = "-";
let gatewayRouterOnline = false;
let gatewayOfflineApActive = false;
let gatewayOfflineApSsid = "";
let gatewayRouterSsid = "";
let gatewayBuiltinSensor = {
  enabled: false,
  model: "disabled",
  present: false,
  tempUnit: "C",
  altitudeConfigured: false,
  altitudeValue: 0,
  altitudeUnit: "m",
  temperature: null,
  pressureHpa: null,
  humidity: null
};
let gatewayBuiltinSensorDraft = {
  dirty: false,
  saving: false,
  tempUnit: "C",
  altitudeValueText: "",
  altitudeUnit: "m"
};
let gatewayMqtt = {
  enabled: false,
  connected: false,
  host: "",
  port: 1883,
  baseTopic: "",
  username: "",
  hasPassword: false,
  lastError: ""
};
let gatewayMqttDraft = {
  dirty: false,
  saving: false,
  enabled: false,
  host: "",
  portText: "1883",
  baseTopic: "",
  username: "",
  password: ""
};
const COPROC_BUSY_MSG = "Coprocessor Busy. Please try after some time.";
let coprocOnline = false;
let coprocFwVersion = "";
let coprocOtaSupported = false;
let coprocOtaBusy = false;
let coprocOtaMaxBytes = 0;
let coprocOtaProject = "";
let coprocOtaState = {
  active: false,
  stage: 0,
  progress: 0,
  message: "",
  error: "",
  version: "",
  current_version: ""
};
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
let sidebarScrollLockY = 0;

function lockPageScroll() {
  if (document.body.classList.contains("sidebar-open")) return;
  sidebarScrollLockY = window.scrollY || window.pageYOffset || 0;
  document.body.classList.add("sidebar-open");
  document.body.style.top = `-${sidebarScrollLockY}px`;
}

function unlockPageScroll() {
  if (!document.body.classList.contains("sidebar-open")) return;
  document.body.classList.remove("sidebar-open");
  document.body.style.top = "";
  window.scrollTo(0, sidebarScrollLockY);
}

function openSidebar() {
  document.getElementById("sidebar").classList.add("open");
  document.getElementById("sb-overlay").classList.add("open");
  lockPageScroll();
}

function closeSidebar() {
  document.getElementById("sidebar").classList.remove("open");
  document.getElementById("sb-overlay").classList.remove("open");
  unlockPageScroll();
}

function toggleSidebar() {
  const sidebar = document.getElementById("sidebar");
  if (sidebar.classList.contains("open")) {
    closeSidebar();
    return;
  }
  openSidebar();
}

function navigate(section) {
  currentSection = section;

  document.querySelectorAll(".sb-btn").forEach(btn => {
    btn.classList.toggle("active", btn.dataset.section === section);
  });
  document.querySelectorAll(".sec").forEach(sec => {
    sec.classList.toggle("active", sec.id === "sec-" + section);
  });

  // Close mobile sidebar
  closeSidebar();
}

document.querySelectorAll(".sb-btn[data-section]").forEach(btn => {
  btn.addEventListener("click", () => navigate(btn.dataset.section));
});

// Mobile menu toggle
$("menu-btn").addEventListener("click", e => {
  e.preventDefault();
  e.stopPropagation();
  toggleSidebar();
});

const $sidebarCloseBtn = $("sidebar-close-btn");
if ($sidebarCloseBtn) {
  $sidebarCloseBtn.addEventListener("click", e => {
    e.preventDefault();
    e.stopPropagation();
    closeSidebar();
  });
}

$("sb-overlay").addEventListener("click", closeSidebar);

window.addEventListener("resize", () => {
  if (getComputedStyle($("menu-btn")).display === "none") {
    closeSidebar();
  }
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
let pageUnloading = false;
let wsHadLiveConnection = false;
let wsDisconnectNotified = false;
let suppressNextWsCloseToast = false;

function loadGatewayNetworkSnapshot() {
  try {
    const raw = localStorage.getItem(GW_NETWORK_SNAPSHOT_KEY);
    if (!raw) return null;
    const parsed = JSON.parse(raw);
    return parsed && typeof parsed === "object" ? parsed : null;
  } catch {
    return null;
  }
}

function saveGatewayNetworkSnapshot() {
  try {
    localStorage.setItem(GW_NETWORK_SNAPSHOT_KEY, JSON.stringify({
      network_mode: gatewayNetworkMode,
      access_ip: gatewayAccessIp,
      offline_ap_ssid: gatewayOfflineApSsid,
      router_ssid: gatewayRouterSsid
    }));
  } catch {}
}

function showGatewayTransitionToast(previous, next) {
  if (!previous || !next) return;
  if (previous.network_mode === next.network_mode &&
      previous.access_ip === next.access_ip) {
    return;
  }

  if (next.network_mode === "Offline AP Active") {
    const ssid = next.offline_ap_ssid || gatewayOfflineApSsid || "the gateway offline AP";
    const ip = next.access_ip || "192.168.8.1";
    showToast(`Gateway switched to Offline mode. Connect to "${ssid}" and open http://${ip}/.`, "warn", 10000);
    return;
  }

  if (next.network_mode === "Online (Router)") {
    const router = next.router_ssid || gatewayRouterSsid || "your router";
    const ip = next.access_ip || "";
    const suffix = ip && ip !== "-" ? ` and open http://${ip}/.` : ".";
    showToast(`Gateway is back Online. Reconnect to "${router}"${suffix}`, "success", 10000);
  }
}

function handleGatewayNetworkNotice(msg) {
  const event = msg?.event || "";
  const message = msg?.message || "";
  if (event === "router_lost" || event === "switching_online") {
    wsDisconnectNotified = true;
  }
  if (message) {
    showToast(message, event === "switching_online" ? "success" : "warn", 10000);
    return;
  }

  if (event === "router_visible") {
    const router = msg.router_ssid || gatewayRouterSsid || "saved router";
    showToast(`Router "${router}" found. Gateway is reconnecting now...`, "warn", 8000);
  }
}

function handleGatewayWsClose() {
  if (pageUnloading) return;

  if (gatewayNetworkMode === "Online (Router)" && gatewayOfflineApSsid) {
    showToast(
      `Gateway connection lost. If it switched to Offline mode, connect to "${gatewayOfflineApSsid}" and open http://192.168.8.1/.`,
      "warn",
      12000
    );
    return;
  }

  if (gatewayNetworkMode === "Offline AP Active") {
    const router = gatewayRouterSsid || "your router";
    showToast(
      `Gateway connection lost. If it returned Online, reconnect to "${router}" and reopen the gateway dashboard.`,
      "warn",
      12000
    );
    return;
  }

  showToast("Gateway connection lost. Attempting to reconnect...", "warn", 8000);
}

window.addEventListener("beforeunload", () => {
  pageUnloading = true;
});

function connect() {
  setWs("connecting");
  ws = new WebSocket(`ws://${location.host}/ws`);

  ws.addEventListener("open", () => {
    setWs("live");
    authSentThisConnection = false;
    wsHadLiveConnection = true;
    wsDisconnectNotified = false;
    // Send token proactively so auth completes before the server's auth_required arrives.
    if (authToken) {
      send({ type: "auth", token: authToken });
      authSentThisConnection = true;
    }
  });
  ws.addEventListener("close", () => {
    setWs("error");
    authSentThisConnection = false;
    if (suppressNextWsCloseToast) {
      suppressNextWsCloseToast = false;
    } else if (wsHadLiveConnection && !wsDisconnectNotified) {
      wsDisconnectNotified = true;
      handleGatewayWsClose();
    }
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
      case "gw_builtin_sensor_settings_ack":
        if (msg.ok) {
          syncGatewayBuiltinSensorDraftFromMeta(true);
          setGatewayBuiltinSensorSaveNote("Saved. Sensor settings updated.", "ok");
          showToast("Gateway built-in sensor settings saved.", "success");
          renderGatewayBuiltinSensorSettings();
        } else {
          gatewayBuiltinSensorDraft.saving = false;
          setGatewayBuiltinSensorSaveNote(msg.err || "Could not save sensor settings.", "err");
          showToast(msg.err || "Could not save sensor settings.", "error");
        }
        if ($gwBuiltinSensorSaveBtn) $gwBuiltinSensorSaveBtn.disabled = false;
        break;
      case "mqtt_config_ack":
        if (msg.ok) {
          gatewayMqtt.enabled = gatewayMqttDraft.enabled;
          gatewayMqtt.host = gatewayMqttDraft.host;
          gatewayMqtt.port = Number(gatewayMqttDraft.portText || gatewayMqtt.port || 1883);
          gatewayMqtt.baseTopic = gatewayMqttDraft.baseTopic;
          gatewayMqtt.username = gatewayMqttDraft.username;
          if (gatewayMqttDraft.password) gatewayMqtt.hasPassword = true;
          gatewayMqttDraft.password = "";
          syncGatewayMqttDraftFromMeta(true);
          setMqttSaveNote("Saved. MQTT settings updated.", "ok");
          showToast("MQTT settings saved.", "success");
          renderMqttSettings();
        } else {
          gatewayMqttDraft.saving = false;
          if ($mqttSaveBtn) $mqttSaveBtn.disabled = false;
          setMqttSaveNote(msg.err || "Could not save MQTT settings.", "err");
          showToast(msg.err || "Could not save MQTT settings.", "error");
        }
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
      case "node_actuator_schema":
        nodeActuatorSchemas.set(msg.node_id, msg.actuators || []);
        renderDashboard();
        renderConnectedNodes();
        if (nsCurrentNodeId === msg.node_id) renderNodeSettings(msg.node_id);
        break;
      case "node_rfid_config": {
        const nodeId = Number(msg.node_id || 0);
        nodeRfidConfigs.set(nodeId, {
          ready: !!msg.ready,
          actuator_count: Number(msg.actuator_count || 0),
          slots: Array.isArray(msg.slots) ? msg.slots : [],
          last_scan: (msg.last_uid || msg.last_uid_len)
            ? {
                uid: normalizeRfidUidInput(msg.last_uid || ""),
                matched_slot: Number(msg.last_matched_slot ?? -1),
                relay_mask: Number(msg.last_relay_mask || 0),
                seen_ms: Number(msg.last_seen_ms || 0)
              }
            : null
        });
        if (!rfidEditorState.has(nodeId)) {
          loadRfidEditorSlot(nodeId, findDefaultRfidSlot(nodeId));
        }
        if (nsCurrentNodeId === nodeId) renderNodeSettings(nodeId);
        renderDashboard();
        break;
      }
      case "node_rfid_config_ack": {
        const nodeId = Number(msg.node_id || 0);
        if (msg.ok) {
          setRfidSaveNote(nodeId, "Saved on node.", "ok");
          showToast("RFID card action saved.", "success");
        } else {
          setRfidSaveNote(nodeId, msg.err || "Could not save RFID config.", "err");
          showToast(msg.err || "Could not save RFID config.", "error");
        }
        break;
      }
      case "rfid_scan_event": {
        const nodeId = Number(msg.node_id || 0);
        const uid = normalizeRfidUidInput(msg.uid || "");
        const matchedSlot = Number(msg.matched_slot ?? -1);
        const prev = nodeRfidConfigs.get(nodeId) || { ready: false, slots: [] };
        nodeRfidConfigs.set(nodeId, {
          ...prev,
          last_scan: {
            uid,
            matched_slot: matchedSlot,
            relay_mask: Number(msg.relay_mask || 0)
          }
        });
        const targetSlot = matchedSlot >= 0
          ? matchedSlot
          : getRfidSlotsForNode(nodeId).find(slot => !slot.enabled)?.slot ?? 0;
        const state = loadRfidEditorSlot(nodeId, targetSlot);
        if (uid) {
          state.uid = uid;
          if (matchedSlot < 0) state.relayMask = 0;
          rfidEditorState.set(nodeId, state);
        }
        if (uid) {
          if (matchedSlot >= 0) {
            showToast("RFID Card Swiped. Performing Actions.", "success");
          } else {
            showToast("New RFID Card Detected. Ready for addition to database.", "warn");
          }
        }
        if (nsCurrentNodeId === nodeId) renderNodeSettings(nodeId);
        break;
      }
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
        [...nodeActuatorSchemas.keys()].forEach(id => {
          if (!activeIds.has(id)) nodeActuatorSchemas.delete(id);
        });
        [...nodeRfidConfigs.keys()].forEach(id => {
          if (!activeIds.has(id)) nodeRfidConfigs.delete(id);
        });
        [...rfidEditorState.keys()].forEach(id => {
          if (!activeIds.has(id)) rfidEditorState.delete(id);
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

        if (nsCurrentNodeId && $nsOverlay.style.display !== "none") {
          const nextModalNode = nodes.get(nsCurrentNodeId);
          if (prevModalNode?.online && nextModalNode && !nextModalNode.online) {
            closeNodeSettings(false);
            showToast(`${nextModalNode.name || `Node #${nsCurrentNodeId}`} went offline. Settings panel closed.`, "warn");
          }
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
          if (n.actuator_schema_ready && !nodeActuatorSchemas.has(n.id)) {
            send({ type: "node_actuator_schema_get", node_id: n.id });
          }
          if (n.rfid_ready && !nodeRfidConfigs.has(n.id)) {
            send({ type: "node_rfid_config_get", node_id: n.id });
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
      case "coproc_ota_state":
        applyCoprocOtaState(msg);
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
      case "pair_capacity_full": {
        const rejectedMac = msg.mac || "";
        if (rejectedMac) pendingMacs.delete(rejectedMac);
        else pendingMacs.clear();
        renderAvailable();

        const currentNodes = Number(msg.current_nodes ?? nodes.size ?? 0);
        const maxNodes = Number(msg.max_nodes ?? currentNodes ?? 0);
        showConfirm({
          title: "Gateway Node Limit Reached",
          body: `This gateway already has <strong>${currentNodes}</strong> paired node(s), which is the current maximum of <strong>${maxNodes}</strong>.<br><br>
                 Disconnect an existing node first, then try pairing this device again.`,
          okLabel: "Dismiss",
          okClass: "warn",
          hideCancel: true
        });
        break;
      }
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
      case "offline_ap_config_ack":
        if (msg.ok) {
          const note = msg.active_now
            ? "✓ Saved — active offline AP credentials will update after the next restart."
            : "✓ Saved — takes effect next time offline mode starts.";
          setOfflineApSaveNote(note, "ok");
          showToast("✓ Offline AP config saved.", "success");
        } else {
          setOfflineApSaveNote("✕ " + (msg.err || "Error"), "err");
          showToast("✕ " + (msg.err || "Could not save offline AP config"), "error");
        }
        if ($saveOfflineBtn) $saveOfflineBtn.disabled = false;
        break;
      case "gw_portal_starting":
        suppressNextWsCloseToast = true;
        showToast("📶 Gateway restarting into WiFi setup portal…", "warn");
        break;
      case "gw_rebooting":
        suppressNextWsCloseToast = true;
        if (gatewayOtaUploading || $gwOtaProgress?.style.display !== "none") {
          gatewayOtaUploading = false;
          gatewayOtaBusy = true;
          setGatewayOtaProgress(100, "Gateway rebooting into the updated firmware...");
          setGatewayOtaNote("Gateway rebooting into the updated firmware...", "ok");
          refreshGatewayOtaUi();
        }
        showToast("Gateway rebooting...", "warn");
        break;
      case "gw_network_notice":
        handleGatewayNetworkNotice(msg);
        break;
      case "gw_factory_reset":
        suppressNextWsCloseToast = true;
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
function getGatewayBuiltinSensorStatusLabel() {
  if (!gatewayBuiltinSensor.enabled) return "Disabled in firmware";
  if (!gatewayBuiltinSensor.present) return "Sensor not detected";
  return getGatewayBuiltinSensorModelLabel();
}

function getGatewayBuiltinSensorModelLabel() {
  if (gatewayBuiltinSensor.model === "bme280") return "BME280";
  if (gatewayBuiltinSensor.model === "bmp280") return "BMP280";
  return "AUTO";
}

function formatGatewayBuiltinSensorValue(value, digits = 1) {
  const num = Number(value);
  return Number.isFinite(num) ? num.toFixed(digits) : "\u2014";
}

function convertGatewayBuiltinAltitudeValue(value, fromUnit, toUnit) {
  const num = Number(value);
  if (!Number.isFinite(num) || fromUnit === toUnit) return num;
  return toUnit === "ft" ? (num * 3.2808399) : (num / 3.2808399);
}

function setGatewayBuiltinSensorSaveNote(msg, type = "") {
  if (!$gwBuiltinSensorSaveNote) return;
  $gwBuiltinSensorSaveNote.textContent = msg;
  $gwBuiltinSensorSaveNote.className = "setting-save-note" + (type ? " " + type : "");
  if (msg) {
    setTimeout(() => {
      if ($gwBuiltinSensorSaveNote.textContent === msg) $gwBuiltinSensorSaveNote.textContent = "";
    }, 5000);
  }
}

function renderGatewayBuiltinSensorCard() {
  if (!$gwBuiltinSensorGrid) return;

  if (!gatewayBuiltinSensor.enabled) {
    $gwBuiltinSensorGrid.style.display = "none";
    $gwBuiltinSensorGrid.innerHTML = "";
    return;
  }

  const present = !!gatewayBuiltinSensor.present;
  const modelLabel = getGatewayBuiltinSensorModelLabel();
  let card = $gwBuiltinSensorGrid.querySelector(".gateway-builtin-sensor-card");
  if (!card) {
    $gwBuiltinSensorGrid.innerHTML = `
      <div class="node-card gateway-builtin-sensor-card">
        <div class="card-hdr">
          <span class="card-dot"></span>
          <span class="card-name">Gateway Built-in Sensor</span>
          <span class="card-badge sensor"></span>
        </div>
        <div class="card-rows"></div>
      </div>`;
    card = $gwBuiltinSensorGrid.querySelector(".gateway-builtin-sensor-card");
  }
  const dot = card?.querySelector(".card-dot");
  const badge = card?.querySelector(".card-badge");
  const rowsEl = card?.querySelector(".card-rows");
  const rows = [];

  if (present) {
    rows.push(`
      <div class="card-row">
        <span class="lbl">Temperature</span>
        <span class="val val-temp">${formatGatewayBuiltinSensorValue(gatewayBuiltinSensor.temperature, 1)} \u00b0${gatewayBuiltinSensor.tempUnit}</span>
      </div>`);
    rows.push(`
      <div class="card-row">
        <span class="lbl">Sea-Level Pressure</span>
        <span class="val val-pres">${formatGatewayBuiltinSensorValue(gatewayBuiltinSensor.pressureHpa, 1)} hPa</span>
      </div>`);
    if (gatewayBuiltinSensor.model === "bme280") {
      rows.push(`
        <div class="card-row">
          <span class="lbl">Humidity</span>
          <span class="val">${formatGatewayBuiltinSensorValue(gatewayBuiltinSensor.humidity, 1)} %</span>
        </div>`);
    }
    if (gatewayBuiltinSensor.altitudeConfigured) {
      rows.push(`
        <div class="card-row">
          <span class="lbl">Pressure Reference</span>
          <span class="val">${formatGatewayBuiltinSensorValue(gatewayBuiltinSensor.altitudeValue, 1)} ${gatewayBuiltinSensor.altitudeUnit}</span>
        </div>`);
    }
  } else {
    rows.push(`
      <div class="card-row">
        <span class="lbl">Status</span>
        <span class="val val-muted">Sensor not detected</span>
      </div>`);
  }

  $gwBuiltinSensorGrid.style.display = "";
  if (card) {
    card.classList.toggle("offline", !present);
  }
  if (dot) {
    dot.className = `card-dot ${present ? "online" : "offline"}`;
  }
  if (badge) {
    badge.textContent = modelLabel;
  }
  if (rowsEl) {
    rowsEl.innerHTML = rows.join("");
  }
}

function syncGatewayBuiltinSensorDraftFromMeta(force = false) {
  if (!force && gatewayBuiltinSensorDraft.dirty) return;
  gatewayBuiltinSensorDraft.tempUnit = gatewayBuiltinSensor.tempUnit || "C";
  gatewayBuiltinSensorDraft.altitudeUnit = gatewayBuiltinSensor.altitudeUnit || "m";
  gatewayBuiltinSensorDraft.altitudeValueText = gatewayBuiltinSensor.altitudeConfigured
    ? formatGatewayBuiltinSensorValue(gatewayBuiltinSensor.altitudeValue, 1)
    : "";
  gatewayBuiltinSensorDraft.dirty = false;
  gatewayBuiltinSensorDraft.saving = false;
}

function markGatewayBuiltinSensorDraftDirty() {
  gatewayBuiltinSensorDraft.dirty = true;
}

function renderGatewayBuiltinSensorSettings() {
  if (!$gwBuiltinSensorSettingsCard) return;

  if (!gatewayBuiltinSensor.enabled) {
    $gwBuiltinSensorSettingsCard.style.display = "none";
    gatewayBuiltinSensorDraft.dirty = false;
    gatewayBuiltinSensorDraft.saving = false;
    return;
  }

  $gwBuiltinSensorSettingsCard.style.display = "";
  if ($gwBuiltinSensorStatus) {
    $gwBuiltinSensorStatus.textContent = getGatewayBuiltinSensorStatusLabel();
  }
  if ($gwBuiltinSensorUnitView) {
    $gwBuiltinSensorUnitView.textContent = gatewayBuiltinSensor.tempUnit === "F"
      ? "Fahrenheit (\u00b0F)"
      : "Celsius (\u00b0C)";
  }
  if ($gwBuiltinSensorAltView) {
    $gwBuiltinSensorAltView.textContent = gatewayBuiltinSensor.altitudeConfigured
      ? `${formatGatewayBuiltinSensorValue(gatewayBuiltinSensor.altitudeValue, 1)} ${gatewayBuiltinSensor.altitudeUnit}`
      : "Not configured";
  }

  const formTempUnit = gatewayBuiltinSensorDraft.dirty
    ? gatewayBuiltinSensorDraft.tempUnit
    : gatewayBuiltinSensor.tempUnit;
  const formAltitudeValue = gatewayBuiltinSensorDraft.dirty
    ? gatewayBuiltinSensorDraft.altitudeValueText
    : (gatewayBuiltinSensor.altitudeConfigured
        ? formatGatewayBuiltinSensorValue(gatewayBuiltinSensor.altitudeValue, 1)
        : "");
  const formAltitudeUnit = gatewayBuiltinSensorDraft.dirty
    ? gatewayBuiltinSensorDraft.altitudeUnit
    : gatewayBuiltinSensor.altitudeUnit;

  if ($gwBuiltinTempUnit) {
    $gwBuiltinTempUnit.value = formTempUnit;
  }
  if ($gwBuiltinAltitudeValue) {
    $gwBuiltinAltitudeValue.value = formAltitudeValue;
  }
  if ($gwBuiltinAltitudeUnit) {
    $gwBuiltinAltitudeUnit.value = formAltitudeUnit;
  }
  if ($gwBuiltinAltitudeUnit) {
    $gwBuiltinAltitudeUnit.dataset.prevValue = formAltitudeUnit;
  }
}

function saveGatewayBuiltinSensorSettings() {
  if (!gatewayBuiltinSensor.enabled) return;

  const tempUnit = $gwBuiltinTempUnit?.value || "C";
  const altitudeUnit = $gwBuiltinAltitudeUnit?.value || "m";
  const altitudeValue = Number($gwBuiltinAltitudeValue?.value ?? "0");

  if (!["C", "F"].includes(tempUnit)) {
    setGatewayBuiltinSensorSaveNote("\u2715 Temperature unit must be C or F.", "err");
    $gwBuiltinTempUnit?.focus();
    return;
  }
  if (!["m", "ft"].includes(altitudeUnit)) {
    setGatewayBuiltinSensorSaveNote("\u2715 Altitude unit must be m or ft.", "err");
    $gwBuiltinAltitudeUnit?.focus();
    return;
  }
  if (!Number.isFinite(altitudeValue)) {
    setGatewayBuiltinSensorSaveNote("\u2715 Altitude must be a valid number.", "err");
    $gwBuiltinAltitudeValue?.focus();
    return;
  }

  gatewayBuiltinSensorDraft.tempUnit = tempUnit;
  gatewayBuiltinSensorDraft.altitudeValueText = $gwBuiltinAltitudeValue?.value ?? "";
  gatewayBuiltinSensorDraft.altitudeUnit = altitudeUnit;
  gatewayBuiltinSensorDraft.dirty = true;
  gatewayBuiltinSensorDraft.saving = true;
  if ($gwBuiltinSensorSaveBtn) $gwBuiltinSensorSaveBtn.disabled = true;
  setGatewayBuiltinSensorSaveNote("Saving...");
  send({
    type: "gw_builtin_sensor_settings_set",
    temp_unit: tempUnit,
    altitude_value: altitudeValue,
    altitude_unit: altitudeUnit
  });
}

function getMqttStatusLabel() {
  if (!gatewayMqtt.enabled) return "Disabled";
  return gatewayMqtt.connected ? "Connected" : "Disconnected";
}

function setMqttSaveNote(msg, type = "") {
  if (!$mqttSaveNote) return;
  $mqttSaveNote.textContent = msg;
  $mqttSaveNote.className = "setting-save-note" + (type ? " " + type : "");
  if (msg) {
    setTimeout(() => {
      if ($mqttSaveNote.textContent === msg) $mqttSaveNote.textContent = "";
    }, 5000);
  }
}

function syncGatewayMqttDraftFromMeta(force = false) {
  if (!force && gatewayMqttDraft.dirty) return;
  gatewayMqttDraft.enabled = !!gatewayMqtt.enabled;
  gatewayMqttDraft.host = gatewayMqtt.host || "";
  gatewayMqttDraft.portText = String(Number(gatewayMqtt.port || 1883));
  gatewayMqttDraft.baseTopic = gatewayMqtt.baseTopic || "";
  gatewayMqttDraft.username = gatewayMqtt.username || "";
  gatewayMqttDraft.password = "";
  gatewayMqttDraft.dirty = false;
  gatewayMqttDraft.saving = false;
}

function markGatewayMqttDraftDirty() {
  gatewayMqttDraft.dirty = true;
}

function renderMqttSettings() {
  if (!$mqttSettingsCard) return;

  const formEnabled = gatewayMqttDraft.dirty ? gatewayMqttDraft.enabled : gatewayMqtt.enabled;
  const formHost = gatewayMqttDraft.dirty ? gatewayMqttDraft.host : gatewayMqtt.host;
  const formPort = gatewayMqttDraft.dirty ? gatewayMqttDraft.portText : String(Number(gatewayMqtt.port || 1883));
  const formBaseTopic = gatewayMqttDraft.dirty ? gatewayMqttDraft.baseTopic : gatewayMqtt.baseTopic;
  const formUsername = gatewayMqttDraft.dirty ? gatewayMqttDraft.username : gatewayMqtt.username;

  if ($mqttStatus) $mqttStatus.textContent = getMqttStatusLabel();
  if ($mqttPasswordStatus) {
    $mqttPasswordStatus.textContent = gatewayMqtt.hasPassword
      ? "Stored on gateway"
      : "Not stored";
  }
  if ($mqttLastError) {
    $mqttLastError.textContent = gatewayMqtt.lastError || "\u2014";
  }
  if ($mqttEnabledToggle) $mqttEnabledToggle.checked = !!formEnabled;
  if ($mqttHostInput) $mqttHostInput.value = formHost || "";
  if ($mqttPortInput) $mqttPortInput.value = formPort || "1883";
  if ($mqttBaseTopicInput) $mqttBaseTopicInput.value = formBaseTopic || "";
  if ($mqttUserInput) $mqttUserInput.value = formUsername || "";
  if ($mqttPassInput && document.activeElement !== $mqttPassInput) {
    $mqttPassInput.value = gatewayMqttDraft.password || "";
  }
  if ($mqttSaveBtn) $mqttSaveBtn.disabled = !!gatewayMqttDraft.saving;
}

function saveMqttConfig() {
  const enabled = !!$mqttEnabledToggle?.checked;
  const host = ($mqttHostInput?.value || "").trim();
  const portText = ($mqttPortInput?.value || "").trim();
  const baseTopic = ($mqttBaseTopicInput?.value || "").trim().replace(/^\/+|\/+$/g, "");
  const username = ($mqttUserInput?.value || "").trim();
  const password = $mqttPassInput?.value || "";
  const port = Number(portText || "1883");

  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    setMqttSaveNote("\u2715 Port must be between 1 and 65535.", "err");
    $mqttPortInput?.focus();
    return;
  }
  if (enabled && !host) {
    setMqttSaveNote("\u2715 Broker host is required to enable MQTT.", "err");
    $mqttHostInput?.focus();
    return;
  }
  if (enabled && !baseTopic) {
    setMqttSaveNote("\u2715 Base topic is required to enable MQTT.", "err");
    $mqttBaseTopicInput?.focus();
    return;
  }
  if (enabled && !username) {
    setMqttSaveNote("\u2715 Username is required to enable MQTT.", "err");
    $mqttUserInput?.focus();
    return;
  }
  if (enabled && !password && !gatewayMqtt.hasPassword) {
    setMqttSaveNote("\u2715 Password is required to enable MQTT.", "err");
    $mqttPassInput?.focus();
    return;
  }

  gatewayMqttDraft.enabled = enabled;
  gatewayMqttDraft.host = host;
  gatewayMqttDraft.portText = String(port);
  gatewayMqttDraft.baseTopic = baseTopic;
  gatewayMqttDraft.username = username;
  gatewayMqttDraft.password = password;
  gatewayMqttDraft.dirty = true;
  gatewayMqttDraft.saving = true;
  if ($mqttSaveBtn) $mqttSaveBtn.disabled = true;
  setMqttSaveNote("Saving...");
  send({
    type: "mqtt_config_set",
    enabled,
    host,
    port,
    base_topic: baseTopic,
    username,
    password
  });
}

function applyMeta(m) {
  const previousSnapshot = loadGatewayNetworkSnapshot();
  gatewayAccessIp = m.access_ip || m.ip || "-";
  gatewayNetworkMode = m.network_mode || "Online (Router)";
  gatewayRouterOnline = !!m.router_online;
  gatewayOfflineApActive = !!m.offline_ap_active;
  gatewayOfflineApSsid = m.offline_ap_ssid || gatewayOfflineApSsid || "";
  gatewayRouterSsid = m.router_ssid || gatewayRouterSsid || "";
  gatewayBuiltinSensor = {
    enabled: !!m.gw_builtin_sensor_enabled,
    model: m.gw_builtin_sensor_model || "disabled",
    present: !!m.gw_builtin_sensor_present,
    tempUnit: m.gw_builtin_sensor_temp_unit || "C",
    altitudeConfigured: !!m.gw_builtin_sensor_altitude_configured,
    altitudeValue: Number(m.gw_builtin_sensor_altitude_value ?? 0),
    altitudeUnit: m.gw_builtin_sensor_altitude_unit || "m",
    temperature: m.gw_builtin_sensor_temperature !== undefined ? Number(m.gw_builtin_sensor_temperature) : null,
    pressureHpa: m.gw_builtin_sensor_pressure_hpa !== undefined ? Number(m.gw_builtin_sensor_pressure_hpa) : null,
    humidity: m.gw_builtin_sensor_humidity !== undefined ? Number(m.gw_builtin_sensor_humidity) : null
  };
  gatewayMqtt = {
    enabled: !!m.mqtt_enabled,
    connected: !!m.mqtt_connected,
    host: m.mqtt_host || "",
    port: Number(m.mqtt_port || 1883),
    baseTopic: m.mqtt_base_topic || "",
    username: m.mqtt_username || "",
    hasPassword: !!m.mqtt_has_password,
    lastError: m.mqtt_last_error || ""
  };
  syncGatewayBuiltinSensorDraftFromMeta(false);
  syncGatewayMqttDraftFromMeta(false);
  $mIp.textContent  = m.ip      ?? "—";
  $mMac.textContent = m.mac     ?? "—";
  $mCh.textContent  = m.channel ?? "—";
  serverUptime   = m.uptime ?? 0;
  uptimeSyncedAt = performance.now();
  $mUp.textContent = fmtUptime(serverUptime);
  $mIp.textContent = gatewayAccessIp;
  if ($gwNetMode) $gwNetMode.textContent = gatewayNetworkMode;
  if ($gwAccessIp) $gwAccessIp.textContent = gatewayAccessIp;
  if ($gwRouterLink) {
    $gwRouterLink.textContent = gatewayRouterOnline
      ? "Connected"
      : (gatewayOfflineApActive ? "Unavailable (offline fallback)" : "Disconnected");
  }
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
  if (gatewayOfflineApSsid && document.activeElement !== $offlineSsidInput) {
    $offlineSsidInput.value = gatewayOfflineApSsid;
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
  coprocOnline = !!m.coproc_online;
  coprocFwVersion = m.coproc_fw_version || "";
  coprocOtaSupported = !!m.coproc_ota_supported;
  coprocOtaBusy = !!m.coproc_ota_busy;
  coprocOtaMaxBytes = Number(m.coproc_ota_max_bytes || 0);
  coprocOtaProject = m.coproc_project || "";
  nodeOtaBusy = !!m.node_ota_busy;
  nodeOtaHelperOnline = !!m.node_ota_helper_online;
  nodeOtaHost = m.node_ota_host || "192.168.4.1";
  if (!gatewayOtaUploading && !gatewayOtaBusy && !coprocOtaBusy) {
    resetGatewayOtaProgress();
  }
  refreshGatewayOtaUi();
  refreshNodeOtaUi();
  renderGatewayBuiltinSensorCard();
  renderGatewayBuiltinSensorSettings();
  renderMqttSettings();
  const currentSnapshot = {
    network_mode: gatewayNetworkMode,
    access_ip: gatewayAccessIp,
    offline_ap_ssid: gatewayOfflineApSsid,
    router_ssid: gatewayRouterSsid
  };
  showGatewayTransitionToast(previousSnapshot, currentSnapshot);
  saveGatewayNetworkSnapshot();
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

function nodeHasSensorData(node) {
  const caps = Number(node?.capabilities || 0);
  return !!(caps & 0x01) || Number(node?.type) === 1;
}

function nodeHasActuators(node) {
  const caps = Number(node?.capabilities || 0);
  return !!(caps & 0x02) || Number(node?.type) === 2 || Number(node?.type) === 3;
}

function nodeHasRfid(node) {
  const caps = Number(node?.capabilities || 0);
  return !!(caps & 0x04);
}

function getNodeTypeClass(nodeOrType) {
  const type = typeof nodeOrType === "number"
    ? nodeOrType
    : Number(nodeOrType?.type || 0);
  if (type === 1) return "sensor";
  if (type === 3) return "hybrid";
  return "relay";
}

function getNodeTypeLabel(nodeOrType, compact = false) {
  const type = typeof nodeOrType === "number"
    ? nodeOrType
    : Number(nodeOrType?.type || 0);
  if (type === 1) return compact ? "SENSOR" : "Sensor";
  if (type === 3) return compact ? "HYBRID" : "Hybrid";
  return compact ? "ACTUATOR" : "Actuator";
}

function getActuatorSchemaForNode(nodeId) {
  return nodeActuatorSchemas.get(nodeId) || [];
}

function getActuatorCountForNode(node) {
  if (!nodeHasActuators(node)) return 0;
  const schema = getActuatorSchemaForNode(node?.id);
  if (schema.length > 0) return schema.length;
  const count = Number(node?.actuator_count || 0);
  return count > 0 ? count : RELAY_CHANNEL_COUNT;
}

function getRelayLabelsForNode(node) {
  const labels = Array.isArray(node?.relay_labels) ? node.relay_labels : [];
  const schema = getActuatorSchemaForNode(node?.id);
  const count = getActuatorCountForNode(node);
  return Array.from({ length: count }, (_, i) => {
    const fallback = schema[i]?.label || defaultRelayLabel(i);
    return sanitizeRelayLabel(labels[i] || fallback, i);
  });
}

function setRelayLabelsOnNode(nodeId, labels) {
  const n = nodes.get(nodeId);
  if (!n) return null;
  const normalized = Array.from({ length: getActuatorCountForNode(n) }, (_, i) => (
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
  const n = nodes.get(nodeId);
  relayLabelDrafts.set(nodeId, Array.from(
    { length: getActuatorCountForNode(n) },
    (_, i) => sanitizeRelayLabel(labels[i], i)
  ));
}

function clearRelayLabelDraft(nodeId) {
  relayLabelDrafts.delete(nodeId);
}

function getActuatorMask(n) {
  return Number(n.actuator_mask ?? n.relay_mask ?? 0);
}

function normalizeRfidUidInput(text) {
  const compact = String(text || "")
    .toUpperCase()
    .replace(/[^0-9A-F]/g, "")
    .slice(0, 20);
  return compact.replace(/(..)(?=.)/g, "$1 ").trim();
}

function compactRfidUid(text) {
  return normalizeRfidUidInput(text).replace(/\s+/g, "");
}

function isValidRfidUidInput(text) {
  const compact = compactRfidUid(text);
  const byteLen = compact.length / 2;
  return compact.length > 0 && [4, 7, 10].includes(byteLen);
}

function getRfidSlotsForNode(nodeId) {
  const cfg = nodeRfidConfigs.get(nodeId);
  const rawSlots = Array.isArray(cfg?.slots) ? cfg.slots : [];
  return Array.from({ length: RFID_SLOT_COUNT }, (_, i) => {
    const slot = rawSlots.find(s => Number(s.slot) === i) || rawSlots[i] || {};
    return {
      slot: i,
      enabled: !!slot.enabled,
      uid: normalizeRfidUidInput(slot.uid || ""),
      relay_mask: Number(slot.relay_mask || 0)
    };
  });
}

function findDefaultRfidSlot(nodeId) {
  const slots = getRfidSlotsForNode(nodeId);
  const firstEnabled = slots.find(slot => slot.enabled);
  return firstEnabled ? firstEnabled.slot : 0;
}

function loadRfidEditorSlot(nodeId, slotIndex) {
  const slots = getRfidSlotsForNode(nodeId);
  const safeSlot = Math.max(0, Math.min(RFID_SLOT_COUNT - 1, Number(slotIndex || 0)));
  const slot = slots[safeSlot] || slots[0];
  const state = {
    selectedSlot: slot.slot,
    uid: slot.enabled ? slot.uid : "",
    relayMask: slot.enabled ? Number(slot.relay_mask || 0) : 0
  };
  rfidEditorState.set(nodeId, state);
  return state;
}

function getRfidEditorState(nodeId) {
  const current = rfidEditorState.get(nodeId);
  if (current && Number.isInteger(current.selectedSlot)) return current;
  return loadRfidEditorSlot(nodeId, findDefaultRfidSlot(nodeId));
}

function setRfidSaveNote(nodeId, msg, type = "") {
  const el = document.getElementById(`rfid-note-${nodeId}`);
  if (!el) return;
  el.textContent = msg;
  el.className = "setting-save-note" + (type ? " " + type : "");
}

function buildRfidSlotLabel(slot) {
  return slot.enabled
    ? `Card ${slot.slot + 1} - ${slot.uid || "UID saved"}`
    : `Card ${slot.slot + 1} - Empty`;
}

function buildRfidRelayButtons(node, relayMask) {
  const labels = getRelayLabelsForNode(node);
  return Array.from({ length: labels.length }, (_, i) => {
    const on = !!((Number(relayMask || 0) >> i) & 1);
    return `<button class="rfid-relay-toggle ${on ? "on" : "off"}"
      data-node-id="${node.id}" data-relay="${i}" type="button">
      ${esc(labels[i])}<br>${on ? "ON" : "OFF"}</button>`;
  }).join("");
}

function buildRfidSection(node) {
  const cfg = nodeRfidConfigs.get(node.id);
  if (!cfg?.ready) {
    return `
      <div class="nsettings-section">
        <div class="nsettings-section-hdr">RFID Card Actions</div>
        <div class="nsettings-section-desc">
          Saved RFID cards can apply a relay scene to this Hybrid node. Waiting for the node
          to send its RFID configuration table...
        </div>
        <div class="nsettings-empty">RFID config not available yet.</div>
      </div>`;
  }

  const slots = getRfidSlotsForNode(node.id);
  const state = getRfidEditorState(node.id);
  const selectedSlot = Math.max(0, Math.min(RFID_SLOT_COUNT - 1, Number(state.selectedSlot || 0)));
  const lastScan = cfg.last_scan || null;

  return `
    <div class="nsettings-section">
      <div class="nsettings-section-hdr">RFID Card Actions</div>
      <div class="nsettings-section-desc">
        Each saved card stores the full relay state mask for this node. When that card is swiped,
        the node overwrites all relay outputs with the saved states.
      </div>
      <div class="nsetting-row">
        <div class="nsetting-info">
          <div class="nsetting-label">Saved Card Slot</div>
          <div class="nsetting-type">Store up to ${RFID_SLOT_COUNT} RFID cards on this node</div>
        </div>
        <select class="ns-select rfid-slot-select" data-node-id="${node.id}">
          ${slots.map(slot => `
            <option value="${slot.slot}" ${slot.slot === selectedSlot ? "selected" : ""}>
              ${esc(buildRfidSlotLabel(slot))}
            </option>`).join("")}
        </select>
      </div>
      <div class="nsetting-row">
        <div class="nsetting-info">
          <div class="nsetting-label">RFID Card UID</div>
          <div class="nsetting-type">Enter manually or pull in the most recent scanned UID</div>
        </div>
        <div class="rfid-uid-row">
          <input type="text"
                 class="setting-input rfid-uid-input"
                 data-node-id="${node.id}"
                 value="${esc(state.uid || "")}"
                 maxlength="29"
                 placeholder="AA BB CC DD">
          <button class="tbl-btn rfid-use-last-btn"
                  data-node-id="${node.id}"
                  type="button"
                  ${lastScan?.uid ? "" : "disabled"}>
            Use Last Scan
          </button>
        </div>
      </div>
      <div class="nsetting-row rfid-last-scan-row">
        <div class="nsetting-info">
          <div class="nsetting-label">Latest Scan</div>
          <div class="nsetting-type">
            ${lastScan?.uid
              ? `${esc(lastScan.uid)}${Number(lastScan.matched_slot) >= 0 ? ` mapped to Card ${Number(lastScan.matched_slot) + 1}` : " not yet assigned"}`
              : "No RFID card has been scanned yet"}
          </div>
        </div>
      </div>
      <div class="rfid-relay-section">
        <div class="nsetting-label">Relay Scene</div>
        <div class="nsetting-type">Tap each relay to define its absolute state for this card</div>
        <div class="rfid-relay-grid">${buildRfidRelayButtons(node, state.relayMask)}</div>
      </div>
      <div class="setting-save-row relay-label-actions">
        <button class="settings-save-btn rfid-save-btn" data-node-id="${node.id}" type="button">
          Save Card Action
        </button>
        <button class="tbl-btn rfid-delete-btn" data-node-id="${node.id}" type="button">
          Delete Slot
        </button>
        <span class="setting-save-note" id="rfid-note-${node.id}"></span>
      </div>
    </div>`;
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
  const typeCls = getNodeTypeClass(n);
  const typeLbl = getNodeTypeLabel(n, true);
  const body = [
    nodeHasSensorData(n) ? buildSensorRows(n) : "",
    nodeHasActuators(n) ? buildRelayGrid(n) : ""
  ].filter(Boolean).join("");

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
  const btns = Array.from({ length: labels.length }, (_, i) => {
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

  const badge = el.querySelector(".card-badge");
  if (badge) {
    badge.className = `card-badge ${getNodeTypeClass(n)}`;
    badge.textContent = getNodeTypeLabel(n, true);
  }

  const rowsEl = el.querySelector(".card-rows");
  if (rowsEl) {
    rowsEl.innerHTML = [
      nodeHasSensorData(n) ? buildSensorRows(n) : "",
      nodeHasActuators(n) ? buildRelayGrid(n) : ""
    ].filter(Boolean).join("");
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
  const typeCls = getNodeTypeClass(n);
  const typeLbl = getNodeTypeLabel(n);
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
  const typeCls = getNodeTypeClass(n);
  const typeLbl = getNodeTypeLabel(n);
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

  const typeLbl = getNodeTypeLabel(n);

  $modalBody.innerHTML = `
    <div class="modal-row">
      <span class="mlbl">Name</span>
      <div class="modal-name-wrap">
        <input class="modal-name-input" id="rename-input"
               value="${esc(n.name || "")}" maxlength="24"
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
  name = name.trim().substring(0, 24);
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
  closeSidebar();
  showConfirm({
    title: "Reboot Gateway?",
    body: `The ESP32-S3 Gateway will <strong>restart immediately</strong>.<br><br>
           The dashboard will disconnect briefly and then <strong>reconnect automatically</strong>
           once the gateway finishes booting.`,
    okLabel: "Yes, Reboot Gateway",
    okClass: "warn",
    callback() {
      send({ type: "reboot_gw" });
      showToast("Gateway rebooting — reconnecting...", "warn");
      // Disable the button briefly so it can't be clicked again mid-reboot
      $gwRebootButtons.forEach(btn => { btn.disabled = true; });
      setTimeout(() => {
        $gwRebootButtons.forEach(btn => { btn.disabled = false; });
      }, 12000);
    }
  });
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
let confirmScrollLockY = 0;

function lockConfirmPageScroll() {
  if (document.body.classList.contains("sidebar-open") || document.body.classList.contains("confirm-open")) return;
  confirmScrollLockY = window.scrollY || window.pageYOffset || 0;
  document.body.style.top = `-${confirmScrollLockY}px`;
  document.body.classList.add("confirm-open");
}

function unlockConfirmPageScroll() {
  if (!document.body.classList.contains("confirm-open")) return;
  document.body.classList.remove("confirm-open");
  if (document.body.classList.contains("sidebar-open")) return;
  document.body.style.top = "";
  window.scrollTo(0, confirmScrollLockY);
}

function showConfirm({ title, body, okLabel = "Confirm", okClass = "", cancelLabel = "Cancel", hideCancel = false, callback }) {
  $confirmTitle.textContent = title;
  $confirmBody.innerHTML    = body;   // supports HTML for bold/line-breaks in warning text
  $confirmOkBtn.textContent = okLabel;
  $confirmOkBtn.className   = "confirm-ok-btn" + (okClass ? " " + okClass : "");
  $confirmCancelBtn.textContent = cancelLabel;
  $confirmCancelBtn.style.display = hideCancel ? "none" : "";
  _confirmCallback          = callback;
  lockConfirmPageScroll();
  $confirmOverlay.style.display = "";
  (hideCancel ? $confirmOkBtn : $confirmCancelBtn).focus();
}
function closeConfirm() {
  $confirmOverlay.style.display = "none";
  _confirmCallback = null;
  $confirmCancelBtn.style.display = "";
  $confirmCancelBtn.textContent = "Cancel";
  unlockConfirmPageScroll();
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

function setOfflineApSaveNote(msg, type = "") {
  if (!$offlineSaveNote) return;
  $offlineSaveNote.textContent = msg;
  $offlineSaveNote.className = "setting-save-note" + (type ? " " + type : "");
  if (msg) setTimeout(() => {
    if ($offlineSaveNote.textContent === msg) $offlineSaveNote.textContent = "";
  }, 5000);
}

function saveOfflineApConfig() {
  const ssid = $offlineSsidInput?.value.trim() || "";
  const pass = $offlinePassInput?.value || "";
  if (ssid.length < 2 || ssid.length > 32) {
    setOfflineApSaveNote("✕ SSID must be 2-32 characters.", "err");
    $offlineSsidInput?.focus();
    return;
  }
  if (pass.length > 0 && pass.length < 8) {
    setOfflineApSaveNote("✕ Password must be 8+ chars or blank.", "err");
    $offlinePassInput?.focus();
    return;
  }
  if ($saveOfflineBtn) $saveOfflineBtn.disabled = true;
  setOfflineApSaveNote("Saving...");
  send({ type: "set_offline_ap_config", ssid, password: pass });
}

if ($saveOfflineBtn) $saveOfflineBtn.addEventListener("click", saveOfflineApConfig);

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

function getGatewayOtaTarget() {
  gatewayOtaTarget = $gwOtaTarget?.value === "coprocessor" ? "coprocessor" : "main";
  return gatewayOtaTarget;
}

function getGatewayOtaTargetLabel(target = getGatewayOtaTarget()) {
  return target === "coprocessor" ? "Coprocessor (ESP32-C3)" : "Main MCU (ESP32-S3)";
}

function getGatewayOtaSelectedVersion(target = getGatewayOtaTarget()) {
  const version = target === "coprocessor" ? coprocFwVersion : fwVersion;
  return version && version !== "—" ? `v${version}` : "v-";
}

function getGatewayOtaSelectedProject(target = getGatewayOtaTarget()) {
  return target === "coprocessor" ? (coprocOtaProject || "-") : (gatewayOtaProject || "-");
}

function getGatewayOtaSelectedSlotSize(target = getGatewayOtaTarget()) {
  const maxBytes = target === "coprocessor" ? coprocOtaMaxBytes : gatewayOtaMaxBytes;
  return maxBytes > 0 ? fmtBytes(maxBytes) : "Not available";
}

function getGatewayOtaSelectedSupported(target = getGatewayOtaTarget()) {
  return target === "coprocessor" ? coprocOtaSupported : gatewayOtaSupported;
}

function isGatewayUploadingToCoprocessor() {
  return gatewayOtaUploading && gatewayOtaTarget === "coprocessor";
}

function isCoprocessorReservedByNodeOta() {
  return nodeOtaUploading || nodeOtaBusy;
}

function isCoprocessorReservedByCoprocOta() {
  return coprocOtaBusy || isGatewayUploadingToCoprocessor();
}

function isGatewayFirmwareUpdateBlockedByNodeOta() {
  return isCoprocessorReservedByNodeOta();
}

function isGatewayFirmwareUpdateBlockedByCoproc() {
  return isCoprocessorReservedByNodeOta() || isCoprocessorReservedByCoprocOta();
}

function getGatewayOtaSelectedBusy(target = getGatewayOtaTarget()) {
  return target === "coprocessor"
    ? (coprocOtaBusy || isCoprocessorReservedByNodeOta())
    : (gatewayOtaBusy || isGatewayFirmwareUpdateBlockedByCoproc());
}

function applyCoprocOtaState(msg) {
  coprocOtaState = {
    active: !!msg.active,
    stage: Number(msg.stage || 0),
    progress: Number(msg.progress || 0),
    message: msg.message || "",
    error: msg.error || "",
    version: msg.version || "",
    current_version: msg.current_version || coprocFwVersion || ""
  };
  coprocOnline = msg.online !== undefined ? !!msg.online : coprocOnline;
  coprocFwVersion = coprocOtaState.current_version || coprocFwVersion;
  coprocOtaBusy = !!msg.active || !!msg.upload_busy;

  if (getGatewayOtaTarget() === "coprocessor") {
    if (coprocOtaState.active) {
      const text = coprocOtaState.error || coprocOtaState.message || "Coprocessor OTA in progress...";
      setGatewayOtaProgress(coprocOtaState.progress, text);
      setGatewayOtaNote(text, coprocOtaState.error ? "err" : "ok");
    } else if (!gatewayOtaUploading) {
      if (coprocOtaState.error) {
        setGatewayOtaProgress(100, coprocOtaState.error);
        setGatewayOtaNote(coprocOtaState.error, "err");
      } else if (coprocOtaState.message) {
        setGatewayOtaProgress(coprocOtaState.progress || 100, coprocOtaState.message);
        setGatewayOtaNote(coprocOtaState.message, "ok");
      } else {
        resetGatewayOtaProgress();
      }
    }
  }

  refreshGatewayOtaUi();
  refreshNodeOtaUi();
}

function refreshGatewayOtaUi() {
  const target = getGatewayOtaTarget();
  const targetLabel = getGatewayOtaTargetLabel(target);
  const supported = getGatewayOtaSelectedSupported(target);
  const busy = getGatewayOtaSelectedBusy(target);
  const nodeOtaBlocksGatewaySection = isGatewayFirmwareUpdateBlockedByNodeOta();
  const coprocBlocksGatewaySection = isGatewayFirmwareUpdateBlockedByCoproc();
  const coprocOtaOwnsGatewaySection = target === "coprocessor"
    && (isGatewayUploadingToCoprocessor() || coprocOtaBusy || coprocOtaState.active);

  if ($gwOtaCurrent) $gwOtaCurrent.textContent = getGatewayOtaSelectedVersion(target);
  if ($gwOtaSlot) $gwOtaSlot.textContent = supported ? getGatewayOtaSelectedSlotSize(target) : "Not available";
  if ($gwOtaProject) $gwOtaProject.textContent = getGatewayOtaSelectedProject(target);

  if (!$gwOtaBtn || !$gwOtaFileInput) return;

  const hasFile = !!$gwOtaFileInput.files?.length;
  const anyGatewayOtaBusy = gatewayOtaUploading || gatewayOtaBusy || coprocBlocksGatewaySection;
  const disabled = gatewayOtaUploading || busy || !supported || !hasFile;
  $gwOtaBtn.disabled = disabled;
  $gwOtaFileInput.disabled = gatewayOtaUploading || busy || !supported;
  if ($gwOtaTarget) $gwOtaTarget.disabled = anyGatewayOtaBusy;

  if (gatewayOtaUploading) {
    $gwOtaBtn.textContent = "Uploading...";
    return;
  }

  $gwOtaBtn.textContent = busy ? "OTA Busy" : "Upload Firmware";
  const hasExplicitNote = !!($gwOtaNote?.textContent?.trim());

  if (!supported) {
    if (target === "coprocessor") {
      setGatewayOtaNote("Coprocessor OTA is unavailable until the coprocessor is running the OTA-capable helper firmware.", "err");
    } else {
      setGatewayOtaNote("OTA is unavailable until the gateway is flashed with the OTA partition layout.", "err");
    }
  } else if (busy) {
    if (nodeOtaBlocksGatewaySection) {
      setGatewayOtaNote(COPROC_BUSY_MSG, "err");
    } else if (coprocOtaOwnsGatewaySection) {
      // Preserve the active coprocessor OTA note/progress owned by this panel.
    } else if (!hasExplicitNote) {
      if (target === "coprocessor" && isCoprocessorReservedByCoprocOta()) {
        setGatewayOtaNote("A coprocessor firmware update is already in progress.", "ok");
      } else if (target !== "coprocessor" && isCoprocessorReservedByCoprocOta()) {
        setGatewayOtaNote("Coprocessor firmware update is in progress.", "err");
      } else {
        setGatewayOtaNote("Another firmware update is already in progress or the gateway is rebooting.", "err");
      }
    }
  } else if (target === "coprocessor" && !coprocOnline && !hasFile && !hasExplicitNote) {
    setGatewayOtaNote("Coprocessor appears offline. Once it responds to the gateway again, UART OTA will be available.", "");
  } else if (!hasFile) {
    setGatewayOtaNote("");
  }

  if (!gatewayOtaUploading && !busy) {
    if (target === "coprocessor") {
      if (!coprocOtaState.active && !coprocOtaState.message && !coprocOtaState.error) {
        resetGatewayOtaProgress();
      }
    } else if (!gatewayOtaBusy) {
      resetGatewayOtaProgress();
    }
  }
}

function uploadGatewayFirmwareNow(file) {
  const target = getGatewayOtaTarget();
  const targetLabel = getGatewayOtaTargetLabel(target);
  const xhr = new XMLHttpRequest();
  const formData = new FormData();
  formData.append("firmware", file, file.name);

  gatewayOtaUploading = true;
  setGatewayOtaNote(`Uploading ${targetLabel} firmware image...`, "");
  setGatewayOtaProgress(0, `Preparing ${file.name} (${fmtBytes(file.size)})`);
  refreshGatewayOtaUi();

  xhr.open("POST", "/api/gateway/ota", true);
  xhr.timeout = 180000;
  if (authToken) xhr.setRequestHeader("X-GW-Token", authToken);
  xhr.setRequestHeader("X-Firmware-Size", String(file.size));
  xhr.setRequestHeader("X-Target-MCU", target);

  xhr.upload.addEventListener("progress", event => {
    if (!event.lengthComputable) return;
    const rawPercent = event.total > 0 ? (event.loaded / event.total) : 0;
    const uploadCap = target === "coprocessor" ? 25 : 96;
    const displayPercent = Math.min(uploadCap, Math.round(rawPercent * uploadCap));
    const text = rawPercent >= 1
      ? target === "coprocessor"
        ? `Upload complete. Validating and staging ${file.name} for UART transfer...`
        : `Upload complete, validating and flashing ${file.name}...`
      : `Uploading ${file.name}: ${Math.round(rawPercent * 100)}%`;
    setGatewayOtaProgress(displayPercent, text);
  });

  xhr.addEventListener("load", () => {
    let response = xhr.response;
    if (!response || typeof response !== "object") {
      try { response = JSON.parse(xhr.responseText || "{}"); } catch { response = {}; }
    }

    if (xhr.status === 200 && response.ok) {
      gatewayOtaUploading = false;
      if (target === "coprocessor") {
        coprocOtaBusy = true;
        const stagedVersion = response.version ? ` v${response.version}` : "";
        setGatewayOtaProgress(25, `Firmware accepted${stagedVersion}. Waiting for UART OTA progress...`);
        setGatewayOtaNote(`Firmware accepted${stagedVersion}. The gateway is preparing the coprocessor OTA session...`, "ok");
        showToast(response.message || `Coprocessor firmware${stagedVersion} queued for UART OTA.`, "warn");
      } else {
        gatewayOtaBusy = true;
        const flashedVersion = response.version ? ` v${response.version}` : "";
        setGatewayOtaProgress(100, response.message || `Firmware${flashedVersion} update complete. Gateway rebooting...`);
        setGatewayOtaNote(`Firmware${flashedVersion} flashed successfully. Gateway is rebooting...`, "ok");
        showToast(response.message || `Gateway firmware${flashedVersion} update complete. Rebooting...`, "warn");
      }
      $gwOtaFileInput.value = "";
      refreshGatewayOtaUi();
      return;
    }

    gatewayOtaUploading = false;
    if (target === "coprocessor") {
      coprocOtaBusy = false;
    } else {
      gatewayOtaBusy = false;
    }
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
    if (target === "coprocessor") {
      coprocOtaBusy = false;
    } else {
      gatewayOtaBusy = false;
    }
    setGatewayOtaProgress(100, "Firmware upload failed.");
    setGatewayOtaNote("Network error while uploading firmware.", "err");
    showToast("Network error while uploading firmware.", "error");
    refreshGatewayOtaUi();
  });

  xhr.addEventListener("timeout", () => {
    gatewayOtaUploading = false;
    if (target === "coprocessor") {
      coprocOtaBusy = false;
    } else {
      gatewayOtaBusy = false;
    }
    setGatewayOtaProgress(100, "Firmware upload timed out.");
    setGatewayOtaNote(target === "coprocessor"
      ? "Upload timed out before the gateway finished staging the coprocessor firmware."
      : "Upload timed out before the gateway finished flashing.", "err");
    showToast("Firmware upload timed out.", "error");
    refreshGatewayOtaUi();
  });

  xhr.send(formData);
}

function uploadGatewayFirmware() {
  const target = getGatewayOtaTarget();
  const targetLabel = getGatewayOtaTargetLabel(target);
  const supported = getGatewayOtaSelectedSupported(target);
  const busy = getGatewayOtaSelectedBusy(target);
  const slotSize = target === "coprocessor" ? coprocOtaMaxBytes : gatewayOtaMaxBytes;
  const nodeOtaBlocksGatewaySection = isGatewayFirmwareUpdateBlockedByNodeOta();
  const coprocOtaActive = isCoprocessorReservedByCoprocOta();

  if (!$gwOtaFileInput?.files?.length) {
    setGatewayOtaNote("Select a .bin firmware file first.", "err");
    return;
  }
  if (!supported) {
    setGatewayOtaNote(target === "coprocessor"
      ? "Coprocessor OTA is unavailable until the coprocessor is running the OTA-capable helper firmware."
      : "OTA is not available on the current gateway partition layout.", "err");
    return;
  }
  if (nodeOtaBlocksGatewaySection) {
    setGatewayOtaNote(COPROC_BUSY_MSG, "err");
    return;
  }
  if (target === "main" && coprocOtaActive) {
    setGatewayOtaNote("Coprocessor firmware update is already in progress.", "err");
    return;
  }
  if (busy || gatewayOtaUploading) {
    setGatewayOtaNote(`${targetLabel} OTA is already busy.`, "err");
    return;
  }

  const file = $gwOtaFileInput.files[0];
  if (!/\.bin$/i.test(file.name)) {
    setGatewayOtaNote("Select a compiled .bin firmware file.", "err");
    return;
  }
  if (slotSize > 0 && file.size > slotSize) {
    setGatewayOtaNote(`Firmware is too large for the OTA slot (${fmtBytes(file.size)} > ${fmtBytes(slotSize)}).`, "err");
    return;
  }

  if (target === "coprocessor") {
    showConfirm({
      title: "Flash Gateway Coprocessor Firmware?",
      body: `You are about to update the <strong>${esc(targetLabel)}</strong> with <strong>${esc(file.name)}</strong> (${fmtBytes(file.size)}).<br><br>
             The gateway will validate the firmware, tell the coprocessor to halt its normal tasks, stream the image over the internal UART link, and then wait for the coprocessor to reboot and reconnect.`,
      okLabel: "Upload and Update",
      okClass: "warn",
      callback: () => uploadGatewayFirmwareNow(file)
    });
    return;
  }

  showConfirm({
    title: "Flash Gateway Firmware?",
    body: `You are about to flash <strong>${esc(file.name)}</strong> (${fmtBytes(file.size)}) to the <strong>${esc(targetLabel)}</strong>.<br><br>
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
      setGatewayOtaNote(`Ready for ${getGatewayOtaTargetLabel()}: ${file.name} (${fmtBytes(file.size)})`, "");
    } else {
      setGatewayOtaNote("");
      resetGatewayOtaProgress();
    }
    refreshGatewayOtaUi();
  });
}

if ($gwOtaTarget) {
  $gwOtaTarget.addEventListener("change", () => {
    setGatewayOtaNote("");
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
    const typeLabel = getNodeTypeLabel(n);
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
  refreshGatewayOtaUi();
}

function refreshNodeOtaUi() {
  populateNodeOtaTargets();
  const coprocReservedByCoprocOta = isCoprocessorReservedByCoprocOta();
  const coprocReservedByAnyOta = coprocReservedByCoprocOta || isCoprocessorReservedByNodeOta();
  if ($nodeOtaHelper) {
    $nodeOtaHelper.textContent = coprocReservedByAnyOta
      ? "Busy"
      : (nodeOtaHelperOnline ? "Online" : "Offline");
  }
  if ($nodeOtaHost) $nodeOtaHost.textContent = nodeOtaHost || "192.168.4.1";

  const selectedId = Number($nodeOtaNode?.value || 0);
  const selectedNode = selectedId ? nodes.get(selectedId) : null;
  if ($nodeOtaTarget) {
    $nodeOtaTarget.textContent = selectedNode
      ? `${selectedNode.name || `Node #${selectedNode.id}`} (${selectedNode.mac})`
      : "-";
  }

  const hasFile = !!$nodeOtaFileInput?.files?.length;
  const disabled = nodeOtaUploading || nodeOtaBusy || coprocReservedByCoprocOta || !selectedNode || !hasFile;
  if ($nodeOtaBtn) $nodeOtaBtn.disabled = disabled;
  if ($nodeOtaFileInput) $nodeOtaFileInput.disabled = nodeOtaUploading || nodeOtaBusy || coprocReservedByCoprocOta;
  if ($nodeOtaNode) $nodeOtaNode.disabled = nodeOtaUploading || nodeOtaBusy || coprocReservedByCoprocOta;

  if ($nodeOtaBtn) {
    $nodeOtaBtn.textContent = nodeOtaUploading
      ? "Uploading..."
      : coprocReservedByCoprocOta
        ? "Coprocessor Busy"
      : nodeOtaBusy
        ? "Node OTA Busy"
        : "Upload Node Firmware";
  }

  if (nodeOtaUploading) return;
  if (coprocReservedByCoprocOta) {
    setNodeOtaNote(COPROC_BUSY_MSG, "err");
  } else if (nodeOtaBusy && !nodeOtaState.error) {
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
  if (isCoprocessorReservedByCoprocOta()) {
    setNodeOtaNote(COPROC_BUSY_MSG, "err");
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
  closeSidebar();
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
      $gwFactoryButtons.forEach(btn => { btn.disabled = true; });
    }
  });
}


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
  if (nodeHasActuators(n) && !nodeActuatorSchemas.has(nodeId)) {
    send({ type: "node_actuator_schema_get", node_id: nodeId });
  }
  if (nodeHasRfid(n) && !nodeRfidConfigs.has(nodeId)) {
    send({ type: "node_rfid_config_get", node_id: nodeId });
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
  if (nodeHasActuators(n) && !nodeActuatorSchemas.has(nodeId)) {
    send({ type: "node_actuator_schema_get", node_id: nodeId });
  }
  if (nodeHasRfid(n) && !nodeRfidConfigs.has(nodeId)) {
    send({ type: "node_rfid_config_get", node_id: nodeId });
  }
}

function closeNodeSettings(clearDraft = true) {
  if (clearDraft && nsCurrentNodeId) clearRelayLabelDraft(nsCurrentNodeId);
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
  if (relayInput) return parseInt(relayInput.dataset.node, 10) === nodeId;

  const rfidInput = active?.closest?.(".rfid-uid-input");
  if (rfidInput) return parseInt(rfidInput.dataset.nodeId, 10) === nodeId;

  return false;
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

  if (nodeHasActuators(n)) sections.push(buildRelayLabelSection(n));
  if (nodeHasRfid(n)) sections.push(buildRfidSection(n));

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
  } else if (nodeHasSensorData(n)) {
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
  if (!n || !nodeHasActuators(n)) return;

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
  if (!n || !nodeHasActuators(n)) return;

  const defaults = Array.from({ length: getActuatorCountForNode(n) }, (_, i) => defaultRelayLabel(i));
  replaceRelayLabelDraft(nodeId, defaults);

  collectRelayLabelInputs(nodeId).forEach((input, index) => {
    input.value = defaults[index];
  });
  setRelayLabelSaveNote(nodeId, "Saving default labels to gateway...", "");
  send({ type: "relay_labels_set", node_id: nodeId, labels: defaults });
}

function saveRfidConfig(nodeId) {
  const node = nodes.get(nodeId);
  if (!node || !nodeHasRfid(node)) return;

  const state = getRfidEditorState(nodeId);
  if (!isValidRfidUidInput(state.uid || "")) {
    setRfidSaveNote(nodeId, "RFID UID must be 4, 7, or 10 bytes in hex.", "err");
    showToast("Invalid RFID UID.", "error");
    return;
  }

  setRfidSaveNote(nodeId, "Saving to node...", "");
  send({
    type: "node_rfid_config_set",
    node_id: nodeId,
    slot: Number(state.selectedSlot || 0),
    enabled: true,
    uid: compactRfidUid(state.uid),
    relay_mask: Number(state.relayMask || 0)
  });
}

function deleteRfidConfig(nodeId) {
  const node = nodes.get(nodeId);
  if (!node || !nodeHasRfid(node)) return;

  const state = getRfidEditorState(nodeId);
  setRfidSaveNote(nodeId, "Deleting slot...", "");
  send({
    type: "node_rfid_config_set",
    node_id: nodeId,
    slot: Number(state.selectedSlot || 0),
    enabled: false,
    uid: "",
    relay_mask: 0
  });
  loadRfidEditorSlot(nodeId, Number(state.selectedSlot || 0));
  if (nsCurrentNodeId === nodeId) renderNodeSettings(nodeId);
}

// ── Toast ─────────────────────────────────────────────────────────────────────
let toastTimer = null;
function showToast(msg, type = "success", durationMs = 4000) {
  $toast.textContent = msg;
  $toast.className = `toast ${type}`;
  $toast.style.display = "";
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { $toast.style.display = "none"; }, durationMs);
}


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
  const sidebarCloseBtn = e.target.closest("#sidebar-close-btn");
  if (sidebarCloseBtn) {
    e.preventDefault();
    e.stopPropagation();
    closeSidebar();
    return;
  }

  const gwRebootBtn = e.target.closest("#gw-reboot-btn, #gw-reboot-btn-mobile");
  if (gwRebootBtn && !gwRebootBtn.disabled) {
    e.preventDefault();
    sendGatewayReboot();
    return;
  }

  const gwFactoryBtn = e.target.closest("#gw-factory-btn, #gw-factory-btn-mobile");
  if (gwFactoryBtn && !gwFactoryBtn.disabled) {
    e.preventDefault();
    factoryReset();
    return;
  }

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

  const rfidToggleBtn = e.target.closest(".rfid-relay-toggle");
  if (rfidToggleBtn) {
    const nodeId = parseInt(rfidToggleBtn.dataset.nodeId, 10);
    const relayIndex = parseInt(rfidToggleBtn.dataset.relay, 10);
    const state = getRfidEditorState(nodeId);
    state.relayMask = Number(state.relayMask || 0) ^ (1 << relayIndex);
    rfidEditorState.set(nodeId, state);
    if (nsCurrentNodeId === nodeId) renderNodeSettings(nodeId);
    return;
  }

  const rfidUseLastBtn = e.target.closest(".rfid-use-last-btn");
  if (rfidUseLastBtn) {
    const nodeId = parseInt(rfidUseLastBtn.dataset.nodeId, 10);
    const cfg = nodeRfidConfigs.get(nodeId);
    if (!cfg?.last_scan?.uid) return;
    const slot = Number(cfg.last_scan.matched_slot) >= 0
      ? Number(cfg.last_scan.matched_slot)
      : getRfidEditorState(nodeId).selectedSlot;
    const state = loadRfidEditorSlot(nodeId, slot);
    state.uid = normalizeRfidUidInput(cfg.last_scan.uid);
    rfidEditorState.set(nodeId, state);
    if (nsCurrentNodeId === nodeId) renderNodeSettings(nodeId);
    return;
  }

  const rfidSaveBtn = e.target.closest(".rfid-save-btn");
  if (rfidSaveBtn) {
    saveRfidConfig(parseInt(rfidSaveBtn.dataset.nodeId, 10));
    return;
  }

  const rfidDeleteBtn = e.target.closest(".rfid-delete-btn");
  if (rfidDeleteBtn) {
    deleteRfidConfig(parseInt(rfidDeleteBtn.dataset.nodeId, 10));
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

  const rfidInput = e.target.closest(".rfid-uid-input");
  if (e.key === "Enter" && rfidInput) {
    e.preventDefault();
    saveRfidConfig(parseInt(rfidInput.dataset.nodeId, 10));
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
    if (sel.classList.contains("rfid-slot-select")) {
      const nodeId = parseInt(sel.dataset.nodeId, 10);
      loadRfidEditorSlot(nodeId, parseInt(sel.value, 10));
      if (nsCurrentNodeId === nodeId) renderNodeSettings(nodeId);
      return;
    }
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
  if (relayInput) {
    setRelayLabelDraft(
      parseInt(relayInput.dataset.node, 10),
      parseInt(relayInput.dataset.relay, 10),
      relayInput.value
    );
    setRelayLabelSaveNote(parseInt(relayInput.dataset.node, 10), "");
    return;
  }

  const rfidInput = e.target.closest(".rfid-uid-input");
  if (rfidInput) {
    const nodeId = parseInt(rfidInput.dataset.nodeId, 10);
    const state = getRfidEditorState(nodeId);
    state.uid = normalizeRfidUidInput(rfidInput.value);
    rfidEditorState.set(nodeId, state);
    rfidInput.value = state.uid;
    setRfidSaveNote(nodeId, "");
  }
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

if ($gwBuiltinSensorSaveBtn) {
  $gwBuiltinSensorSaveBtn.addEventListener("click", saveGatewayBuiltinSensorSettings);
}
if ($gwBuiltinTempUnit) {
  $gwBuiltinTempUnit.addEventListener("change", () => {
    gatewayBuiltinSensorDraft.tempUnit = $gwBuiltinTempUnit.value || "C";
    markGatewayBuiltinSensorDraftDirty();
    setGatewayBuiltinSensorSaveNote("");
  });
}
if ($gwBuiltinAltitudeValue) {
  $gwBuiltinAltitudeValue.addEventListener("input", () => {
    gatewayBuiltinSensorDraft.altitudeValueText = $gwBuiltinAltitudeValue.value;
    markGatewayBuiltinSensorDraftDirty();
    setGatewayBuiltinSensorSaveNote("");
  });
}
if ($gwBuiltinAltitudeUnit) {
  $gwBuiltinAltitudeUnit.addEventListener("change", () => {
    const prevUnit = $gwBuiltinAltitudeUnit.dataset.prevValue || gatewayBuiltinSensor.altitudeUnit || "m";
    const nextUnit = $gwBuiltinAltitudeUnit.value || "m";
    const currentValue = Number($gwBuiltinAltitudeValue?.value ?? "0");
    const converted = convertGatewayBuiltinAltitudeValue(currentValue, prevUnit, nextUnit);
    if ($gwBuiltinAltitudeValue && Number.isFinite(converted)) {
      $gwBuiltinAltitudeValue.value = converted.toFixed(1);
    }
    $gwBuiltinAltitudeUnit.dataset.prevValue = nextUnit;
    gatewayBuiltinSensorDraft.altitudeUnit = nextUnit;
    gatewayBuiltinSensorDraft.altitudeValueText = $gwBuiltinAltitudeValue?.value ?? "";
    markGatewayBuiltinSensorDraftDirty();
    setGatewayBuiltinSensorSaveNote("");
  });
}

if ($mqttEnabledToggle) {
  $mqttEnabledToggle.addEventListener("change", () => {
    gatewayMqttDraft.enabled = !!$mqttEnabledToggle.checked;
    markGatewayMqttDraftDirty();
    setMqttSaveNote("");
  });
}
if ($mqttHostInput) {
  $mqttHostInput.addEventListener("input", () => {
    gatewayMqttDraft.host = $mqttHostInput.value;
    markGatewayMqttDraftDirty();
    setMqttSaveNote("");
  });
}
if ($mqttPortInput) {
  $mqttPortInput.addEventListener("input", () => {
    gatewayMqttDraft.portText = $mqttPortInput.value;
    markGatewayMqttDraftDirty();
    setMqttSaveNote("");
  });
}
if ($mqttBaseTopicInput) {
  $mqttBaseTopicInput.addEventListener("input", () => {
    gatewayMqttDraft.baseTopic = $mqttBaseTopicInput.value;
    markGatewayMqttDraftDirty();
    setMqttSaveNote("");
  });
}
if ($mqttUserInput) {
  $mqttUserInput.addEventListener("input", () => {
    gatewayMqttDraft.username = $mqttUserInput.value;
    markGatewayMqttDraftDirty();
    setMqttSaveNote("");
  });
}
if ($mqttPassInput) {
  $mqttPassInput.addEventListener("input", () => {
    gatewayMqttDraft.password = $mqttPassInput.value;
    markGatewayMqttDraftDirty();
    setMqttSaveNote("");
  });
}
if ($mqttSaveBtn) {
  $mqttSaveBtn.addEventListener("click", saveMqttConfig);
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
