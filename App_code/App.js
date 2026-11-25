import { View, Text, Button, StyleSheet, PermissionsAndroid, Platform } from 'react-native';
import { BleManager } from 'react-native-ble-plx';
import { Buffer } from 'buffer';
import { useState, useRef, useEffect } from 'react';
import RNAndroidNotificationListener from 'react-native-android-notification-listener';
import { onNotification } from './notifications';

const SERVICE = '12345678-1234-5678-1234-56789abcdef0';
const MPU     = '12345678-1234-5678-1234-56789abcdef2';
const BATT    = '12345678-1234-5678-1234-56789abcdef3';
const RX      = '12345678-1234-5678-1234-56789abcdef4';
const TIME    = '12345678-1234-5678-1234-56789abcdef5';
const WEATHER = '12345678-1234-5678-1234-56789abcdef6';

// Pretoria; change these if you move
const LATITUDE  = -25.7545;
const LONGITUDE = 28.2314;
const WEATHER_REFRESH_MS = 60 * 60 * 1000;   // 1 hour

const WEATHER_URL =
  `https://api.open-meteo.com/v1/forecast?latitude=${LATITUDE}&longitude=${LONGITUDE}` +
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
  if ([56, 57].includes(code)) return 51;            // freezing drizzle -> drizzle
  if ([66, 67].includes(code)) return 61;            // freezing rain -> rain
  if (code === 80) return 61;                        // showers -> rain
  if (code === 81) return 63;
  if (code === 82) return 65;
  if (code === 77 || code === 85 || code === 86) return 71;   // snow grains/showers -> snow
  if (code === 96 || code === 99) return 95;         // thunderstorm with hail -> storm
  return code;
}

async function fetchWeather() {
  const res = await fetch(WEATHER_URL);
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
  const [status, setStatus] = useState('Not connected');
  const [accel, setAccel] = useState({ x: 0, y: 0, z: 0 });
  const [battery, setBattery] = useState(null);
  const [weather, setWeather] = useState(null);
  const deviceRef = useRef(null);
  const weatherTimer = useRef(null);

    useEffect(() => {
    // ask for notification access (opens the system settings page)
    (async () => {
      const s = await RNAndroidNotificationListener.getPermissionStatus();
      if (s !== 'authorized') RNAndroidNotificationListener.requestPermission();
    })();

    // forward each notification to the watch
    return onNotification((n) => {
      if (!deviceRef.current) return;                    // watch not connected
      const text = String(n.title || n.app || 'New').slice(0, 20);   // the watch buffer holds 20 chars
      writeText(RX, text).catch(() => {});
    });
  }, []);

  // BLE values travel as base64 strings, so encode text before writing
  const writeText = (uuid, text) =>
    deviceRef.current.writeCharacteristicWithResponseForService(
      SERVICE, uuid, Buffer.from(text, 'utf8').toString('base64')
    );

  // fetch real weather, show it in the app, and send it to the watch
  const sendWeather = async () => {
    try {
      const w = await fetchWeather();
      setWeather(w);

      if (!deviceRef.current) return;
      // format the watch expects: "temp,humidity,code"
      const payload = `${w.temp.toFixed(1)},${w.humidity},${toWatchCode(w.code)}`;
      await writeText(WEATHER, payload);
    } catch (e) {
      setStatus('Weather error: ' + e.message);
    }
  };

  const connect = async () => {
    if (!(await requestPermissions())) {
      setStatus('Permissions denied');
      return;
    }
    setStatus('Scanning...');

    manager.startDeviceScan([SERVICE], null, async (error, device) => {
      if (error) { setStatus(error.message); return; }
      manager.stopDeviceScan();

      try {
        setStatus('Connecting...');
        const d = await device.connect();
        await d.discoverAllServicesAndCharacteristics();
        deviceRef.current = d;
        setStatus('Connected');

        d.onDisconnected(() => {
          deviceRef.current = null;
          clearInterval(weatherTimer.current);
          setStatus('Disconnected');
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
        setStatus('Error: ' + e.message);
      }
    });
  };

  const send = (uuid, text) => {
    if (deviceRef.current) writeText(uuid, text).catch((e) => setStatus(e.message));
  };

  return (
    <View style={styles.container}>
      <Text style={styles.status}>{status}</Text>
      <Button title="Connect to watch" onPress={connect} />

      <Text style={styles.text}>Battery: {battery ?? '--'} %</Text>
      <Text style={styles.text}>AX {accel.x.toFixed(2)}</Text>
      <Text style={styles.text}>AY {accel.y.toFixed(2)}</Text>
      <Text style={styles.text}>AZ {accel.z.toFixed(2)}</Text>

      <Text style={styles.text}>
        Weather: {weather ? `${weather.temp.toFixed(1)} C, ${weather.humidity}%, code ${weather.code}` : '--'}
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