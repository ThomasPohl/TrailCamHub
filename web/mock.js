// "Camera mock" view: flashes the mock (tools/camera_mock) and drives its
// serial console from the page: status, session control, the camera identity,
// the size of the media list and the fault injection.
//
// The mock's console is the plain ESP-IDF console (prompt "mock>"), not the
// hub's "TCH ..." protocol, so this file has its own command runner. It shares
// the serial connection with the hub views (window.trailcamSerial).
(function () {
  "use strict";

  const view = document.getElementById("view-mock");
  if (!view) {
    return;
  }
  const serial = () => window.trailcamSerial;
  const $ = (id) => document.getElementById(id);

  const connectButton = $("mock-connect");
  const otherPortButton = $("mock-other-port");
  const disconnectButton = $("mock-disconnect");
  const connStatus = $("mock-conn-status");
  const status = $("mock-status");
  const answer = $("mock-answer");
  const factsList = $("mock-facts");
  const factsAge = $("mock-facts-age");
  const log = $("mock-log");
  const commandForm = $("mock-command");
  const idForm = $("mock-id-form");
  const mediaForm = $("mock-media-form");
  const bleName = $("mock-ble-name");
  const faultRows = [...view.querySelectorAll(".mock-fault")];

  // The hub dock owns the connection buttons and their texts; this view
  // mirrors them.
  const hub = {
    connect: $("connect-serial"),
    other: $("other-port"),
    disconnect: $("disconnect-serial"),
    status: $("serial-status"),
  };

  const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const ANSI = /\u001b\[[0-9;?]*[A-Za-z]/g;
  // "I (1234) mock_ap: ..." log lines belong to the console log only.
  const LOG_LINE = /^[EWIDV] \(\d+\) /;

  // --- Connection ---

  function syncConnection() {
    connectButton.hidden = hub.connect.disabled;
    disconnectButton.hidden = hub.disconnect.disabled;
    disconnectButton.textContent = hub.disconnect.textContent;
    otherPortButton.hidden = hub.other.hidden || hub.connect.disabled;
    connStatus.textContent = hub.status.textContent;
  }
  const observer = new MutationObserver(syncConnection);
  for (const element of [hub.connect, hub.other, hub.disconnect]) {
    observer.observe(element, { attributes: true, childList: true, characterData: true, subtree: true });
  }
  observer.observe(hub.status, { childList: true, characterData: true, subtree: true });
  syncConnection();

  connectButton.addEventListener("click", () => void serial().connect());
  otherPortButton.addEventListener("click", () => void serial().connect(true));
  disconnectButton.addEventListener("click", () => void serial().disconnect());

  // The mock has no "ping" and no battery, so the page must not send one.
  new MutationObserver(() => {
    const shown = document.body.dataset.view === "mock";
    serial()?.setKeepAwake(!shown);
    if (shown && serial()?.isConnected() && !factsList.classList.contains("known")) {
      void refresh();
    }
  }).observe(document.body, {
    attributes: true,
    attributeFilter: ["data-view"],
  });

  // --- Console log ---

  const MAX_LOG_CHARS = 300_000;
  let logChars = 0;

  function appendLog(text) {
    const following = log.scrollHeight - log.scrollTop - log.clientHeight <= 24;
    log.append(`${text}\n`);
    logChars += text.length + 1;
    while (logChars > MAX_LOG_CHARS && log.firstChild) {
      logChars -= log.firstChild.textContent.length;
      log.firstChild.remove();
    }
    if (following) {
      log.scrollTop = log.scrollHeight;
    }
  }

  window.addEventListener("serial-line", (event) => {
    const text = String(event.detail).replace(ANSI, "");
    if (text.trim() !== "") {
      appendLog(text);
    }
  });

  $("mock-log-clear").addEventListener("click", () => {
    log.textContent = "";
    logChars = 0;
  });
  $("mock-log-copy").addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(log.textContent);
      status.textContent = "Console log copied.";
    } catch {
      status.textContent = "Could not copy the log.";
    }
  });

  // --- Commands ---

  // One command at a time. The console prints no end marker, so an answer is
  // what arrives until it is quiet for `quietMs`.
  let chain = Promise.resolve();

  function cleanLine(raw, sent) {
    let text = String(raw).replace(ANSI, "");
    text = text.replace(/^(\s*mock>\s*)+/, "").trim();
    if (text === "" || text === sent || LOG_LINE.test(text)) {
      return null;
    }
    return text;
  }

  function run(line, quietMs = 450, maxMs = 5000) {
    const job = () =>
      new Promise((resolve, reject) => {
        const lines = [];
        let quiet;
        let hard;
        const finish = () => {
          clearTimeout(quiet);
          clearTimeout(hard);
          window.removeEventListener("serial-line", onLine);
        };
        const onLine = (event) => {
          const text = cleanLine(event.detail, line);
          if (text === null) {
            return;
          }
          lines.push(text);
          clearTimeout(quiet);
          quiet = setTimeout(() => {
            finish();
            resolve(lines);
          }, quietMs);
        };
        window.addEventListener("serial-line", onLine);
        // Nothing at all within this time: the answer is empty.
        quiet = setTimeout(() => {
          finish();
          resolve(lines);
        }, quietMs + 700);
        hard = setTimeout(() => {
          finish();
          resolve(lines);
        }, maxMs);
        serial()
          .sendLine(line)
          .catch((error) => {
            finish();
            reject(error);
          });
      });
    const result = chain.then(job, job);
    chain = result.catch(() => {});
    return result;
  }

  function connected() {
    if (serial()?.isConnected()) {
      return true;
    }
    status.textContent = "Connect to the mock first.";
    return false;
  }

  // Sends `line`, shows the answer under the command line and returns it.
  // `null` if there is no connection or the write failed.
  async function send(line, options = {}) {
    if (!connected()) {
      return null;
    }
    try {
      const lines = await run(line, options.quietMs, options.maxMs);
      answer.textContent = lines.join("\n");
      return lines;
    } catch (error) {
      status.textContent = `Could not send: ${error.message}`;
      return null;
    }
  }

  // The first line of an answer that tells why a command failed.
  function problemIn(lines) {
    return lines.find((text) => /usage|takes|error code|unrecognized|not found/i.test(text)) ?? null;
  }

  // --- Status ---

  const FACTS = {};
  for (const dd of factsList.querySelectorAll("[data-fact]")) {
    FACTS[dd.dataset.fact] = dd;
  }

  function setFact(name, text, bad = false) {
    FACTS[name].textContent = text;
    FACTS[name].classList.toggle("bad", bad);
  }

  function clearFacts() {
    for (const name of Object.keys(FACTS)) {
      setFact(name, "–");
    }
    factsList.classList.remove("known", "stale");
  }

  // Puts the faults of a `faults: …` line into the fields below.
  function showFaults(text) {
    const faults = text.replace(/^faults:\s*/, "").replace(/\s*\(dropped.*$/, "");
    setFact("faults", faults === "none" ? "none" : faults);
    const number = (pattern) => {
      const match = pattern.exec(faults);
      return match ? match[1] : null;
    };
    const fields = {
      drop: number(/drop (\d+) %/),
      dup: number(/duplicate (\d+) %/),
      lateack: number(/ACKs (\d+) ms late/),
      battery: number(/battery (\d+) %/),
      sdfree: number(/SD free (\d+) MB/),
      abort: number(/stop after (\d+) frames/),
    };
    for (const row of faultRows) {
      const name = row.dataset.fault;
      if (name === "mute") {
        row.querySelector("input").checked = /mute after login/.test(faults);
        continue;
      }
      const value = fields[name];
      // A fault that is off keeps what was typed, except the percentages and
      // the delay, whose "off" is 0.
      if (value !== null) {
        row.querySelector("input").value = value;
      } else if (["drop", "dup", "lateack"].includes(name)) {
        row.querySelector("input").value = "0";
      }
    }
  }

  function showStatus(lines) {
    let known = false;
    for (const text of lines) {
      if (text.startsWith("BLE:")) {
        known = true;
        setFact("ble", text.slice(4).trim());
      } else if (text.startsWith("AP:")) {
        known = true;
        setFact("ap", text.slice(3).trim().replace(/ \(BSSID [^)]*password ([^)]*)\)/, " · password $1"));
      } else if (text.startsWith("session:")) {
        setFact("session", text.slice(8).trim());
      } else if (text.startsWith("faults:")) {
        showFaults(text);
      }
    }
    return known;
  }

  function showId(lines) {
    for (const text of lines) {
      const match = /([0-9A-F]{2}(?::[0-9A-F]{2}){5}) \((TC100_[0-9A-F]+)\)/i.exec(text);
      if (match) {
        setFact("id", `${match[1]} (${match[2]})`);
        const field = idForm.elements.mac;
        if (document.activeElement !== field) {
          field.value = match[1].toUpperCase();
        }
        showBleName();
        return;
      }
    }
  }

  function showMedia(lines) {
    for (const text of lines) {
      const match = /^(\d+) recordings/.exec(text);
      if (match) {
        setFact("media", match[1]);
        if (document.activeElement !== mediaForm.elements.count) {
          mediaForm.elements.count.value = match[1];
        }
        return;
      }
    }
  }

  let refreshing = false;

  // Reads status, identity and media count. The board may still be booting
  // right after connecting or resetting, so an empty answer is tried once more.
  async function refresh(retry = true) {
    if (refreshing || !serial()?.isConnected()) {
      return;
    }
    refreshing = true;
    try {
      let lines = await run("status", 350, 3000);
      let known = showStatus(lines);
      if (!known && retry) {
        await sleep(2000);
        lines = await run("status", 350, 3000);
        known = showStatus(lines);
      }
      if (!known) {
        factsAge.textContent = "No answer. Is the camera mock installed on this board? (The hub firmware answers differently.)";
        factsList.classList.add("stale");
        return;
      }
      showId(await run("id", 300, 2000));
      showMedia(await run("media", 300, 2000));
      factsList.classList.add("known");
      factsList.classList.remove("stale");
      factsAge.textContent = `Updated ${new Date().toLocaleTimeString()}`;
    } catch (error) {
      factsAge.textContent = `Could not read the status: ${error.message}`;
    } finally {
      refreshing = false;
    }
  }

  window.addEventListener("serial-connected", () => {
    setConnected(true);
    if (document.body.dataset.view === "mock") {
      serial().setKeepAwake(false);
      answer.textContent = "";
      status.textContent = "";
      setTimeout(() => void refresh(), 500);
    }
  });
  window.addEventListener("serial-disconnected", () => {
    setConnected(false);
    if (factsList.classList.contains("known")) {
      factsList.classList.add("stale");
      factsAge.textContent = `Not connected. ${factsAge.textContent.replace(/^Not connected\. /, "")}`;
    }
  });

  // --- Controls ---

  const needMock = [...view.querySelectorAll("button:not(#mock-connect):not(#mock-other-port):not(#mock-disconnect):not(#mock-log-clear):not(#mock-log-copy)"), commandForm.elements.line];
  // The install button is not one of them (it is an ESP Web Tools element).
  function setConnected(on) {
    for (const element of needMock) {
      if (!element.closest("esp-web-install-button")) {
        element.disabled = !on;
      }
    }
    for (const input of view.querySelectorAll("form input, .mock-fault input")) {
      input.disabled = !on;
    }
  }
  setConnected(false);
  clearFacts();

  for (const button of view.querySelectorAll("[data-mock-command]")) {
    button.addEventListener("click", async () => {
      const line = button.dataset.mockCommand;
      status.textContent = "";
      const lines = await send(line, { quietMs: line === "help" ? 700 : 450 });
      if (lines && line !== "status" && line !== "help") {
        void refresh(false);
      } else if (lines && line === "status") {
        showStatus(lines);
        void refresh(false);
      }
    });
  }

  async function resetBoard() {
    if (!connected()) {
      return;
    }
    try {
      const info = await serial().reset();
      status.textContent = "Reset sent; reconnecting…";
      await sleep(1500);
      const back = serial().isConnected() || (await serial().reconnect(info));
      status.textContent = back ? "The mock restarted." : "The mock restarted; connect again.";
      if (back) {
        await sleep(2500);
        void refresh();
      }
    } catch (error) {
      status.textContent = `Could not reset: ${error.message}. Press the board's EN button.`;
    }
  }
  $("mock-reset").addEventListener("click", () => void resetBoard());

  // The command line, with a history on the arrow keys.
  const history = [];
  let historyAt = 0;
  commandForm.elements.line.addEventListener("keydown", (event) => {
    if (event.key !== "ArrowUp" && event.key !== "ArrowDown") {
      return;
    }
    event.preventDefault();
    historyAt = Math.max(0, Math.min(history.length, historyAt + (event.key === "ArrowUp" ? -1 : 1)));
    commandForm.elements.line.value = history[historyAt] ?? "";
  });
  commandForm.addEventListener("submit", async (event) => {
    event.preventDefault();
    const line = commandForm.elements.line.value.trim();
    if (line === "") {
      return;
    }
    history.push(line);
    historyAt = history.length;
    commandForm.elements.line.value = "";
    status.textContent = "";
    await send(line, { quietMs: 700 });
    void refresh(false);
  });

  // --- Settings: identity ---

  function showBleName() {
    const match = /^(?:[0-9A-F]{2}:){4}([0-9A-F]{2}):([0-9A-F]{2})$/i.exec(idForm.elements.mac.value.trim());
    bleName.textContent = match ? `TC100_${match[1]}${match[2]}`.toUpperCase() : "–";
  }
  idForm.elements.mac.addEventListener("input", showBleName);

  async function storeIdentity(argument) {
    status.textContent = "";
    const lines = await send(`id ${argument}`);
    if (lines === null) {
      return;
    }
    const problem = problemIn(lines);
    if (problem || !lines.some((text) => /^Stored|^Back to/.test(text))) {
      status.textContent = problem ?? "The mock did not confirm. Is this the camera mock?";
      return;
    }
    // The identity applies after a reset (BLE name, MAC).
    await resetBoard();
  }

  idForm.addEventListener("submit", (event) => {
    event.preventDefault();
    const mac = idForm.elements.mac.value.trim().toUpperCase();
    if (!/^([0-9A-F]{2}:){5}[0-9A-F]{2}$/.test(mac)) {
      status.textContent = "The MAC has six pairs of hex digits, like C6:1E:0D:E0:0C:FB.";
      return;
    }
    void storeIdentity(mac);
  });
  $("mock-id-default").addEventListener("click", () => void storeIdentity("default"));

  // --- Settings: media list ---

  async function applyMedia(count) {
    if (!Number.isInteger(count) || count < 0 || count > 1000) {
      status.textContent = "The media list holds 0 to 1000 recordings.";
      return;
    }
    status.textContent = "";
    mediaForm.elements.count.value = String(count);
    const lines = await send(`media ${count}`);
    if (lines === null) {
      return;
    }
    status.textContent = problemIn(lines) ?? `${count} recordings (until the next reset).`;
    void refresh(false);
  }
  mediaForm.addEventListener("submit", (event) => {
    event.preventDefault();
    void applyMedia(Number(mediaForm.elements.count.value));
  });
  for (const button of view.querySelectorAll("[data-media-preset]")) {
    button.addEventListener("click", () => void applyMedia(Number(button.dataset.mediaPreset)));
  }

  // --- Fault injection ---

  async function fault(argumentsText) {
    status.textContent = "";
    const lines = await send(`fault ${argumentsText}`);
    if (lines === null) {
      return;
    }
    const problem = problemIn(lines);
    if (problem) {
      status.textContent = problem;
      return;
    }
    const line = lines.find((text) => text.startsWith("faults:"));
    if (line) {
      showFaults(line);
    }
  }

  for (const row of faultRows) {
    const name = row.dataset.fault;
    const input = row.querySelector("input");
    if (row.dataset.toggle) {
      input.addEventListener("change", () => void fault(`${name} ${input.checked ? "on" : "off"}`));
      continue;
    }
    row.querySelector("button:not([data-fault-off])").addEventListener("click", () => {
      if (input.value.trim() === "" || !input.checkValidity()) {
        status.textContent = `Check the value of "${row.querySelector("label").textContent.trim()}".`;
        return;
      }
      void fault(`${name} ${input.value.trim()}`);
    });
    row.querySelector("[data-fault-off]")?.addEventListener("click", () => void fault(`${name} off`));
  }
  $("mock-faults-off").addEventListener("click", () => void fault("off"));
})();
