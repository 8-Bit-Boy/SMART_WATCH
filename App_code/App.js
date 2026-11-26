import { useEffect, useState } from 'react';
import { View, Text, Button, StyleSheet, PermissionsAndroid, Platform, Linking } from 'react-native';
import RNAndroidNotificationListener from 'react-native-android-notification-listener';
import {
  subscribe, startWatchService, ensureBlePermissions,
  connectNow, sendWeatherNow, sendTestMessage,
} from './watchService';
import { startBackground, stopBackground } from './background';

export default function App() {
  const [s, setS] = useState({
    status: 'Starting...', accel: { x: 0, y: 0, z: 0 },
    battery: null, weather: null, location: null,
  });

  useEffect(() => {
    const unsub = subscribe(setS);

    (async () => {
      // one prompt at a time: Bluetooth first (the service needs it), then notifications
      if (!(await ensureBlePermissions())) return;

      if (Platform.OS === 'android' && Platform.Version >= 33) {
        await PermissionsAndroid.request(PermissionsAndroid.PERMISSIONS.POST_NOTIFICATIONS);
      }

      startWatchService();          // idempotent: does nothing if it's already running
      await startBackground();      // foreground service keeps it alive after the app closes

      const st = await RNAndroidNotificationListener.getPermissionStatus();
      if (st !== 'authorized') RNAndroidNotificationListener.requestPermission();
    })();

    return unsub;
  }, []);

  return (
    <View style={styles.container}>
      <Text style={styles.status}>{s.status}</Text>
      <Button title="Connect now" onPress={connectNow} />

      <Text style={styles.text}>Battery: {s.battery ?? '--'} %</Text>
      <Text style={styles.text}>AX {s.accel.x.toFixed(2)}</Text>
      <Text style={styles.text}>AY {s.accel.y.toFixed(2)}</Text>
      <Text style={styles.text}>AZ {s.accel.z.toFixed(2)}</Text>

      <Text style={styles.text}>
        Weather: {s.weather ? `${s.weather.temp.toFixed(1)} C, ${s.weather.humidity}%, code ${s.weather.code}` : '--'}
      </Text>
      <Text style={styles.text}>
        Location: {s.location ? `${s.location.lat.toFixed(2)}, ${s.location.lon.toFixed(2)}` : '--'}
      </Text>

      <Button title="Send weather now" onPress={sendWeatherNow} />
      <Button title="Send test message" onPress={sendTestMessage} />
      <Button title="Stop background service" onPress={stopBackground} />
      <Button title="Battery settings" onPress={() => Linking.openSettings()} />
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, justifyContent: 'center', alignItems: 'center', gap: 12 },
  status: { fontSize: 18, fontWeight: 'bold' },
  text: { fontSize: 20 },
});