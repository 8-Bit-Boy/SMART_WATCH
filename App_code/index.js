import { registerRootComponent } from 'expo';
import { AppRegistry } from 'react-native';
import { RNAndroidNotificationListenerHeadlessJsName } from 'react-native-android-notification-listener';

import App from './App';
import { emitNotification } from './notifications';

AppRegistry.registerHeadlessTask(
  RNAndroidNotificationListenerHeadlessJsName,
  () => async ({ notification }) => {
    if (!notification) return;
    try {
      emitNotification(JSON.parse(notification));
    } catch (e) {}
  }
);

registerRootComponent(App);