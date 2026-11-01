// Device setup panel: stores the hub's settings (home Wi-Fi, backend URL,
// device key) and starts scans over the serial connection. It speaks the
// hub's line protocol: commands go out as text lines, replies come back as
// "TCH ..." lines inside the normal log stream (see hub_console.h).

(() => {
  const panel = document.querySelector("#device-setup");
  if (!panel) {
    return;
  }

  const connectButton = panel.querySelector("#ds-connect");
  const connection = panel.querySelector("#ds-connection");
  const stateChip = panel.querySelector("#ds-state");
  const form = panel.querySelector("#ds-form");
  const formStatus = panel.querySelector("#ds-form-status");
  const actionButtons = panel.querySelectorAll("[data-command]");
  const eraseButton = panel.querySelector("#ds-erase");
  const log = panel.querySelector("#ds-log");
  const current = {
    wifi_ssid: panel.querySelector("#ds-current-wifi_ssid"),
    wifi_password: panel.querySelector("#ds-current-wifi_password"),
    api_url: panel.querySelector("#ds-current-api_url"),
    device_key: panel.querySelector("#ds-current-device_key"),
    uplink: panel.querySelector("#ds-current-uplink"),
  };

  const STATE_TEXT = {
    IDLE: "Idle — waiting for a scan",
    BLE_ACTIVATION: "Waking the camera (BLE)",
    WIFI_JOIN: "Joining the camera Wi-Fi",
    SESSION: "Reading the camera",
    BACKOFF: "Camera not reached — retrying",
    UPLINK_JOIN: "Joining the home Wi-Fi",
    UPLINK: "Uploading to the backend",
  };

  const DONE_TEXT = {
    "camera-scanned": "Scan finished (no uplink configured)",
    "uplink-done": "Scan and upload finished",
    "camera-unreachable": "Camera not reached",
    "uplink-unreachable": "Home Wi-Fi not reached",
    stopped: "Scan stopped",
  };

  const serial = () => window.trailcamSerial;

  // Remembered form values, so the settings need not be typed again on every
  // visit. Opt-out via the checkbox, which also deletes what is stored.
  const FIELDS = ["wifi_ssid", "wifi_password", "api_url", "device_key"];
  const STORAGE_KEY = "trailcamhub.deviceSetup";
  const PREFERENCE_KEY = "trailcamhub.deviceSetup.remember";
  const rememberBox = panel.querySelector("#ds-remember");

  function readRemembered() {
    try {
      const raw = window.localStorage.getItem(STORAGE_KEY);
      return raw ? JSON.parse(raw) : null;
    } catch {
      return null;
    }
  }

  function writeRemembered() {
    if (!rememberBox.checked) {
      return;
    }
    const values = Object.fromEntries(FIELDS.map((key) => [key, form.elements[key].value]));
    try {
      window.localStorage.setItem(STORAGE_KEY, JSON.stringify(values));
    } catch {
      // Storage blocked or full: remembering is only a convenience.
    }
  }

  function forgetRemembered() {
    try {
      window.localStorage.removeItem(STORAGE_KEY);
    } catch {
      // Nothing stored that we could reach.
    }
  }

  function restoreRemembered() {
    // On by default; unticking stores an "off" marker and deletes the values.
    rememberBox.checked = readRememberPreference();
    const stored = rememberBox.checked ? readRemembered() : null;
    if (!stored) {
      return;
    }
    for (const key of FIELDS) {
      if (typeof stored[key] === "string") {
        form.elements[key].value = stored[key];
      }
    }
  }

  function readRememberPreference() {
    try {
      return window.localStorage.getItem(PREFERENCE_KEY) !== "off";
    } catch {
      return false;
    }
  }

  function writeRememberPreference(on) {
    try {
      if (on) {
        window.localStorage.removeItem(PREFERENCE_KEY);
      } else {
        window.localStorage.setItem(PREFERENCE_KEY, "off");
      }
    } catch {
      // Ignore: the checkbox simply starts unticked next time.
    }
  }

  function setConnected(connected) {
    connection.textContent = connected ? "Connected" : "Not connected";
    connectButton.hidden = connected;
    for (const button of [...actionButtons, eraseButton, form.querySelector("button")]) {
      button.disabled = !connected;
    }
    if (!connected) {
      showState(undefined);
    }
  }

  function showState(state) {
    stateChip.hidden = !state;
    stateChip.textContent = state ? STATE_TEXT[state] ?? state : "";
    stateChip.dataset.state = state ?? "";
  }

  function addLog(kind, text) {
    const item = document.createElement("li");
    item.className = `ds-log-${kind}`;
    const time = new Date().toLocaleTimeString();
    item.textContent = `${time}  ${text}`;
    log.prepend(item);
    while (log.children.length > 40) {
      log.lastElementChild.remove();
    }
  }

  async function send(line, label = line) {
    try {
      await serial().sendLine(line);
      addLog("sent", `→ ${label}`);
    } catch (error) {
      addLog("error", `Could not send: ${error.message}`);
      throw error;
    }
  }

  function toHex(text) {
    return Array.from(new TextEncoder().encode(text), (b) =>
      b.toString(16).padStart(2, "0"),
    ).join("");
  }

  // Mirrors the firmware's checks (hub_config_rules.cpp) for early feedback.
  function validate(values) {
    if (values.wifi_ssid && new TextEncoder().encode(values.wifi_ssid).length > 32) {
      return "The Wi-Fi name may be at most 32 bytes.";
    }
    if (values.wifi_password && (values.wifi_password.length < 8 || values.wifi_password.length > 64)) {
      return "The Wi-Fi password must have 8 to 64 characters.";
    }
    if (values.api_url && !/^https:\/\/\S+$/.test(values.api_url)) {
      return "The API URL must start with https://.";
    }
    if (values.device_key && !/^[A-Za-z0-9_-]{1,64}\.[A-Za-z0-9_-]{16,128}$/.test(values.device_key)) {
      return "The device key must look like <hubId>.<secret>.";
    }
    return "";
  }

  async function saveSettings(event) {
    event.preventDefault();
    const values = Object.fromEntries(
      ["wifi_ssid", "wifi_password", "api_url", "device_key"].map((key) => [
        key,
        form.elements[key].value.trim(),
      ]),
    );
    // Passwords may legitimately contain spaces at the ends.
    values.wifi_password = form.elements.wifi_password.value;
    const problem = validate(values);
    if (problem) {
      formStatus.textContent = problem;
      return;
    }
    const entries = Object.entries(values).filter(([, value]) => value !== "");
    if (entries.length === 0) {
      formStatus.textContent = "Nothing to save: all fields are empty.";
      return;
    }
    formStatus.textContent = "Saving…";
    try {
      for (const [key, value] of entries) {
        const secret = key === "wifi_password" || key === "device_key";
        await send(`config set-hex ${key} ${toHex(value)}`, `config set ${key} ${secret ? "••••" : value}`);
      }
      await send("config show");
      if (!rememberBox.checked) {
        form.elements.wifi_password.value = "";
        form.elements.device_key.value = "";
      }
      formStatus.textContent = "Saved. Empty fields keep their stored value.";
    } catch {
      formStatus.textContent = "Saving failed; check the connection.";
    }
  }

  function handleLine(line) {
    if (!line.startsWith("TCH ")) {
      return;
    }
    const [, kind, ...rest] = line.split(" ");
    const text = rest.join(" ");
    switch (kind) {
      case "READY":
        addLog("event", `Hub ready (${text})`);
        showState(text);
        void send("config show").catch(() => {});
        break;
      case "STATE":
        showState(text);
        break;
      case "CONFIG": {
        const [key, ...value] = rest;
        if (current[key]) {
          current[key].textContent = value.join(" ");
        }
        break;
      }
      case "OK":
        addLog("ok", text);
        break;
      case "ERR":
        addLog("error", text);
        break;
      case "EVENT": {
        const [event, ...details] = rest;
        if (event === "state") {
          showState(details[0]);
        } else if (event === "stopping") {
          addLog("event", "Stopping — closing the camera session");
        } else if (event === "done") {
          addLog(details[0].endsWith("unreachable") ? "error" : "ok", DONE_TEXT[details[0]] ?? details[0]);
        } else {
          addLog(event === "uplink" && details[0] === "failed" ? "error" : "event", text);
        }
        break;
      }
      case "HELP":
        addLog("event", text);
        break;
      default:
        break;
    }
  }

  connectButton.addEventListener("click", () => serial()?.connect());
  form.addEventListener("submit", saveSettings);
  form.addEventListener("input", (event) => {
    if (event.target !== rememberBox) {
      writeRemembered();
    }
  });
  rememberBox.addEventListener("change", () => {
    writeRememberPreference(rememberBox.checked);
    if (rememberBox.checked) {
      writeRemembered();
    } else {
      forgetRemembered();
    }
  });
  for (const button of actionButtons) {
    button.addEventListener("click", () => {
      void send(button.dataset.command).catch(() => {});
    });
  }
  eraseButton.addEventListener("click", () => {
    if (window.confirm("Erase all settings stored on the hub?")) {
      void send("config clear")
        .then(() => send("config show"))
        .catch(() => {});
    }
  });

  window.addEventListener("serial-line", (event) => handleLine(event.detail));
  window.addEventListener("serial-connected", () => {
    setConnected(true);
    // Ask for the current state; the hub may have booted long ago.
    void send("status").catch(() => {});
  });
  window.addEventListener("serial-disconnected", () => setConnected(false));

  restoreRemembered();
  setConnected(false);
  if (!("serial" in navigator)) {
    connectButton.disabled = true;
    connection.textContent = "Web Serial is not supported by this browser.";
  }
})();
