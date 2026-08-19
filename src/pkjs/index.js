var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;

  var settings = clay.getSettings(e.response);

 if (settings.HapticFrequency !== undefined) {
    settings.HapticFrequency = parseInt(settings.HapticFrequency, 10);
  }
  if (settings.Palette !== undefined) {
    settings.Palette = parseInt(settings.Palette, 10);
  }
  if (settings.BreathPattern !== undefined) {
    settings.BreathPattern = parseInt(settings.BreathPattern, 10);
  }
  if (settings.HapticEnabled !== undefined) {
    settings.HapticEnabled = settings.HapticEnabled ? 1 : 0;
  }
  
  console.log('Clay settings:', JSON.stringify(settings));

  Pebble.sendAppMessage(
    settings,
    function () { console.log('Settings sent'); },
    function (err) { console.log('Settings failed: ' + JSON.stringify(err)); }
  );
});

Pebble.addEventListener('ready', function () {
  console.log('PebbleKit JS ready');
});