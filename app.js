// =============================================================================
//  Car Security Dashboard — MQTT over WebSockets (HiveMQ Cloud)
//
//  Subscribes:
//    carsec/<id>/state    JSON state snapshot
//    carsec/<id>/alarm    "1" on intrusion edge
//    carsec/<id>/online   "true" | "false"   (LWT)
//
//  Publishes:
//    carsec/<id>/cmd      "ARM" | "DISARM"
// =============================================================================

const CONFIG_KEY = 'carsec.config.v1';

function loadConfig() {
  try { return JSON.parse(localStorage.getItem(CONFIG_KEY)) || null; }
  catch { return null; }
}
function saveConfig(cfg) { localStorage.setItem(CONFIG_KEY, JSON.stringify(cfg)); }

// ---------- DOM refs ---------------------------------------------------------
const $ = (id) => document.getElementById(id);
const els = {
  status: $('status'), statusBox: $('statusBox'),
  door: $('door'), engine: $('engine'), net: $('net'),
  lat: $('lat'), lon: $('lon'), heap: $('heap'),
  connDot: $('connDot'), connText: $('connText'), deviceId: $('deviceId'),
  notification: $('notification'), notifText: $('notifText'),
  armBtn: $('armBtn'), disarmBtn: $('disarmBtn'),
  settingsBtn: $('settingsBtn'), settingsModal: $('settingsModal'),
  cfgHost: $('cfgHost'), cfgPort: $('cfgPort'), cfgUser: $('cfgUser'),
  cfgPass: $('cfgPass'), cfgDevice: $('cfgDevice'),
  cfgSave: $('cfgSave'), cfgCancel: $('cfgCancel'),
};

// ---------- Map smoothing (same logic that lived in the ESP HTML) -----------
let lastMapLat = 0, lastMapLon = 0;
const latHist = [], lonHist = [];
const SMOOTH_N  = 5;
const THRESHOLD = 0.00005;

function smooth(hist, val) {
  hist.push(val);
  if (hist.length > SMOOTH_N) hist.shift();
  return hist.reduce((a, b) => a + b, 0) / hist.length;
}
function updateMap(rawLat, rawLon) {
  if (rawLat === 0 && rawLon === 0) return;
  const lat = smooth(latHist, rawLat);
  const lon = smooth(lonHist, rawLon);
  if (lastMapLat !== 0 &&
      Math.abs(lat - lastMapLat) < THRESHOLD &&
      Math.abs(lon - lastMapLon) < THRESHOLD) return;
  lastMapLat = lat;
  lastMapLon = lon;
  const url = 'https://www.openstreetmap.org/export/embed.html'
            + '?bbox=' + (lon-0.01)+','+(lat-0.01)+','+(lon+0.01)+','+(lat+0.01)
            + '&layer=mapnik&marker=' + lat + ',' + lon;
  const mapDiv = $('map');
  const iframe = mapDiv.querySelector('iframe');
  if (iframe) iframe.src = url;
  else mapDiv.innerHTML = '<iframe src="' + url + '"></iframe>';
}

function showNotification(msg) {
  els.notifText.innerText = msg;
  els.notification.classList.add('show');
  setTimeout(() => els.notification.classList.remove('show'), 5000);
}

function setConnState(state, text) {
  els.connDot.className = 'conn-dot ' + state;
  els.connText.innerText = text;
}

// ---------- MQTT -------------------------------------------------------------
let client = null;
let topics = null;

function topicsFor(deviceId) {
  const base = `carsec/${deviceId}`;
  return {
    state:  `${base}/state`,
    alarm:  `${base}/alarm`,
    online: `${base}/online`,
    cmd:    `${base}/cmd`,
  };
}

function connect(cfg) {
  if (client) { try { client.end(true); } catch {} client = null; }

  topics = topicsFor(cfg.device);
  els.deviceId.innerText = cfg.device;

  const url = `wss://${cfg.host}:${cfg.port}/mqtt`;
  setConnState('connecting', `Connecting to ${cfg.host}…`);

  client = mqtt.connect(url, {
    username: cfg.user,
    password: cfg.pass,
    clientId: 'dash-' + Math.random().toString(16).slice(2, 10),
    clean: true,
    reconnectPeriod: 3000,
    connectTimeout: 10000,
  });

  client.on('connect', () => {
    setConnState('connected', 'Connected');
    client.subscribe([topics.state, topics.alarm, topics.online], { qos: 1 });
  });

  client.on('reconnect', () => setConnState('connecting', 'Reconnecting…'));
  client.on('close',     () => setConnState('error', 'Disconnected'));
  client.on('error',     (e) => {
    console.error(e);
    setConnState('error', 'Error: ' + e.message);
  });

  client.on('message', (topic, payload) => {
    const msg = payload.toString();

    if (topic === topics.state) {
      try { applyState(JSON.parse(msg)); }
      catch (e) { console.warn('Bad state payload', msg); }
    }
    else if (topic === topics.alarm) {
      if (msg === '1') {
        els.statusBox.classList.add('alarm-active');
        showNotification('INTRUSION DETECTED!');
      }
    }
    else if (topic === topics.online) {
      if (msg === 'false') {
        setConnState('error', 'Device offline');
      } else if (msg === 'true' && client.connected) {
        setConnState('connected', 'Connected');
      }
    }
  });
}

function applyState(d) {
  if (d.status !== undefined) {
    els.status.innerText = d.status;
    if (d.status === 'ALARM') {
      els.statusBox.classList.add('alarm-active');
      if (!els.notification.classList.contains('show'))
        showNotification('INTRUSION DETECTED!');
    } else {
      els.statusBox.classList.remove('alarm-active');
    }
  }
  if (d.door !== undefined) {
    els.door.innerText = d.door;
    els.door.className = 'status-value ' + (d.door === 'OPEN' ? 'door-open' : 'door-closed');
  }
  if (d.engine !== undefined) {
    els.engine.innerText = d.engine;
    els.engine.className = 'status-value ' + (d.engine === 'ON' ? 'engine-on' : 'engine-off');
  }
  if (d.net !== undefined) {
    els.net.innerText = d.net;
    els.net.className = 'status-value ' + (d.net === 'OK' ? 'net-ok' : 'net-lost');
  }
  if (d.heap !== undefined) els.heap.innerText = d.heap;

  if (typeof d.lat === 'number' && typeof d.lon === 'number') {
    els.lat.innerText = d.lat.toFixed(6);
    els.lon.innerText = d.lon.toFixed(6);
    updateMap(d.lat, d.lon);
  }
}

function sendCmd(cmd) {
  if (!client || !client.connected) {
    showNotification('Not connected to broker.');
    return;
  }
  client.publish(topics.cmd, cmd, { qos: 1 });
}

// ---------- Settings modal --------------------------------------------------
function openSettings(cfg) {
  els.cfgHost.value   = cfg?.host   || '';
  els.cfgPort.value   = cfg?.port   || 8884;
  els.cfgUser.value   = cfg?.user   || '';
  els.cfgPass.value   = cfg?.pass   || '';
  els.cfgDevice.value = cfg?.device || 'car001';
  els.settingsModal.hidden = false;
}
function closeSettings() { els.settingsModal.hidden = true; }

els.settingsBtn.addEventListener('click', () => openSettings(loadConfig()));
els.cfgCancel.addEventListener('click', closeSettings);
els.cfgSave.addEventListener('click', () => {
  const cfg = {
    host:   els.cfgHost.value.trim(),
    port:   parseInt(els.cfgPort.value, 10) || 8884,
    user:   els.cfgUser.value,
    pass:   els.cfgPass.value,
    device: els.cfgDevice.value.trim() || 'car001',
  };
  if (!cfg.host || !cfg.user) { alert('Host and username are required.'); return; }
  saveConfig(cfg);
  closeSettings();
  connect(cfg);
});

els.armBtn.addEventListener('click',    () => sendCmd('ARM'));
els.disarmBtn.addEventListener('click', () => sendCmd('DISARM'));

// ---------- Boot ------------------------------------------------------------
(function init() {
  const cfg = loadConfig();
  if (cfg && cfg.host) {
    connect(cfg);
  } else {
    setConnState('error', 'Not configured');
    openSettings(null);
  }
})();
