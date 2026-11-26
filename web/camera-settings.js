// "Camera settings" in the Camera tab of the advanced view (#197): reads the
// settings each camera reported at its last read with the console command
// `camset`, shows them as a form and sends only what changed with
// `camset <devId> name=value ...`. The hub queues one `settings` action, the
// same path and checks as the web UI and the gallery
// (docs/camera-actions.md); the camera takes it at its next read.
(function () {
  "use strict";

  const section = document.getElementById("cam-settings-section");
  if (!section) {
    return;
  }
  const serial = () => window.trailcamSerial;
  const readButton = document.getElementById("cam-settings-read");
  const box = document.getElementById("cam-settings");
  const note = document.getElementById("cam-settings-note");

  const seconds = (s) =>
    s < 60 ? `${s} s` : s < 3600 ? `${s / 60} min` : `${s / 3600} h`;
  const list = (...pairs) => pairs.map(([value, label]) => ({ value, label }));
  const secondChoices = (...values) => values.map((value) => ({ value, label: seconds(value) }));
  const PHOTO = { 9: "48 MP", 10: "60 MP" };
  const VIDEO = { 1: "720p", 2: "1080p", 4: "2.7K", 5: "4K" };

  // The allowlist of components/camera_link/camera_settings.cpp, in its order.
  // kind: "choice" (choices or from the capability), "number" (min..max),
  // "switch" (0 / 1), "language". The detection area needs the live picture
  // (#198) and is left out.
  const FIELDS = [
    { name: "workMode", label: "Trigger", kind: "choice", choices: list([0, "Motion"], [1, "Interval"], [2, "Time-lapse"]) },
    { name: "shootMode", label: "Record", kind: "choice", choices: list([0, "Photo"], [1, "Video"], [2, "Photo and video"]) },
    { name: "picResolution", label: "Photo resolution", kind: "choice", from: "images", text: (v) => PHOTO[v] ?? `level ${v}` },
    { name: "videoResolution", label: "Video resolution", kind: "choice", from: "videos", text: (v) => VIDEO[v] ?? `level ${v}` },
    { name: "continuousShootCnt", label: "Photos per trigger", kind: "number", min: 1, max: 5 },
    { name: "recordDuration", label: "Video length (s)", kind: "number", min: 1, max: 600 },
    { name: "recordAudio", label: "Record sound", kind: "switch" },
    { name: "timelapseInterval", label: "Interval", kind: "choice", choices: secondChoices(30, 60, 300, 600, 1800, 3600, 7200, 21600, 43200, 86400) },
    { name: "pirSens", label: "Motion sensitivity", kind: "choice", choices: list([0, "Low"], [1, "Medium"], [2, "High"]) },
    { name: "pirCoolingTime", label: "Pause after a trigger", kind: "choice", choices: secondChoices(5, 10, 20, 30, 60, 120, 300, 600, 1800, 3600) },
    { name: "sidePirOnOff", label: "Side motion sensors", kind: "switch" },
    { name: "workTimeOnOff", label: "Only at set times", kind: "switch" },
    { name: "workTimeStartHour", label: "From (hour)", kind: "number", min: 0, max: 23 },
    { name: "workTimeStartMin", label: "From (minute)", kind: "number", min: 0, max: 59 },
    { name: "workTimeEndHour", label: "To (hour)", kind: "number", min: 0, max: 23 },
    { name: "workTimeEndMin", label: "To (minute)", kind: "number", min: 0, max: 59 },
    { name: "enableAiFilter", label: "Detection area on", kind: "switch" },
    { name: "infraRedLum", label: "Infrared brightness", kind: "number", min: 1, max: 3 },
    { name: "osdInfoEnable", label: "Info bar in the picture", kind: "switch" },
    { name: "WiFiLedEnable", label: "Wi-Fi LED", kind: "switch" },
    { name: "dateFormat", label: "Date format", kind: "number", min: 0, max: 2 },
    { name: "timeFormat", label: "Time format", kind: "number", min: 0, max: 2 },
    { name: "languageCode", label: "Language", kind: "language" },
    { name: "micVol", label: "Microphone volume (%)", kind: "number", min: 0, max: 100 },
    { name: "spkVol", label: "Speaker volume (%)", kind: "number", min: 0, max: 100 },
  ];

  // "a=1 b=de" → { a: "1", b: "de" }.
  function pairs(text) {
    const out = {};
    for (const word of text.trim().split(/\s+/)) {
      const eq = word.indexOf("=");
      if (eq > 0) {
        out[word.slice(0, eq)] = word.slice(eq + 1);
      }
    }
    return out;
  }

  // The answer of `camset`: [{ id, values: {name: "text"} | null, capability: {images: [..], …} }].
  function parseCameras(lines) {
    const cameras = new Map();
    for (const line of lines) {
      const settings = /^INFO camset (\S+) (.*)$/.exec(line);
      if (settings) {
        const id = settings[1];
        const rest = settings[2];
        cameras.set(id, {
          id,
          values: rest.startsWith("(") ? null : pairs(rest),
          capability: {},
        });
        continue;
      }
      const capability = /^INFO capability (\S+) (.*)$/.exec(line);
      if (capability && cameras.has(capability[1])) {
        const parsed = pairs(capability[2]);
        const split = (key) => (parsed[key] ? parsed[key].split(",") : []);
        cameras.get(capability[1]).capability = {
          images: split("images").map(Number),
          videos: split("videos").map(Number),
          languages: split("languages"),
        };
      }
    }
    return [...cameras.values()];
  }

  function control(field, value, capability) {
    let input;
    if (field.kind === "switch") {
      input = document.createElement("input");
      input.type = "checkbox";
      input.checked = value === "1";
      input.dataset.value = value;
      input.readValue = () => (input.checked ? "1" : "0");
      return input;
    }
    if (field.kind === "number") {
      input = document.createElement("input");
      input.type = "number";
      input.min = String(field.min);
      input.max = String(field.max);
      input.step = "1";
      input.value = value;
    } else {
      input = document.createElement("select");
      let choices = field.choices ?? [];
      if (field.from) {
        choices = (capability[field.from] ?? []).map((v) => ({ value: v, label: field.text(v) }));
      }
      if (field.kind === "language") {
        choices = (capability.languages ?? []).map((v) => ({ value: v, label: v }));
      }
      // The current value stays selectable even if no list has it.
      if (!choices.some((c) => String(c.value) === value)) {
        choices = [{ value, label: field.text ? field.text(value) : `${value}` }, ...choices];
      }
      for (const choice of choices) {
        const option = document.createElement("option");
        option.value = String(choice.value);
        option.textContent = choice.label;
        input.append(option);
      }
      input.value = value;
    }
    input.dataset.value = value;
    input.readValue = () => input.value.trim();
    return input;
  }

  function valid(field, text) {
    if (field.kind !== "number") {
      return text !== "";
    }
    if (!/^\d+$/.test(text)) {
      return false;
    }
    const n = Number(text);
    return n >= field.min && n <= field.max;
  }

  function renderCamera(camera) {
    const form = document.createElement("form");
    form.className = "cam-settings-form";
    const fieldset = document.createElement("fieldset");
    const legend = document.createElement("legend");
    legend.textContent = `Camera ${camera.id}`;
    fieldset.append(legend);
    if (!camera.values) {
      const empty = document.createElement("p");
      empty.className = "note";
      empty.textContent = "No settings read yet. They come with the next read of the camera.";
      fieldset.append(empty);
      form.append(fieldset);
      return form;
    }
    const grid = document.createElement("div");
    grid.className = "cam-settings-grid";
    const inputs = [];
    for (const field of FIELDS) {
      const value = camera.values[field.name];
      if (value === undefined) {
        continue; // not reported: the camera does not support it
      }
      const id = `cs-${camera.id}-${field.name}`;
      const label = document.createElement("label");
      label.htmlFor = id;
      label.textContent = field.label;
      label.title = field.name;
      const input = control(field, value, camera.capability);
      input.id = id;
      grid.append(label, input);
      inputs.push({ field, input });
    }
    fieldset.append(grid);
    const status = document.createElement("p");
    status.className = "note";
    status.setAttribute("role", "status");
    const apply = document.createElement("button");
    apply.type = "submit";
    apply.className = "primary-button";
    apply.textContent = "Apply changes";
    fieldset.append(apply, status);
    form.append(fieldset);

    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      const changed = [];
      for (const { field, input } of inputs) {
        const text = input.readValue();
        if (text === input.dataset.value) {
          continue;
        }
        if (!valid(field, text)) {
          status.textContent = `${field.label}: ${field.min} to ${field.max}.`;
          status.classList.add("wz-problem");
          input.focus();
          return;
        }
        changed.push(`${field.name}=${text}`);
      }
      status.classList.remove("wz-problem");
      if (changed.length === 0) {
        status.textContent = "Nothing changed.";
        return;
      }
      apply.disabled = true;
      status.textContent = "Sending…";
      try {
        const answer = await serial().command(`camset ${camera.id} ${changed.join(" ")}`, 10000);
        const error = answer.find((text) => text.startsWith("ERR "));
        if (error) {
          throw new Error(error.slice(4));
        }
        const ok = (answer.find((text) => text.startsWith("OK ")) ?? "OK ").slice(3);
        status.textContent = `${ok}. The camera takes it at its next read; see Actions.`;
        for (const { input } of inputs) {
          input.dataset.value = input.readValue();
        }
      } catch (error) {
        status.textContent = `Not sent: ${error.message}`;
        status.classList.add("wz-problem");
      } finally {
        apply.disabled = false;
      }
    });
    return form;
  }

  readButton.addEventListener("click", async () => {
    readButton.disabled = true;
    note.textContent = "Reading…";
    note.classList.remove("wz-problem");
    try {
      const answer = await serial().command("camset", 10000);
      const error = answer.find((text) => text.startsWith("ERR "));
      if (error) {
        throw new Error(`${error.slice(4)} (older firmware?)`);
      }
      const cameras = parseCameras(answer);
      box.replaceChildren(...cameras.map(renderCamera));
      note.textContent =
        cameras.length === 0
          ? "No camera read yet."
          : "As each camera reported them at its last read. Settings the camera does not support are not shown; hover a label for the camera's field name.";
    } catch (error) {
      note.textContent = `Not read: ${error.message}`;
      note.classList.add("wz-problem");
    } finally {
      readButton.disabled = !serial()?.isConnected();
    }
  });

  window.addEventListener("serial-disconnected", () => box.replaceChildren());
})();
