// ---------- HARDCODED BROKER -------------------------------------------------
const MQTT_HOST   = '06a2ba7fa5274bd89278d9107b2f4f8b.s1.eu.hivemq.cloud';
const MQTT_PORT   = 8884;
const DEVICE_ID   = 'car001';
// -----------------------------------------------------------------------------

const CREDS_KEY = 'carsec.creds.v1';

function loadCreds() {
  try { return JSON.parse(localStorage.getItem(CREDS_KEY)) || null; }
  catch { return null; }
}
function saveCreds(c) { localStorage.setItem(CREDS_KEY, JSON.stringify(c)); }

// ---------- DOM refs ---------------------------------------------------------
const $ = (id) => document.getElementById(id);
const els = {
  status: $('status'), statusBox: $('statusBox'),
  door: $('door'), engine: $('engine'), net: $('net'),
  lat: $('lat'), lon: $('lon'), heap: $('heap'),
  connDot: $('connDot'), connText: $('connText'), deviceId: $('deviceId'),
  notification: $('notification'), notifText: $('notifText'),
  armBtn: $('armBtn'), disarmBtn: $('disarmBtn'),
  loginBtn: $('settingsBtn'), loginModal: $('settingsModal'),
  cfgUser: $('cfgUser'), cfgPass: $('cfgPass'),
  cfgSave: $('cfgSave'), cfgCancel: $('cfgCancel'),
};

els.deviceId.innerText = DEVICE_ID;

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
const TOPICS = {
  state:  `carsec/${DEVICE_ID}/state`,
  alarm:  `carsec/${DEVICE_ID}/alarm`,
  online: `carsec/${DEVICE_ID}/online`,
  cmd:    `carsec/${DEVICE_ID}/cmd`,
};

function connect(creds) {
  if (client) { try { client.end(true); } catch {} client = null; }

  const url = `wss://${MQTT_HOST}:${MQTT_PORT}/mqtt`;
  setConnState('connecting', 'Connecting…');

  client = mqtt.connect(url, {
    username: creds.user,
    password: creds.pass,
    clientId: 'dash-' + Math.random().toString(16).slice(2, 10),
    clean: true,
    reconnectPeriod: 3000,
    connectTimeout: 10000,
  });

  client.on('connect', () => {
    setConnState('connected', 'Connected');
    client.subscribe([TOPICS.state, TOPICS.alarm, TOPICS.online], { qos: 1 });
  });

  client.on('reconnect', () => setConnState('connecting', 'Reconnecting…'));
  client.on('close',     () => setConnState('error', 'Disconnected'));
  client.on('error',     (e) => {
    console.error(e);
    setConnState('error', 'Error: ' + e.message);
  });

  client.on('message', (topic, payload) => {
    const msg = payload.toString();

    if (topic === TOPICS.state) {
      try { applyState(JSON.parse(msg)); }
      catch (e) { console.warn('Bad state payload', msg); }
    }
    else if (topic === TOPICS.alarm) {
      if (msg === '1') {
        els.statusBox.classList.add('alarm-active');
        showNotification('INTRUSION DETECTED!');
      }
    }
    else if (topic === TOPICS.online) {
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
  client.publish(TOPICS.cmd, cmd, { qos: 1 });
}

// ---------- Login modal -----------------------------------------------------
function openLogin(creds) {
  els.cfgUser.value = creds?.user || '';
  els.cfgPass.value = creds?.pass || '';
  els.loginModal.hidden = false;
  setTimeout(() => els.cfgUser.focus(), 50);
}
function closeLogin() { els.loginModal.hidden = true; }

els.loginBtn.addEventListener('click', () => openLogin(loadCreds()));
els.cfgCancel.addEventListener('click', closeLogin);
els.cfgSave.addEventListener('click', () => {
  const creds = {
    user: els.cfgUser.value.trim(),
    pass: els.cfgPass.value,
  };
  if (!creds.user || !creds.pass) { alert('Username and password are required.'); return; }
  saveCreds(creds);
  closeLogin();
  connect(creds);
});

// Enter key submits
[els.cfgUser, els.cfgPass].forEach(el =>
  el.addEventListener('keydown', e => { if (e.key === 'Enter') els.cfgSave.click(); })
);

els.armBtn.addEventListener('click',    () => sendCmd('ARM'));
els.disarmBtn.addEventListener('click', () => sendCmd('DISARM'));

// ---------- Boot ------------------------------------------------------------
(function init() {
  const creds = loadCreds();
  if (creds && creds.user) {
    connect(creds);
  } else {
    setConnState('error', 'Not signed in');
    openLogin(null);
  }
})();