// Device setup: the "Advanced" view's dock (hub status, commands, activity)
// and its settings form. Stores the hub's settings (home Wi-Fi, backend URL,
// device key, wake-up schedule) and starts scans over the serial connection. It speaks the
// hub's line protocol: commands go out as text lines, replies come back as
// "TCH ..." lines inside the normal log stream (see hub_console.h).

(() => {
  const panel = document;
  if (!document.querySelector("#device-setup")) {
    return;
  }

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
    camera_plan: panel.querySelector("#ds-current-camera_plan"),
    checkin_min: panel.querySelector("#ds-current-checkin_min"),
    quiet: panel.querySelector("#ds-current-quiet"),
    location: panel.querySelector("#ds-current-location"),
    transport: panel.querySelector("#ds-current-transport"),
    apn: panel.querySelector("#ds-current-apn"),
    sim_pin: panel.querySelector("#ds-current-sim_pin"),
    power: panel.querySelector("#ds-current-power"),
    direction: panel.querySelector("#ds-current-direction"),
    destination: panel.querySelector("#ds-current-destination"),
    webdav_url: panel.querySelector("#ds-current-webdav_url"),
    webdav_user: panel.querySelector("#ds-current-webdav_user"),
    webdav_password: panel.querySelector("#ds-current-webdav_password"),
    webdav_videos: panel.querySelector("#ds-current-webdav_videos"),
    health_url: panel.querySelector("#ds-current-health_url"),
    web_password: panel.querySelector("#ds-current-web_password"),
    camera_user: panel.querySelector("#ds-current-camera_user"),
    camera_password: panel.querySelector("#ds-current-camera_password"),
    backend: panel.querySelector("#ds-current-backend"),
  };
  const scheduleList = panel.querySelector("#ds-schedule");
  const factsList = panel.querySelector("#hub-facts");
  const factsAge = panel.querySelector("#hub-facts-age");
  // "TCH INFO" lines of the status reply with a row of their own in the
  // dock: [prefix, data-fact of the row].
  const FACTS = [
    ["mode ", "mode"],
    ["camera plan ", "plan"],
    ["check-in every ", "checkin"],
    ["next camera read ", "next-read"],
    ["next check-in ", "next-checkin"],
    ["battery ", "battery"],
    ["SD card ", "sd"],
    ["firmware ", "firmware"],
    ["web UI ", "web"],
  ];
  // Lines that only appear when there is something to tell.
  const NOTES = ["failed in a row", "camera battery ", "hub battery level ", "schedule stretched", "retry in "];

  const STATE_TEXT = {
    IDLE: "Idle — waiting for a scan",
    BLE_ACTIVATION: "Waking the camera (BLE)",
    WIFI_JOIN: "Joining the camera Wi-Fi",
    SESSION: "Reading the camera",
    BACKOFF: "Camera not reached — retrying",
    UPLINK_JOIN: "Going online (home Wi-Fi or modem)",
    UPLINK: "Uploading to the backend",
  };

  const DONE_TEXT = {
    "camera-scanned": "Scan finished (no uplink configured)",
    "camera-listed": "Listing finished (nothing fetched or synced)",
    "uplink-done": "Scan and upload finished",
    "camera-unreachable": "Camera not reached",
    "camera-no-card": "The camera has no SD card",
    "uplink-unreachable": "Not online (home Wi-Fi or modem)",
    stopped: "Scan stopped",
  };

  const serial = () => window.trailcamSerial;
  const settings = window.trailcamSettings;

  // Remembered form values, so the settings need not be typed again on every
  // visit. Opt-out via the checkbox, which also deletes what is stored.
  const FIELDS = ["wifi_ssid", "wifi_password", "power", "direction", "destination", "webdav_url", "webdav_user", "webdav_password", "webdav_videos", "health_url", "web_password", "camera_user", "camera_password", "transport", "apn", "sim_pin", "api_url", "device_key", "camera_plan", "checkin_min", "quiet", "location"];
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
    for (const button of [
      ...actionButtons,
      eraseButton,
      form.querySelector('button[type="submit"]'),
      ...Object.values(resetButtons),
    ]) {
      button.disabled = !connected;
    }
    if (!connected) {
      showState(undefined);
      // The last status stays readable, marked as old.
      if (factsList.classList.contains("known")) {
        factsList.classList.add("stale");
        factsAge.textContent = `Not connected. ${factsAge.textContent.replace(/^Not connected\. /, "")}`;
      }
    }
  }

  // One line of the status reply: its own row, a note below the rows, or
  // nothing.
  function showFact(text) {
    if (text.startsWith("mode ")) {
      // The first line of a status reply.
      scheduleList.replaceChildren();
      factsList.classList.add("known");
      factsList.classList.remove("stale");
      factsAge.textContent = `Status of ${new Date().toLocaleTimeString()}`;
    }
    const fact = FACTS.find(([prefix]) => text.startsWith(prefix));
    if (fact) {
      const row = factsList.querySelector(`[data-fact="${fact[1]}"]`);
      row.textContent = text.slice(fact[0].length);
      row.title = row.textContent;
      row.classList.toggle("bad", /not mounted|unknown|overdue/.test(row.textContent));
    } else if (NOTES.some((prefix) => text.startsWith(prefix))) {
      const item = document.createElement("li");
      item.textContent = text;
      scheduleList.append(item);
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

  // A "Reset" button next to each stored setting: `config unset <key>`
  // brings the default back or removes the value (#156). Two clicks, as a
  // secret cannot be read back once it is gone.
  const resetButtons = {};
  for (const [key, span] of Object.entries(current)) {
    if (key === "backend" || !span) {
      continue;
    }
    const button = document.createElement("button");
    button.type = "button";
    button.className = "ds-reset";
    button.textContent = "Reset";
    button.hidden = true;
    button.setAttribute("aria-label", `Reset ${key} on the hub`);
    let armed;
    button.addEventListener("click", async () => {
      if (!armed) {
        button.textContent = "Really reset?";
        armed = setTimeout(() => {
          armed = undefined;
          button.textContent = "Reset";
        }, 4000);
        return;
      }
      clearTimeout(armed);
      armed = undefined;
      button.textContent = "Reset";
      try {
        await send(`config unset ${key}`);
        await send("config show");
        formStatus.textContent = `${key} reset on the hub.`;
      } catch {
        formStatus.textContent = "Reset failed; check the connection.";
      }
    });
    span.after(" ", button);
    resetButtons[key] = button;
  }

  // Shown only for a stored value, and only while connected.
  function showReset(key, shown) {
    const button = resetButtons[key];
    if (button) {
      button.hidden = shown === "" || shown === "–" || shown === "(default)" || shown === "(not set)";
    }
  }

  async function saveSettings(event) {
    event.preventDefault();
    const values = Object.fromEntries(FIELDS.map((key) => [key, settings.fieldValue(form, key)]));
    const problem = settings.problem(values);
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
        await send(settings.setCommand(key, value), `config set ${key} ${settings.shown(key, value)}`);
      }
      await send("config show");
      if (!rememberBox.checked) {
        settings.clearSecrets(form);
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
          showReset(key, value.join(" "));
        }
        break;
      }
      case "INFO":
        showFact(text);
        break;
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
        } else if (event === "media") {
          // One line per recording of "camlist": the Camera tab shows them.
        } else if (event === "done") {
          addLog(details[0].endsWith("unreachable") ? "error" : "ok", DONE_TEXT[details[0]] ?? details[0]);
          if (details.includes("no-card")) {
            addLog("error", DONE_TEXT["camera-no-card"]);
          }
          // Next read, battery and SD card have changed.
          void serial().sendLine("status").catch(() => {});
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

  // The browser's position, only on a click and rounded to 0.1 degrees
  // (about 10 km) before it is put into the field; never sent anywhere else.
  const geolocateButton = panel.querySelector("#ds-geolocate");
  if (!("geolocation" in navigator)) {
    geolocateButton.hidden = true;
  }
  geolocateButton.addEventListener("click", () => {
    formStatus.textContent = "Asking the browser for the location…";
    navigator.geolocation.getCurrentPosition(
      (position) => {
        const round = (v) => (Math.round(v * 10) / 10 + 0).toFixed(1);
        form.elements.location.value = `${round(position.coords.latitude)},${round(position.coords.longitude)}`;
        writeRemembered();
        formStatus.textContent = "Location filled in; save to store it on the hub.";
      },
      () => {
        formStatus.textContent = "The browser did not give a location; type it in (e.g. 52.5,13.4).";
      },
      { enableHighAccuracy: false, timeout: 15000, maximumAge: 3600000 },
    );
  });

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
    // Ask for the current state; the hub may have booted long ago. Not while
    // the camera mock is shown: its console does not know the hub's commands.
    if (document.body.dataset.view !== "mock") {
      void send("status").catch(() => {});
    }
  });
  window.addEventListener("serial-disconnected", () => setConnected(false));

  restoreRemembered();
  setConnected(false);
})();
