const { withAndroidManifest } = require('@expo/config-plugins');

const SERVICE = 'com.asterinet.react.bgactions.RNBackgroundActionsTask';
const PERMS = [
  'android.permission.FOREGROUND_SERVICE',
  'android.permission.FOREGROUND_SERVICE_CONNECTED_DEVICE',
  'android.permission.WAKE_LOCK',
  'android.permission.POST_NOTIFICATIONS',
];

module.exports = (config) =>
  withAndroidManifest(config, (cfg) => {
    const manifest = cfg.modResults.manifest;

    // needed so we can use tools:replace below
    manifest.$['xmlns:tools'] = 'http://schemas.android.com/tools';

    manifest['uses-permission'] = manifest['uses-permission'] || [];
    for (const p of PERMS) {
      if (!manifest['uses-permission'].some((x) => x.$['android:name'] === p)) {
        manifest['uses-permission'].push({ $: { 'android:name': p } });
      }
    }

    const app = manifest.application[0];
    app.service = app.service || [];
    let svc = app.service.find((s) => s.$['android:name'] === SERVICE);
    if (!svc) {
      svc = { $: { 'android:name': SERVICE } };
      app.service.push(svc);
    }
    svc.$['android:foregroundServiceType'] = 'connectedDevice';
    svc.$['tools:replace'] = 'android:foregroundServiceType';

    return cfg;
  });