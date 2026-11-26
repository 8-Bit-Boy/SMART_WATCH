//All the Bluetooth, weather and notification logic
import { PermissionsAndroid, Platform } from 'react-native';
import { Buffer } from 'buffer';
import * as Location from 'expo-location';
import { onNotification } from './notifications';
import { BleManager, ScanMode } from 'react-native-ble-plx';

const SERVICE = '12345678-1234-5678-1234-56789abcdef0';
const MPU     = '12345678-1234-5678-1234-56789abcdef2';
const BATT    = '12345678-1234-5678-1234-56789abcdef3';
const RX      = '12345678-1234-5678-1234-56789abcdef4';
const TIME    = '12345678-1234-5678-1234-56789abcdef5';
const WEATHER = '12345678-1234-5678-1234-56789abcdef6';

const RETRY_MS = 10 * 1000;
const SCAN_MS = 8 * 1000;
const WEATHER_REFRESH_MS = 60 * 60 * 1000;

// used only if the phone's location can't be read and nothing is cached
const DEFAULT_LOCATION = { lat: -25.7545, lon: 28.2314 };   // Pretoria

const weatherUrl = (lat, lon) =>
  `https://api.open-meteo.com/v1/forecast?latitude=${lat}&longitude=${lon}` +
  `&current=temperature_2m,relative_humidity_2m,weather_code`;

const manager = globalThis.__bleManager ?? (globalThis.__bleManager = new BleManager());
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---------- state the UI subscribes to ----------
const state = {
  status: 'Starting...',
  accel: { x: 0, y: 0, z: 0 },
  battery: null,
  weather: null,
  location: null,
};
const subs = new Set();
const set = (patch) => {
  Object.assign(state, patch);
  subs.forEach((fn) => fn({ ...state }));
};
export const subscribe = (fn) => {
  subs.add(fn);
  fn({ ...state });
  return () => subs.delete(fn);
};

// ---------- internals ----------
let device = null;
let busy = false;
let permsOk = false;
let started = false;
let scanTimeout = null;
let weatherTimer = null;
let retryTimer = null;
let unsubNotif = null;
let lastLoc = null;
let knownId = null;            // the watch's address once found
let fails = 0;                 // consecutive failed connects
let scanBlockedUntil = 0;      // back-off after a scan error

export async function ensureBlePermissions() {
  if (permsOk) return true;
  if (Platform.OS !== 'android') return true;
  if (Platform.Version >= 31) {
    const r = await PermissionsAndroid.requestMultiple([
      PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
      PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
    ]);
    permsOk = Object.values(r).every((v) => v === 'granted');
  } else {
    const r = await PermissionsAndroid.request(
      PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION
    );
    permsOk = r === 'granted';
  }
  return permsOk;
}

// map weather codes the watch doesn't have text for onto ones it does
function toWatchCode(code) {
  if ([56, 57].includes(code)) return 51;
  if ([66, 67].includes(code)) return 61;
  if (code === 80) return 61;
  if (code === 81) return 63;
  if (code === 82) return 65;
  if (code === 77 || code === 85 || code === 86) return 71;
  if (code === 96 || code === 99) return 95;
  return code;
}

async function getPhoneLocation() {
  const perm = await Location.requestForegroundPermissionsAsync();
  if (perm.status !== 'granted') throw new Error('Location permission denied');
  const pos = await Location.getCurrentPositionAsync({ accuracy: Location.Accuracy.Low });
  return { lat: pos.coords.latitude, lon: pos.coords.longitude };
}

async function fetchWeather(lat, lon) {
  const res = await fetch(weatherUrl(lat, lon));
  if (!res.ok) throw new Error('Weather HTTP ' + res.status);
  const c = (await res.json()).current;
  return {
    temp: c.temperature_2m,
    humidity: Math.round(c.relative_humidity_2m),
    code: c.weather_code,
  };
}

// BLE values travel as base64 strings
const writeText = (uuid, text) => {
  if (!device) return Promise.reject(new Error('Not connected'));
  return device.writeCharacteristicWithResponseForService(
    SERVICE, uuid, Buffer.from(text, 'utf8').toString('base64')
  );
};

async function sendWeather() {
  try {
    // phone location if available, else last good fix, else the default
    let loc = lastLoc ?? DEFAULT_LOCATION;
    try {
      loc = await getPhoneLocation();
      lastLoc = loc;
    } catch (e) {
      set({ status: 'Location unavailable, using ' + (lastLoc ? 'last known' : 'default') });
    }
    set({ location: loc });

    const w = await fetchWeather(loc.lat, loc.lon);
    set({ weather: w });

    if (!device) return;
    await writeText(WEATHER, `${w.temp.toFixed(1)},${w.humidity},${toWatchCode(w.code)}`);
  } catch (e) {
    set({ status: 'Weather error: ' + e.message });
  }
}

// scans only to find the watch the first time (or after repeated failures)
async function findWatch() {
  if (Date.now() < scanBlockedUntil) {
    set({ status: 'Waiting before scanning again...' });
    return null;
  }

  // 1. does the phone already hold a link to the watch? (then it isn't advertising)
  try {
    const existing = await manager.connectedDevices([SERVICE]);
    if (existing.length > 0) {
      set({ status: 'Found existing link' });
      return existing[0].id;
    }
  } catch (_) {}

  // 2. otherwise scan for it
  return new Promise((resolve) => {
    let done = false;
    const finish = (id) => {
      if (done) return;
      done = true;
      clearTimeout(scanTimeout);
      manager.stopDeviceScan();
      resolve(id);
    };

    set({ status: 'Scanning...' });
    manager.stopDeviceScan();

    manager.startDeviceScan([SERVICE], { scanMode: ScanMode.LowLatency }, (error, dev) => {
      if (error) {
        scanBlockedUntil = Date.now() + 30000;
        console.log('Scan error:', error.message);
        set({ status: 'Scan error: ' + error.message });
        finish(null);
        return;
      }
      console.log('Found watch:', dev.id);
      finish(dev.id);
    });

    scanTimeout = setTimeout(() => finish(null), SCAN_MS);
  });
}

// connects straight to a known address, no scanning
async function connectTo(id) {
  set({ status: 'Connecting...' });
  await manager.cancelDeviceConnection(id).catch(() => {});   // clear any stale link
  await sleep(300);
  const d = await manager.connectToDevice(id, { timeout: 10000 });
  await sleep(500);
  await d.discoverAllServicesAndCharacteristics();

  device = d;
  fails = 0;
  set({ status: 'Connected' });

  d.onDisconnected((err) => {
    device = null;
    clearInterval(weatherTimer);
    set({ status: 'Disconnected' + (err ? ': ' + err.message : '') + ', retrying...' });
  });

  await writeText(TIME, String(Math.floor(Date.now() / 1000)));

  sendWeather();
  clearInterval(weatherTimer);
  weatherTimer = setInterval(sendWeather, WEATHER_REFRESH_MS);

  d.monitorCharacteristicForService(SERVICE, MPU, (err, c) => {
    if (err || !c?.value) return;
    const b = Buffer.from(c.value, 'base64');
    set({ accel: { x: b.readInt16LE(0) / 100, y: b.readInt16LE(2) / 100, z: b.readInt16LE(4) / 100 } });
  });

  d.monitorCharacteristicForService(SERVICE, BATT, (err, c) => {
    if (err || !c?.value) return;
    set({ battery: Buffer.from(c.value, 'base64').readUInt8(0) });
  });
}

async function attemptConnect() {
  if (device || busy) return;
  busy = true;

  try {
    if (!(await ensureBlePermissions())) {
      set({ status: 'Permissions denied' });
      return;
    }

    if (!knownId) knownId = await findWatch();        // scan only when we don't know the watch
    if (!knownId) {
      set({ status: 'Watch not found, retrying...' });
      return;
    }

    await connectTo(knownId);
  } catch (e) {
    console.log('Connect error:', e.message);
    set({ status: 'Connect failed: ' + e.message });
    if (knownId) { try { await manager.cancelDeviceConnection(knownId); } catch (_) {} }

    // after 3 failures in a row, forget the address and scan again next time
    if (++fails >= 3) { knownId = null; fails = 0; }
  } finally {
    busy = false;
  }
}

// ---------- public controls ----------
export function startWatchService() {
  if (started) return;
  started = true;

  // clean up anything left over from a previous (hot-reloaded) copy of this file
  if (globalThis.__watchRetry) clearInterval(globalThis.__watchRetry);
  if (globalThis.__watchUnsubNotif) globalThis.__watchUnsubNotif();

  attemptConnect();
  retryTimer = setInterval(attemptConnect, RETRY_MS);
  globalThis.__watchRetry = retryTimer;

  unsubNotif = onNotification((n) => {
    if (!device) return;
    const text = String(n.title || n.app || 'New').slice(0, 20);
    writeText(RX, text).catch(() => {});
  });
  globalThis.__watchUnsubNotif = unsubNotif;
}

export function stopWatchService() {
  started = false;
  clearInterval(retryTimer);
  clearTimeout(scanTimeout);
  clearInterval(weatherTimer);
  manager.stopDeviceScan();
  if (unsubNotif) unsubNotif();
  if (device) device.cancelConnection().catch(() => {});
  device = null;
  busy = false;
}

export const connectNow = attemptConnect;
export const sendWeatherNow = sendWeather;
export const sendTestMessage = () => writeText(RX, 'hello').catch((e) => set({ status: e.message }));