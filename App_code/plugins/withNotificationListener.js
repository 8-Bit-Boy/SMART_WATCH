const { withAndroidManifest } = require('@expo/config-plugins');

const SERVICE =
  'com.lesimoes.androidnotificationlistener.RNAndroidNotificationListener';

module.exports = (config) =>
  withAndroidManifest(config, (cfg) => {
    const app = cfg.modResults.manifest.application[0];
    app.service = app.service || [];

    if (!app.service.some((s) => s.$['android:name'] === SERVICE)) {
      app.service.push({
        $: {
          'android:name': SERVICE,
          'android:label': '@string/app_name',
          'android:permission': 'android.permission.BIND_NOTIFICATION_LISTENER_SERVICE',
          'android:exported': 'true',
        },
        'intent-filter': [
          {
            action: [
              { $: { 'android:name': 'android.service.notification.NotificationListenerService' } },
            ],
          },
        ],
      });
    }
    return cfg;
  });