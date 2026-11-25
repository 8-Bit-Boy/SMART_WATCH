import { useEffect, useRef, useState } from 'react';
import { View, Text, Button, StyleSheet, PermissionsAndroid, Platform } from 'react-native';
import { BleManager } from 'react-native-ble-plx';
import { Buffer } from 'buffer';
import RNAndroidNotificationListener from 'react-native-android-notification-listener';
import { onNotification } from './notifications';
import * as Location from 'expo-location';

const SERVICE = '12345678-1234-5678-1234-56789abcdef0';
const MPU     = '12345678-1234-5678-1234-56789abcdef2';
const BATT    = '12345678-1234-5678-1234-56789abcdef3';
const RX      = '12345678-1234-5678-1234-56789abcdef4';
const TIME    = '12345678-1234-5678-1234-56789abcdef5';
const WEATHER = '12345678-1234-5678-1234-56789abcdef6';

// reconnect timing
const RETRY_MS = 10 * 1000;   // try again every 10 s
const SCAN_MS  = 5 * 1000;    // each attempt scans for 5 s
const WEATHER_REFRESH_MS = 60 * 60 * 1000;   // 1 hour


// used only if the phone's location can't be read and nothing is cached
const DEFAULT_LOCATION = { lat: -25.7545, lon: 28.2314 };   // Pretoria
//cape town location , for testing. Uncomment for final run
// const DEFAULT_LOCATION = { lat: -33.9249, lon: 18.4241 };   // Cape Town (test only)

const weatherUrl = (lat, lon) =>
  `https://api.open-meteo.com/v1/forecast?latitude=${lat}&longitude=${lon}` +
  `&current=temperature_2m,relative_humidity_2m,weather_code`;


const manager = new BleManager();


async function requestPermissions() {
  if (Platform.OS !== 'android') return true;
  if (Platform.Version >= 31) {
    const r = await PermissionsAndroid.requestMultiple([
      PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
      PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
    ]);
    return Object.values(r).every((v) => v === 'granted');
  }
  const r = await PermissionsAndroid.request(
    PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION
  );
  return r === 'granted';
}

// The watch only has text for some weather codes, so map the others
// onto the closest one it knows (otherwise it would show "Unknown")
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

  // Low accuracy (about city level) is plenty for weather and uses much less battery
  const pos = await Location.getCurrentPositionAsync({ accuracy: Location.Accuracy.Low });
  return { lat: pos.coords.latitude, lon: pos.coords.longitude };
}

async function fetchWeather(lat, lon) {
  const res = await fetch(weatherUrl(lat, lon));
  if (!res.ok) throw new Error('Weather HTTP ' + res.status);
  const json = await res.json();
  const c = json.current;
  return {
    temp: c.temperature_2m,
    humidity: Math.round(c.relative_humidity_2m),
    code: c.weather_code,
  };
}

export default function App() {
  const [status, setStatus] = useState('Starting...');
  const [accel, setAccel] = useState({ x: 0, y: 0, z: 0 });
  const [battery, setBattery] = useState(null);
  const [weather, setWeather] = useState(null);
  const [location, setLocation] = useState(null);

  const lastLocRef = useRef(null);        // last location that worked
  const deviceRef = useRef(null);       // the connected watch, or null
  const busyRef = useRef(false);        // true while scanning or connecting
  const permsRef = useRef(false);       // permissions already granted?
  const scanTimeout = useRef(null);
  const weatherTimer = useRef(null);

  // BLE values travel as base64 strings, so encode text before writing
  const writeText = (uuid, text) =>
    deviceRef.current.writeCharacteristicWithResponseForService(
      SERVICE, uuid, Buffer.from(text, 'utf8').toString('base64')
    );

  // fetch real weather, show it in the app, and send it to the watch
  const sendWeather = async () => {
    try {
      // 1. where is the phone now? fall back to the last good fix, then the default
      let loc = lastLocRef.current ?? DEFAULT_LOCATION;
      try {
        loc = await getPhoneLocation();
        lastLocRef.current = loc;
      } catch (e) {
        setStatus('Location unavailable, using ' + (lastLocRef.current ? 'last known' : 'default'));
      }
      setLocation(loc);

      // 2. weather for that spot
      const w = await fetchWeather(loc.lat, loc.lon);
      setWeather(w);

      // 3. send it to the watch
      if (!deviceRef.current) return;
      const payload = `${w.temp.toFixed(1)},${w.humidity},${toWatchCode(w.code)}`;
      await writeText(WEATHER, payload);
    } catch (e) {
      setStatus('Weather error: ' + e.message);
    }
  };

  // one scan-and-connect attempt; safe to call as often as you like
  const attemptConnect = async () => {
    if (deviceRef.current || busyRef.current) return;   // already connected or mid-attempt
    busyRef.current = true;

    if (!permsRef.current) {
      permsRef.current = await requestPermissions();
      if (!permsRef.current) {
        busyRef.current = false;
        setStatus('Permissions denied');
        return;
      }
    }

    setStatus('Scanning...');
    let found = false;

    manager.startDeviceScan([SERVICE], null, async (error, device) => {
      if (error) {
        clearTimeout(scanTimeout.current);
        manager.stopDeviceScan();
        busyRef.current = false;
        setStatus('Scan error: ' + error.message + ' (retrying)');
        return;
      }

      if (found) return;          // the scan can report the same watch twice
      found = true;
      clearTimeout(scanTimeout.current);
      manager.stopDeviceScan();

      try {
        setStatus('Connecting...');
        const d = await device.connect();
        await d.discoverAllServicesAndCharacteristics();
        deviceRef.current = d;
        setStatus('Connected');

      d.onDisconnected((err) => {
        deviceRef.current = null;
        clearInterval(weatherTimer.current);
        setStatus('Disconnected' + (err ? ': ' + err.message : '') + ', retrying...');
      });

        // sync the clock on every connection
        await writeText(TIME, String(Math.floor(Date.now() / 1000)));

        // send weather now, then refresh it every hour while connected
        sendWeather();
        clearInterval(weatherTimer.current);
        weatherTimer.current = setInterval(sendWeather, WEATHER_REFRESH_MS);

        // accelerometer: 3 x int16, little-endian, scaled x100
        d.monitorCharacteristicForService(SERVICE, MPU, (err, c) => {
          if (err || !c?.value) return;
          const b = Buffer.from(c.value, 'base64');
          setAccel({
            x: b.readInt16LE(0) / 100,
            y: b.readInt16LE(2) / 100,
            z: b.readInt16LE(4) / 100,
          });
        });

        // battery: 1 byte, percent
        d.monitorCharacteristicForService(SERVICE, BATT, (err, c) => {
          if (err || !c?.value) return;
          setBattery(Buffer.from(c.value, 'base64').readUInt8(0));
        });
      } catch (e) {
        setStatus('Connect failed: ' + e.message);
        try { await device.cancelConnection(); } catch (_) {}
      } finally {
        busyRef.current = false;
      }
    });

    // no watch found within SCAN_MS: stop and wait for the next retry
    scanTimeout.current = setTimeout(() => {
      manager.stopDeviceScan();
      busyRef.current = false;
      setStatus('Watch not found, retrying...');
    }, SCAN_MS);
  };

  // try on launch, then every RETRY_MS (does nothing while connected)
  useEffect(() => {
    attemptConnect();
    const retry = setInterval(attemptConnect, RETRY_MS);
    return () => {
      clearInterval(retry);
      clearTimeout(scanTimeout.current);
      clearInterval(weatherTimer.current);
      manager.stopDeviceScan();
    };
  }, []);

  // notifications: ask for access, then forward each one to the watch
  useEffect(() => {
    (async () => {
      const s = await RNAndroidNotificationListener.getPermissionStatus();
      if (s !== 'authorized') RNAndroidNotificationListener.requestPermission();
    })();

    return onNotification((n) => {
      if (!deviceRef.current) return;                              // watch not connected
      const text = String(n.title || n.app || 'New').slice(0, 20); // watch buffer holds 20 chars
      writeText(RX, text).catch(() => {});
    });
  }, []);

  const send = (uuid, text) => {
    if (deviceRef.current) writeText(uuid, text).catch((e) => setStatus(e.message));
  };

  return (
    <View style={styles.container}>
      <Text style={styles.status}>{status}</Text>
      <Button title="Connect now" onPress={attemptConnect} />

      <Text style={styles.text}>Battery: {battery ?? '--'} %</Text>
      <Text style={styles.text}>AX {accel.x.toFixed(2)}</Text>
      <Text style={styles.text}>AY {accel.y.toFixed(2)}</Text>
      <Text style={styles.text}>AZ {accel.z.toFixed(2)}</Text>

      <Text style={styles.text}>
        Weather: {weather ? `${weather.temp.toFixed(1)} C, ${weather.humidity}%, code ${weather.code}` : '--'}
      </Text>

      <Text style={styles.text}>
        Location: {location ? `${location.lat.toFixed(2)}, ${location.lon.toFixed(2)}` : '--'}
      </Text>

      <Button title="Send weather now" onPress={sendWeather} />
      <Button title="Send test message" onPress={() => send(RX, 'hello')} />
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, justifyContent: 'center', alignItems: 'center', gap: 12 },
  status: { fontSize: 18, fontWeight: 'bold' },
  text: { fontSize: 20 },
});