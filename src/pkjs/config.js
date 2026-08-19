module.exports = [
  {
    "type": "heading",
    "defaultValue": "Breath Reminders"
  },
  {
    "type": "toggle",
    "messageKey": "HapticEnabled",
    "label": "Vibration reminders",
    "defaultValue": true
  },
  {
    "type": "select",
    "messageKey": "BreathPattern",
    "label": "Breathing pattern",
    "defaultValue": "0",
    "options": [
      { "label": "4-7-8 (Relaxing)",      "value": "0" },
      { "label": "5-5 (Balancing)",         "value": "1" },
      { "label": "Box 4-4-4-4 (Centering)", "value": "2" }
    ]
  },
  {
    "type": "select",
    "messageKey": "HapticFrequency",
    "label": "Reminder frequency",
    "defaultValue": "2",
    "options": [
      { "label": "Hourly",           "value": "0" },
      { "label": "Every 30 minutes", "value": "1" },
      { "label": "Every 15 minutes", "value": "2" }
    ]
  },
  {
    "type": "heading",
    "defaultValue": "Appearance"
  },
  {
    "type": "select",
    "messageKey": "Palette",
    "label": "Color palette",
    "defaultValue": "0",
    "options": [
      { "label": "Bloom",  "value": "0" },
      { "label": "Sand",   "value": "1" },
      { "label": "Forest", "value": "2" },
      { "label": "Frost",  "value": "3" },
      { "label": "Ember",  "value": "4" }
    ]
  },
  {
    "type": "submit",
    "defaultValue": "Save Settings"
  }
];