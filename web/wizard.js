// Setup assistant: board → install → connect → questions → save → check.
// Uses the serial connection of serial-console.js (window.trailcamSerial) and
// stores the answers with the console commands "config set-hex"/"unset"
// (docs/modes.md). The settings form under "Advanced" still offers every
// setting on its own.
(function () {
  "use strict";

  const root = document.getElementById("wizard");
  if (!root) {
    return;
  }
  const serial = () => window.trailcamSerial;
  const $ = (selector) => root.querySelector(selector);

  // Used if boards.json cannot be loaded (e.g. the page opened from a file).
  const FALLBACK_BOARDS = [
    {
      id: "esp32s3",
      name: "Waveshare ESP32-S3-A7670E-4G",
      capabilities: ["modem", "battery_gauge", "psram", "deep_sleep"],
      preset: { power: "battery", direction: "push" },
      installer: true,
      notes: "Main board: LTE modem, 18650 holder with solar input, battery gauge.",
    },
  ];
  const CAPABILITY_TEXT = {
    modem: "mobile network",
    ethernet: "Ethernet",
    battery_gauge: "battery gauge",
    psram: "PSRAM",
    display: "display",
    button: "button",
    web_ui: "web page",
    deep_sleep: "deep sleep",
  };
  const DONE_RESULT = {
    "camera-scanned": { camera: true },
    "camera-listed": { camera: true },
    "uplink-done": { camera: true, online: true, sent: true },
    "camera-unreachable": { camera: false },
    "uplink-unreachable": { camera: true, online: false },
    stopped: {},
  };

  const state = {
    boards: FALLBACK_BOARDS,
    board: null,
    step: 1,
    hub: null, // facts from "status"
    saved: false,
  };

  // --- Steps ---

  const steps = [...root.querySelectorAll(".wz-step")];
  const progress = [...root.querySelectorAll(".wz-progress li")];

  function show(step) {
    state.step = step;
    for (const element of steps) {
      element.hidden = Number(element.dataset.step) !== step;
    }
    for (const item of progress) {
      const n = Number(item.dataset.step);
      item.classList.toggle("done", n < step);
      if (n === step) {
        item.setAttribute("aria-current", "step");
      } else {
        item.removeAttribute("aria-current");
      }
    }
    if (step === 4) {
      prepareQuestions();
    }
    if (step === 6) {
      void runChecks();
    }
    root.scrollIntoView({ behavior: "smooth", block: "start" });
  }

  function stepElement(step) {
    return steps.find((element) => Number(element.dataset.step) === step);
  }

  function nextButton(step) {
    return stepElement(step).querySelector("[data-next]");
  }

  for (const element of steps) {
    const step = Number(element.dataset.step);
    element.querySelector("[data-next]")?.addEventListener("click", () => show(step + 1));
    element.querySelector("[data-back]")?.addEventListener("click", () => show(step - 1));
  }

  const has = (capability) => Boolean(state.board?.capabilities.includes(capability));

  // --- 1. Board ---

  function renderBoards() {
    const container = $("#wz-boards");
    container.replaceChildren();
    for (const board of state.boards) {
      const label = document.createElement("label");
      label.className = "wz-choice";
      const input = document.createElement("input");
      input.type = "radio";
      input.name = "board";
      input.value = board.id;
      input.checked = state.board?.id === board.id;
      input.addEventListener("change", () => selectBoard(board.id));
      const title = document.createElement("strong");
      title.textContent = board.name;
      const caps = document.createElement("small");
      caps.textContent = board.capabilities.map((c) => CAPABILITY_TEXT[c] ?? c).join(" · ");
      const notes = document.createElement("small");
      notes.textContent = board.notes ?? "";
      label.append(input, title, caps, notes);
      container.append(label);
    }
  }

  function selectBoard(id) {
    state.board = state.boards.find((board) => board.id === id) ?? null;
    nextButton(1).disabled = !state.board;
    for (const input of root.querySelectorAll('input[name="board"]')) {
      input.checked = input.value === id;
    }
    if (state.board) {
      window.trailcamInstaller?.setBoard(state.board.id);
    }
    showInstallable();
    $("#wz-image").textContent = state.board ? `trailcamhub-${state.board.id}-release.bin` : "";
  }

  // The install button only for a board the chosen build has an image of.
  function showInstallable() {
    const installer = window.trailcamInstaller;
    const installable =
      Boolean(state.board?.installer) && (!installer || installer.hasImage(state.board.id));
    $("#wz-install-ok").hidden = !installable;
    $("#wz-install-none").hidden = installable;
  }
  window.trailcamInstaller?.onChange(showInstallable);

  fetch("./boards.json", { cache: "no-store" })
    .then((response) => (response.ok ? response.json() : Promise.reject()))
    .then((data) => {
      if (Array.isArray(data?.boards) && data.boards.length > 0) {
        state.boards = data.boards;
      }
    })
    .catch(() => {})
    .finally(() => {
      renderBoards();
      if (state.boards.length === 1) {
        selectBoard(state.boards[0].id);
      }
    });

  // The install button points at the latest release build (installer.js).
  const buildInfo = document.getElementById("build-info");
  const showBuild = () => {
    $("#wz-build").textContent = buildInfo?.textContent.trim() || "latest";
  };
  if (buildInfo) {
    new MutationObserver(showBuild).observe(buildInfo, { childList: true, characterData: true, subtree: true });
  }

  // --- 3. Connect ---

  const connectStatus = $("#wz-connect-status");

  function parseStatus(lines) {
    const facts = { lines };
    for (const line of lines) {
      const text = line.replace(/^(INFO|CONFIG|STATE|READY) /, "");
      let match;
      if ((match = text.match(/^firmware (\S+) \((.+)-(dev|release)\)/))) {
        facts.version = match[1];
        facts.board = match[2];
        facts.variant = match[3];
      } else if (text.startsWith("mode ")) {
        facts.mode = text.slice(5);
      } else if (text.startsWith("SD card")) {
        facts.sd = text;
      } else if (text.startsWith("battery ")) {
        facts.battery = text;
      } else if (line.startsWith("STATE ")) {
        facts.state = text;
      }
    }
    return facts;
  }

  async function readHub() {
    connectStatus.textContent = "Asking the hub…";
    try {
      const facts = parseStatus(await serial().command("status", 8000));
      state.hub = facts;
      const list = $("#wz-hub");
      list.replaceChildren();
      const add = (text) => {
        const item = document.createElement("li");
        item.textContent = text;
        list.append(item);
      };
      add(facts.version ? `Firmware ${facts.version} for ${facts.board} (${facts.variant})` : "Firmware: unknown (an older build)");
      if (facts.mode) {
        add(`Now: ${facts.mode}`);
      }
      if (facts.sd) {
        add(facts.sd);
      }
      if (facts.battery) {
        add(`Hub ${facts.battery}`);
      }
      list.hidden = false;
      if (facts.board && facts.board !== state.board?.id && state.boards.some((b) => b.id === facts.board)) {
        selectBoard(facts.board);
        add(`The firmware is for ${state.board.name}; the questions follow that board.`);
      }
      connectStatus.textContent = "Connected";
      nextButton(3).disabled = false;
      $("#wz-reset").hidden = false;
    } catch {
      connectStatus.textContent =
        "The hub does not answer. It may be asleep or still starting: reset it and wait a few seconds.";
      $("#wz-reset").hidden = false;
    }
  }

  $("#wz-connect").addEventListener("click", async () => {
    if (!serial()) {
      return;
    }
    if (!serial().isConnected()) {
      connectStatus.textContent = "Choose the hub's serial port…";
      const ok = await serial().connect();
      if (!ok && !serial().isConnected()) {
        connectStatus.textContent = "Not connected";
        return;
      }
    }
    // The boot log may still be running.
    await new Promise((resolve) => setTimeout(resolve, 500));
    await readHub();
  });

  $("#wz-reset").addEventListener("click", async () => {
    connectStatus.textContent = "Resetting…";
    try {
      const info = await serial().reset();
      // The USB-Serial-JTAG of the S3/C5 goes away during a reset.
      await new Promise((resolve) => setTimeout(resolve, 1500));
      if (!serial().isConnected() && !(await serial().reconnect(info))) {
        connectStatus.textContent = "The board restarted. Press Connect again.";
        return;
      }
      await new Promise((resolve) => setTimeout(resolve, 3000));
      await readHub();
    } catch (error) {
      connectStatus.textContent = `Could not reset: ${error.message}. Press the board's reset button.`;
    }
  });

  window.addEventListener("serial-disconnected", () => {
    if (state.step === 3) {
      connectStatus.textContent = "Not connected";
    }
  });

  // --- 4. Questions ---

  const form = $("#wz-form");
  const problem = $("#wz-form-problem");

  function value(name) {
    const field = form.elements[name];
    return field ? String(field.value ?? "").trim() : "";
  }

  function setChoice(name, choice) {
    for (const input of form.querySelectorAll(`input[name="${name}"]`)) {
      input.checked = input.value === choice;
    }
  }

  function prepareQuestions() {
    // Options the board cannot do are hidden.
    for (const label of form.querySelectorAll("[data-needs]")) {
      const ok = has(label.dataset.needs);
      label.hidden = !ok;
      if (!ok) {
        label.querySelector("input").checked = false;
      }
    }
    // Battery needs deep sleep in the build.
    const batteryLabel = form.querySelector('input[name="power"][value="battery"]').closest("label");
    batteryLabel.hidden = !has("deep_sleep");
    if (!value("power")) {
      const preset = state.board?.preset?.power;
      setChoice("power", preset === "battery" && has("deep_sleep") ? "battery" : "mains");
    }
    update();
  }

  // Shows the questions and fields that apply to the answers so far.
  function update() {
    const power = value("power");
    const goal = value("goal");
    const pullLabel = form.querySelector('input[name="goal"][value="pull"]').closest("label");
    pullLabel.classList.toggle("wz-disabled", power === "battery");
    if (power === "battery" && goal === "pull") {
      setChoice("goal", "");
    }

    const allowed = new Set();
    if (goal === "telegram") {
      allowed.add("modem");
    } else if (goal === "pull") {
      allowed.add("wifi");
      allowed.add("ap");
      if (has("ethernet")) allowed.add("ethernet");
    } else if (goal) {
      allowed.add("wifi");
      if (has("ethernet")) allowed.add("ethernet");
      if (has("modem")) allowed.add("modem");
    }
    for (const label of form.querySelectorAll("[data-transport]")) {
      const ok = allowed.has(label.dataset.transport);
      label.hidden = !ok;
      if (!ok) {
        label.querySelector("input").checked = false;
      }
    }
    $("#wz-q-transport").hidden = allowed.size === 0;
    if (allowed.size === 1) {
      setChoice("transport", [...allowed][0]);
    } else if (!value("transport") && allowed.size > 0) {
      const preferred = ["ethernet", "wifi", "modem"].find((t) => allowed.has(t));
      setChoice("transport", preferred);
    }

    const transport = value("transport");
    const visible = {
      wifi: transport === "wifi",
      modem: transport === "modem",
      webdav: goal === "webdav",
      telegram: goal === "telegram",
      backend: goal === "backend",
      push: Boolean(goal) && goal !== "pull",
    };
    for (const group of form.querySelectorAll(".wz-fields")) {
      group.hidden = !visible[group.dataset.for];
    }
    problem.textContent = "";
  }

  form.addEventListener("change", update);

  // The answers as settings; null if something is missing.
  function settingsFromAnswers() {
    const power = value("power");
    const goal = value("goal");
    const transport = value("transport");
    if (!power || !goal || !transport) {
      return { error: "Please answer all three questions." };
    }
    const set = { power };
    const unset = [];
    if (goal === "pull") {
      set.direction = "pull";
    } else {
      set.direction = "push";
      set.destination = goal;
    }
    if (transport === "ap") {
      // No home network: the hub keeps its own access point.
      set.transport = "wifi";
      unset.push("wifi_ssid", "wifi_password");
    } else {
      set.transport = transport;
    }
    const need = (name, text) => {
      const v = name.endsWith("password") ? form.elements[name].value : value(name);
      if (!v) {
        throw new Error(text);
      }
      set[name] = v;
    };
    const optional = (name) => {
      const v = name.endsWith("password") ? form.elements[name].value : value(name);
      if (v) {
        set[name] = v;
      }
    };
    try {
      if (transport === "wifi") {
        need("wifi_ssid", "Enter the name of your home Wi-Fi.");
        need("wifi_password", "Enter the password of your home Wi-Fi.");
        if (set.wifi_password.length < 8 || set.wifi_password.length > 64) {
          throw new Error("A Wi-Fi password has 8 to 64 characters.");
        }
      }
      if (transport === "modem") {
        optional("apn");
        optional("sim_pin");
        if (set.apn && !/^[A-Za-z0-9._-]{1,63}$/.test(set.apn)) {
          throw new Error("The APN may only contain letters, digits, '.', '-' and '_'.");
        }
        if (set.sim_pin && !/^\d{4}$/.test(set.sim_pin)) {
          throw new Error("The SIM PIN must have 4 digits.");
        }
      }
      if (goal === "webdav") {
        need("webdav_url", "Enter the URL of the WebDAV folder.");
        if (!/^https?:\/\/\S+$/.test(set.webdav_url)) {
          throw new Error("The folder URL must start with https:// or http://.");
        }
        optional("webdav_user");
        optional("webdav_password");
      }
      if (goal === "telegram") {
        need("telegram_token", "Paste the bot token from @BotFather.");
        if (!/^\d{1,20}:[A-Za-z0-9_-]{30,}$/.test(set.telegram_token)) {
          throw new Error("The bot token looks like 123456789:AA… (from @BotFather).");
        }
      }
      if (goal === "backend") {
        need("api_url", "Enter the backend's API URL.");
        need("device_key", "Enter the hub's device key.");
        if (!/^https:\/\/\S+$/.test(set.api_url)) {
          throw new Error("The API URL must start with https://.");
        }
      }
      if (goal !== "pull") {
        optional("health_url");
        if (set.health_url && !/^https?:\/\/\S+$/.test(set.health_url)) {
          throw new Error("The health check URL must start with https:// or http://.");
        }
      }
    } catch (error) {
      return { error: error.message };
    }
    return { set, unset };
  }

  // --- 5. Save ---

  const SECRETS = ["wifi_password", "device_key", "sim_pin", "webdav_password", "telegram_token"];

  function toHex(text) {
    return Array.from(new TextEncoder().encode(text), (b) => b.toString(16).padStart(2, "0")).join("");
  }

  function checkItem(list, text) {
    const item = document.createElement("li");
    item.className = "pending";
    item.textContent = text;
    list.append(item);
    return {
      ok(detail) {
        item.className = "ok";
        if (detail) item.textContent = `${text}: ${detail}`;
      },
      fail(detail) {
        item.className = "fail";
        item.textContent = `${text}: ${detail}`;
      },
      skip(detail) {
        item.className = "skip";
        item.textContent = `${text}: ${detail}`;
      },
    };
  }

  // Sends one command and fails on "TCH ERR".
  async function run(line) {
    const answer = await serial().command(line, 10000);
    const error = answer.find((l) => l.startsWith("ERR "));
    if (error) {
      throw new Error(error.slice(4));
    }
    return answer;
  }

  $("#wz-save").addEventListener("click", async () => {
    const result = settingsFromAnswers();
    if (result.error) {
      problem.textContent = result.error;
      return;
    }
    if (!serial()?.isConnected()) {
      problem.textContent = "Not connected to the hub: go back to step 3.";
      return;
    }
    show(5);
    const list = $("#wz-save-list");
    list.replaceChildren();
    const next = nextButton(5);
    next.disabled = true;
    let failed = false;
    // The mode first: the hub checks some values against it.
    const order = ["power", "direction", "transport", "destination"];
    const keys = [...order.filter((k) => k in result.set), ...Object.keys(result.set).filter((k) => !order.includes(k))];
    for (const key of keys) {
      const shown = SECRETS.includes(key) ? "••••" : result.set[key];
      const item = checkItem(list, `${key} = ${shown}`);
      try {
        await run(`config set-hex ${key} ${toHex(result.set[key])}`);
        item.ok();
      } catch (error) {
        item.fail(error.message);
        failed = true;
      }
    }
    for (const key of result.unset) {
      const item = checkItem(list, `${key} erased`);
      try {
        await run(`config unset ${key}`);
        item.ok();
      } catch (error) {
        item.fail(error.message);
        failed = true;
      }
    }
    for (const name of SECRETS) {
      if (form.elements[name]) form.elements[name].value = "";
    }
    const done = checkItem(list, failed ? "Some settings were not stored" : "All settings stored");
    if (failed) {
      done.fail("go back, correct them and save again");
    } else {
      done.ok();
      state.saved = true;
      next.disabled = false;
    }
  });

  // --- 6. Check ---

  const checks = $("#wz-checks");

  async function runChecks() {
    checks.replaceChildren();
    $("#wz-next").replaceChildren();
    const answers = checkItem(checks, "Hub answers");
    let facts;
    try {
      facts = parseStatus(await serial().command("status", 8000));
      answers.ok(facts.version ? `firmware ${facts.version}` : undefined);
    } catch (error) {
      answers.fail(`${error.message}; reset it in step 3`);
      return;
    }
    const sd = checkItem(checks, "SD card");
    if (facts.sd && !facts.sd.includes("not mounted")) {
      sd.ok(facts.sd.replace(/^SD card /, ""));
    } else {
      sd.fail("not found: insert a FAT32-formatted microSD card");
    }
    if (has("battery_gauge")) {
      const battery = checkItem(checks, "Hub battery");
      if (facts.battery && !facts.battery.includes("unknown")) {
        battery.ok(facts.battery.replace(/^battery /, ""));
      } else {
        battery.fail("no reading: is the 18650 cell in its holder?");
      }
    }
    const mode = checkItem(checks, "Mode");
    if (facts.mode) {
      mode.ok(facts.mode);
    } else {
      mode.skip("unknown (older firmware)");
    }
    const goal = value("goal");
    const settings = checkItem(checks, goal === "pull" ? "Web page" : "Destination settings");
    try {
      const config = await serial().command("config show", 8000);
      const backend = config.find((l) => l.startsWith("CONFIG backend "));
      if (goal === "pull") {
        const wifi = value("transport") === "wifi";
        settings.ok(
          has("ethernet") && value("transport") === "ethernet"
            ? "at the hub's Ethernet address and http://trailcamhub.local (password in the boot log)"
            : wifi
              ? "in your home Wi-Fi at http://trailcamhub.local (password: the hub's Wi-Fi password, on its display)"
              : "join the Wi-Fi TrailCamHub-XXXX, then open http://10.42.0.1",
        );
      } else if (backend && backend.includes("configured")) {
        settings.ok(backend.slice("CONFIG backend ".length));
      } else {
        settings.fail(backend ? backend.slice("CONFIG ".length) : "incomplete");
      }
    } catch (error) {
      settings.fail(error.message);
    }
    if (goal === "telegram") {
      const item = document.createElement("p");
      item.className = "note";
      item.textContent =
        "Now send /start to your bot in Telegram. The hub pairs with your chat at its next check-in (the camera search below does one).";
      $("#wz-next").append(item);
    }
  }

  $("#wz-recheck").addEventListener("click", () => void runChecks());

  // A search runs for minutes; its result comes as "TCH EVENT done <result>".
  let searching = false;
  $("#wz-scan").addEventListener("click", async () => {
    if (searching) {
      return;
    }
    searching = true;
    const camera = checkItem(checks, "Camera found and read");
    const online = value("goal") !== "pull" ? checkItem(checks, "Pictures sent") : null;
    const problems = [];
    const outcome = await new Promise((resolve) => {
      const timer = setTimeout(() => finish("timeout"), 10 * 60 * 1000);
      const onLine = (event) => {
        const line = event.detail;
        let match;
        if ((match = line.match(/^TCH EVENT done (\S+)(.*)$/))) {
          if (match[2].includes("no-card")) problems.push("the camera has no SD card");
          finish(match[1]);
        } else if ((match = line.match(/^TCH EVENT uplink failed (.*)$/))) {
          problems.push(match[1]);
        }
      };
      const finish = (result) => {
        clearTimeout(timer);
        window.removeEventListener("serial-line", onLine);
        resolve(result);
      };
      window.addEventListener("serial-line", onLine);
      run("scan").catch((error) => finish(`error ${error.message}`));
    });
    searching = false;
    const result = DONE_RESULT[outcome];
    if (!result) {
      camera.fail(outcome === "timeout" ? "no result within 10 minutes" : outcome.replace(/^error /, ""));
      online?.skip("not tried");
      return;
    }
    if (result.camera === true) {
      camera.ok(problems.includes("the camera has no SD card") ? "but it has no SD card" : undefined);
    } else if (result.camera === false) {
      camera.fail("not reached: is it switched on, nearby and paired with the TrailCam Go app before?");
    } else {
      camera.skip("stopped");
    }
    if (online) {
      if (result.sent) {
        online.ok();
      } else if (result.online === false) {
        online.fail(value("transport") === "modem" ? "not online over the mobile network (SIM, APN, signal)" : "the home Wi-Fi was not joined");
      } else if (problems.length > 0) {
        online.fail(problems.join("; "));
      } else {
        online.skip("not tried");
      }
    }
  });

  show(1);
})();
