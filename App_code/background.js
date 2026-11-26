import BackgroundService from 'react-native-background-actions';
import { startWatchService, stopWatchService } from './watchService';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// the task just keeps the JS alive; the real work runs on timers in watchService
const task = async () => {
  startWatchService();
  while (BackgroundService.isRunning()) {
    await sleep(5000);
  }
  stopWatchService();
};

const options = {
  taskName: 'SmartWatch',
  taskTitle: 'Smart watch',
  taskDesc: 'Keeping your watch synced',
  taskIcon: { name: 'ic_launcher', type: 'mipmap' },
  color: '#2563eb',
  foregroundServiceType: ['connectedDevice'],   // must match the manifest
};

export async function startBackground() {
  if (BackgroundService.isRunning()) return;
  await BackgroundService.start(task, options);
}

export async function stopBackground() {
  await BackgroundService.stop();
}