// "SD card" tab: browse the hub's SD card (ls), download any file (cat, in
// chunks, with gaps requested again), delete files and directories (rm, with
// a confirmation), plus the diagnostics `heap` and `update` (#156).
//
// The commands exist only in builds with CONFIG_HUB_CONSOLE_DIAGNOSTICS; the
// tab reads `help` after connecting and says so if they are missing.
(function () {
  "use strict";

  const serial = () => window.trailcamSerial;
  const panel = document.getElementById("adv-panel-sd");
  if (!panel) {
    return;
  }
  const statusLine = document.getElementById("sd-status");
  const crumbs = document.getElementById("sd-path");
  const body = document.getElementById("sd-files");
  const refresh = document.getElementById("sd-refresh");
  const dialog = document.getElementById("sd-confirm");
  const diagOut = document.getElementById("sd-diag-output");
  const diagButtons = [
    ...panel.querySelectorAll("[data-sd-command]"),
    document.getElementById("sd-status-read"),
  ];
  const statusRows = document.getElementById("sd-status-rows");

  // Path below the card's root, "" for the root.
  let path = "";
  let busy = false;
  let available = null; // null: not checked yet

  const join = (dir, name) => (dir ? `${dir}/${name}` : name);

  function say(text) {
    statusLine.textContent = text;
  }

  function setBusy(on) {
    busy = on;
    refresh.disabled = on || !serial()?.isConnected();
    for (const button of [...body.querySelectorAll("button"), ...diagButtons]) {
      button.disabled = on || !serial()?.isConnected();
    }
  }

  function formatSize(bytes) {
    if (bytes < 1024) {
      return `${bytes} B`;
    }
    if (bytes < 1024 * 1024) {
      return `${(bytes / 1024).toFixed(1)} KB`;
    }
    return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
  }

  function formatTime(seconds) {
    if (!seconds) {
      return "–";
    }
    return new Date(seconds * 1000)
      .toISOString()
      .slice(0, 16)
      .replace("T", " ");
  }

  // Parses `ls` lines ("INFO <d|f> <size> <mtime> <name>"); directories
  // first, then by name. Skips "." and "..".
  function parseListing(lines) {
    return lines
      .map((line) => line.match(/^INFO ([df]) (-?\d+) (-?\d+) (.+)$/))
      .filter(Boolean)
      .map((m) => ({
        dir: m[1] === "d",
        size: Number(m[2]),
        time: Number(m[3]),
        name: m[4],
      }))
      .filter((entry) => entry.name !== "." && entry.name !== "..")
      .sort(
        (a, b) => Number(b.dir) - Number(a.dir) || a.name.localeCompare(b.name),
      );
  }

  function renderCrumbs() {
    crumbs.replaceChildren();
    const parts = path ? path.split("/") : [];
    const add = (label, target) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "link-button";
      button.textContent = label;
      button.addEventListener("click", () => open(target));
      crumbs.append(button);
    };
    add("SD card", "");
    parts.forEach((part, i) => {
      crumbs.append(" / ");
      add(part, parts.slice(0, i + 1).join("/"));
    });
  }

  function cell(text) {
    const td = document.createElement("td");
    td.textContent = text;
    return td;
  }

  function actionButton(label, title, onClick, danger) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = danger ? "secondary-button danger" : "secondary-button";
    button.textContent = label;
    button.title = title;
    button.addEventListener("click", onClick);
    return button;
  }

  function render(entries) {
    body.replaceChildren();
    if (entries.length === 0) {
      const row = document.createElement("tr");
      const td = cell("Empty.");
      td.colSpan = 4;
      row.append(td);
      body.append(row);
      return;
    }
    for (const entry of entries) {
      const row = document.createElement("tr");
      const name = document.createElement("td");
      if (entry.dir) {
        const button = document.createElement("button");
        button.type = "button";
        button.className = "link-button";
        button.textContent = `${entry.name}/`;
        button.addEventListener("click", () => open(join(path, entry.name)));
        name.append(button);
      } else {
        name.textContent = entry.name;
      }
      const actions = document.createElement("td");
      actions.className = "sd-actions";
      if (!entry.dir) {
        actions.append(
          actionButton("Download", `Download ${entry.name}`, () =>
            download(entry),
          ),
        );
      }
      actions.append(
        actionButton(
          "Delete…",
          `Delete ${entry.name}`,
          () => remove(entry),
          true,
        ),
      );
      row.append(
        name,
        cell(entry.dir ? "–" : formatSize(entry.size)),
        cell(formatTime(entry.time)),
        actions,
      );
      body.append(row);
    }
  }

  let helpText = null;
  async function hasCommand(name) {
    if (helpText === null) {
      helpText = (await serial().command("help", 10000)).join("\n");
    }
    return new RegExp(`(^|\\s)${name}(\\s|$)`, "m").test(helpText);
  }

  async function checkAvailable() {
    if (available === null) {
      available = (await hasCommand("ls")) && (await hasCommand("cat"));
    }
    return available;
  }

  async function open(target) {
    if (busy || !serial()?.isConnected()) {
      say("Connect to the hub first.");
      return;
    }
    setBusy(true);
    try {
      if (!(await checkAvailable())) {
        say(
          "This firmware has no SD card commands (built without CONFIG_HUB_CONSOLE_DIAGNOSTICS).",
        );
        return;
      }
      say("Reading…");
      const lines = await serial().command(`ls ${target || "/"}`, 15000);
      const error = lines.find((line) => line.startsWith("ERR "));
      if (error) {
        say(`Cannot list ${target || "the root"}: ${error.slice(4)}`);
        return;
      }
      path = target;
      const entries = parseListing(lines);
      renderCrumbs();
      render(entries);
      say(`${entries.length} entr${entries.length === 1 ? "y" : "ies"}.`);
    } catch (error) {
      say(`No answer: ${error.message}. The hub may be asleep: reset it.`);
    } finally {
      setBusy(false);
    }
  }

  function base64ToBytes(text) {
    const binary = atob(text);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) {
      bytes[i] = binary.charCodeAt(i);
    }
    return bytes;
  }

  // Reads the file in chunks. The USB console can drop output, so only the
  // bytes up to the first gap count; the next request starts there.
  const CHUNK = 64 * 1024;
  async function readFile(target, size, progress) {
    const parts = [];
    let have = 0;
    let stalls = 0;
    while (have < size) {
      const lines = await serial().command(
        `cat ${target} ${have} ${Math.min(CHUNK, size - have)}`,
        120000,
      );
      const error = lines.find((line) => line.startsWith("ERR "));
      if (error) {
        throw new Error(error.slice(4));
      }
      const before = have;
      for (const line of lines) {
        const match = line.match(/^DATA (\d+) (\S+)$/);
        if (!match || Number(match[1]) !== have) {
          continue;
        }
        const bytes = base64ToBytes(match[2]);
        parts.push(bytes);
        have += bytes.length;
      }
      if (have === before) {
        stalls++;
        if (stalls >= 3) {
          throw new Error(`no data at byte ${have}`);
        }
      } else {
        stalls = 0;
      }
      progress(have);
    }
    return new Blob(parts);
  }

  async function download(entry) {
    if (busy) {
      return;
    }
    setBusy(true);
    const target = join(path, entry.name);
    try {
      const blob = await readFile(target, entry.size, (have) =>
        say(
          `Downloading ${entry.name}: ${formatSize(have)} of ${formatSize(entry.size)}…`,
        ),
      );
      const link = document.createElement("a");
      link.href = URL.createObjectURL(blob);
      link.download = entry.name;
      link.click();
      setTimeout(() => URL.revokeObjectURL(link.href), 10000);
      say(`Downloaded ${entry.name} (${formatSize(blob.size)}).`);
    } catch (error) {
      say(`Download of ${entry.name} failed: ${error.message}`);
    } finally {
      setBusy(false);
    }
  }

  function confirmDelete(entry) {
    dialog.querySelector("p").textContent = entry.dir
      ? `Delete the directory ${join(path, entry.name)} and everything in it from the hub's SD card? This cannot be undone.`
      : `Delete ${join(path, entry.name)} from the hub's SD card? This cannot be undone.`;
    dialog.returnValue = "";
    return new Promise((resolve) => {
      dialog.addEventListener(
        "close",
        () => resolve(dialog.returnValue === "ok"),
        { once: true },
      );
      dialog.showModal();
    });
  }

  async function remove(entry) {
    if (busy || !(await confirmDelete(entry))) {
      return;
    }
    setBusy(true);
    const target = join(path, entry.name);
    let ok = false;
    try {
      const lines = await serial().command(
        `rm ${entry.dir ? "-r " : ""}${target}`,
        60000,
      );
      const error = lines.find((line) => line.startsWith("ERR "));
      say(error ? `Not deleted: ${error.slice(4)}` : `Deleted ${target}.`);
      ok = !error;
    } catch (error) {
      say(`No answer: ${error.message}`);
    } finally {
      setBusy(false);
    }
    if (ok) {
      await open(path);
      say(`Deleted ${target}. ${statusLine.textContent}`);
    }
  }

  async function runDiagnostic(command) {
    if (busy || !serial()?.isConnected()) {
      say("Connect to the hub first.");
      return;
    }
    setBusy(true);
    diagOut.textContent = `> ${command}\n`;
    try {
      if (command === "sdbench" && !(await hasCommand("sdbench"))) {
        diagOut.textContent +=
          "This firmware has no SD write test (built without CONFIG_HUB_CONSOLE_DIAGNOSTICS).";
        return;
      }
      const slow = command === "update" || command === "sdbench";
      const lines = await serial().command(command, slow ? 180000 : 15000);
      diagOut.textContent += lines.join("\n") || "(no output)";
    } catch (error) {
      diagOut.textContent += `${(error.lines ?? []).join("\n")}\n${error.message}`;
    } finally {
      setBusy(false);
    }
  }

  // Every `status` line as item and value (#156): the dock shows only the
  // main ones. "INFO camera C1 …" lines (#202) keep the camera in the item.
  function parseStatus(lines) {
    return lines
      .filter((line) => line.startsWith("INFO "))
      .map((line) => {
        const text = line.slice(5);
        const colon = text.indexOf(": ");
        if (colon > 0 && colon < 40) {
          return [text.slice(0, colon), text.slice(colon + 2)];
        }
        const words = text.split(" ");
        const keep = words[0] === "camera" && words.length > 2 ? 2 : 1;
        return [words.slice(0, keep).join(" "), words.slice(keep).join(" ")];
      });
  }

  async function readStatus() {
    if (busy || !serial()?.isConnected()) {
      say("Connect to the hub first.");
      return;
    }
    setBusy(true);
    try {
      const rows = parseStatus(await serial().command("status", 15000));
      statusRows.replaceChildren(
        ...rows.map(([item, value]) => {
          const row = document.createElement("tr");
          row.append(cell(item), cell(value));
          return row;
        }),
      );
      statusRows.closest("table").hidden = rows.length === 0;
      say(rows.length ? "" : "The hub gave no status lines.");
    } catch (error) {
      say(
        `No answer: ${error.message}. The hub may be asleep: press reset on the board.`,
      );
    } finally {
      setBusy(false);
    }
  }

  refresh.addEventListener("click", () => open(path));
  document
    .getElementById("sd-status-read")
    .addEventListener("click", readStatus);
  for (const button of diagButtons.filter((b) => b.dataset.sdCommand)) {
    button.addEventListener("click", () =>
      runDiagnostic(button.dataset.sdCommand),
    );
  }
  window.addEventListener("serial-connected", () => {
    available = null;
    helpText = null;
    setBusy(false);
  });
  window.addEventListener("serial-disconnected", () => setBusy(false));
  renderCrumbs();
  setBusy(false);
})();
