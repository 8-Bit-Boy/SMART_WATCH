import { useState, useRef } from 'react';
import { View, Text, Button, StyleSheet, PermissionsAndroid, Platform } from 'react-native';
import { BleManager } from 'react-native-ble-plx';
import { Buffer } from 'buffer';

const SERVICE = '12345678-1234-5678-1234-56789abcdef0';
const MPU     = '12345678-1234-5678-1234-56789abcdef2';
const BATT    = '12345678-1234-5678-1234-56789abcdef3';
const RX      = '12345678-1234-5678-1234-56789abcdef4';
const TIME    = '12345678-1234-5678-1234-56789abcdef5';
const WEATHER = '12345678-1234-5678-1234-56789abcdef6';

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

export default function App() {
  const [status, setStatus] = useState('Not connected');
  const [accel, setAccel] = useState({ x: 0, y: 0, z: 0 });
  const [battery, setBattery] = useState(null);
  const deviceRef = useRef(null);

  // BLE values travel as base64 strings, so encode text before writing
  const writeText = (uuid, text) =>
    deviceRef.current.writeCharacteristicWithResponseForService(
      SERVICE, uuid, Buffer.from(text, 'utf8').toString('base64')
    );

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
          setStatus('Disconnected');
        });

        // sync the clock on every connection
        await writeText(TIME, String(Math.floor(Date.now() / 1000)));

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

      <Button title="Send test message" onPress={() => send(RX, 'hello')} />
      <Button title="Send test weather" onPress={() => send(WEATHER, '23.5,60,2')} />
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, justifyContent: 'center', alignItems: 'center', gap: 12 },
  status: { fontSize: 18, fontWeight: 'bold' },
  text: { fontSize: 20 },
});