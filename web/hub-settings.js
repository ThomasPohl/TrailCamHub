// What the setup assistant (wizard.js) and the settings form of the
// "Advanced" view (device-setup.js) both need to store settings on the hub:
// which keys are secrets, the early checks of the values and the console
// commands that write them. Loaded before both; see docs/installer.md for
// the commands.
(function () {
  "use strict";

  // Never shown, never read back from the hub.
  const SECRETS = ["wifi_password", "device_key", "sim_pin", "webdav_password", "web_password", "camera_password"];

  // Passwords may legitimately contain spaces at the ends; every other
  // value is trimmed.
  const UNTRIMMED = ["wifi_password", "webdav_password", "web_password", "camera_password"];

  // Mirrors the firmware's checks (hub_config_rules.cpp) for early feedback;
  // the hub checks again and answers with its own reason. Each rule gets a
  // value that is not empty and returns true if it is fine.
  const RULES = {
    wifi_ssid: [(v) => new TextEncoder().encode(v).length <= 32, "The Wi-Fi name may be at most 32 bytes."],
    wifi_password: [(v) => v.length >= 8 && v.length <= 64, "The Wi-Fi password must have 8 to 64 characters."],
    // The APN goes into an AT command on the hub.
    apn: [(v) => /^[A-Za-z0-9._-]{1,63}$/.test(v), "The APN may only contain letters, digits, '.', '-' and '_'."],
    sim_pin: [(v) => /^\d{4}$/.test(v), "The SIM PIN must have 4 digits."],
    api_url: [(v) => /^https:\/\/\S+$/.test(v), "The API URL must start with https://."],
    device_key: [(v) => /^[A-Za-z0-9_-]{1,64}\.[A-Za-z0-9_-]{16,128}$/.test(v), "The device key must look like <hubId>.<secret>."],
    // The hub checks the plan in full.
    camera_plan: [(v) => /^(off|((\d\d:\d\d-\d\d:\d\d|\*)=\d{1,4}[,; ]*)+)$/.test(v), "The camera plan must look like 05:00-09:00=30,*=120 (or off)."],
    checkin_min: [(v) => /^\d{1,4}$/.test(v) && Number(v) <= 1440, "The check-in interval must be 0 to 1440 minutes."],
    webdav_url: [(v) => /^https?:\/\/\S+$/.test(v), "The WebDAV folder URL must start with https:// or http://."],
    health_url: [(v) => /^https?:\/\/\S+$/.test(v), "The health check URL must start with https:// or http://."],
    web_password: [(v) => v.length >= 8 && v.length <= 63, "The web UI password must have 8 to 63 characters."],
  };

  function toHex(text) {
    return Array.from(new TextEncoder().encode(text), (b) => b.toString(16).padStart(2, "0")).join("");
  }

  window.trailcamSettings = {
    SECRETS,
    isSecret: (key) => SECRETS.includes(key),
    // The value of a form field as it is stored.
    fieldValue: (form, key) => {
      const raw = String(form.elements[key]?.value ?? "");
      return UNTRIMMED.includes(key) ? raw : raw.trim();
    },
    // The first problem among `values` ({key: value}; empty values are not
    // checked), or "".
    problem: (values) => {
      for (const [key, value] of Object.entries(values)) {
        const rule = RULES[key];
        if (rule && value !== "" && !rule[0](value)) {
          return rule[1];
        }
      }
      return "";
    },
    // The console command that stores `value` (hex, so that any character
    // survives the line protocol) and the text to show for it.
    setCommand: (key, value) => `config set-hex ${key} ${toHex(value)}`,
    shown: (key, value) => (SECRETS.includes(key) ? "••••" : value),
    // Empties the secret fields of a form after they were written.
    clearSecrets: (form) => {
      for (const key of SECRETS) {
        if (form.elements[key]) {
          form.elements[key].value = "";
        }
      }
    },
  };
})();
