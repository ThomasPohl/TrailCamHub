// "Advanced" tools in the dock: reset, restart, a command line (with a
// history on the arrow keys) and the support package.
(function () {
  "use strict";

  const serial = () => window.trailcamSerial;
  const status = document.getElementById("adv-status");
  const answer = document.getElementById("adv-answer");
  if (!status) {
    return;
  }

  function connected() {
    if (serial()?.isConnected()) {
      return true;
    }
    status.textContent = "Connect to the hub first.";
    return false;
  }

  document.getElementById("adv-reset").addEventListener("click", async () => {
    if (!connected()) {
      return;
    }
    try {
      const info = await serial().reset();
      status.textContent = "Reset sent; reconnecting…";
      await new Promise((resolve) => setTimeout(resolve, 1500));
      const back = serial().isConnected() || (await serial().reconnect(info));
      status.textContent = back ? "The hub restarted (awake for ten minutes on battery)." : "The hub restarted; connect again.";
    } catch (error) {
      status.textContent = `Could not reset: ${error.message}. Press the board's reset button.`;
    }
  });

  document.getElementById("adv-restart").addEventListener("click", async () => {
    if (!connected()) {
      return;
    }
    try {
      await serial().command("restart", 5000);
      status.textContent = "Restarting; connect again if the connection drops.";
    } catch (error) {
      status.textContent = `No answer: ${error.message} (older firmware has no 'restart').`;
    }
  });

  const commandForm = document.getElementById("adv-command");
  const commandLine = commandForm.elements.line;
  // Buttons that need the hub; the support package also works without it
  // (it then holds only this page's log).
  const needHub = [document.getElementById("adv-reset"), document.getElementById("adv-restart"), commandForm.querySelector("button")];

  function setConnected(on) {
    for (const button of needHub) {
      button.disabled = !on;
    }
    if (on) {
      status.textContent = "";
    }
  }
  window.addEventListener("serial-connected", () => setConnected(true));
  window.addEventListener("serial-disconnected", () => setConnected(false));
  setConnected(false);

  // Commands sent from the command line, oldest first; arrow up/down walk it.
  const history = [];
  let historyAt = 0;
  commandLine.addEventListener("keydown", (event) => {
    if (event.key !== "ArrowUp" && event.key !== "ArrowDown") {
      return;
    }
    event.preventDefault();
    historyAt = Math.max(0, Math.min(history.length, historyAt + (event.key === "ArrowUp" ? -1 : 1)));
    commandLine.value = history[historyAt] ?? "";
  });

  commandForm.addEventListener("submit", async (event) => {
    event.preventDefault();
    const line = commandLine.value.trim();
    if (!line || !connected()) {
      return;
    }
    if (history[history.length - 1] !== line) {
      history.push(line);
    }
    historyAt = history.length;
    commandLine.value = "";
    answer.textContent = `> ${line}\n`;
    try {
      const lines = await serial().command(line, 60000);
      answer.textContent += lines.join("\n") || "(no output)";
    } catch (error) {
      answer.textContent += `${(error.lines ?? []).join("\n")}\n${error.message}`;
    }
  });

  // --- Support package ---

  function base64ToBytes(text) {
    const binary = atob(text);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) {
      bytes[i] = binary.charCodeAt(i);
    }
    return bytes;
  }

  // The last `maxBytes` of the newest file in logs/ on the SD card whose
  // name matches `pattern`, if the build has the diagnostic commands (ls,
  // cat). The directory holds the log files (log-NNNNN.txt) and the cycle
  // journal (cycles.jsonl).
  async function newestLog(pattern, maxBytes) {
    const listing = await serial().command("ls logs", 10000);
    if (listing.some((line) => line.startsWith("ERR "))) {
      return { note: listing.join("\n") };
    }
    const files = listing
      .map((line) => line.match(/^INFO f (\d+) (\d+) (\S+)$/))
      .filter(Boolean)
      .map((m) => ({ size: Number(m[1]), time: Number(m[2]), name: m[3] }))
      .filter((file) => pattern.test(file.name))
      .sort((a, b) => b.time - a.time || b.name.localeCompare(a.name));
    if (files.length === 0) {
      return { note: "no such file on the SD card" };
    }
    const file = files[0];
    const offset = Math.max(0, file.size - maxBytes);
    const lines = await serial().command(`cat logs/${file.name} ${offset}`, 120000);
    const parts = [];
    for (const line of lines) {
      const match = line.match(/^DATA \d+ (\S+)$/);
      if (match) {
        parts.push(base64ToBytes(match[1]));
      }
    }
    const text = new TextDecoder().decode(await new Blob(parts).arrayBuffer());
    return { name: file.name, offset, text };
  }

  document.getElementById("adv-support").addEventListener("click", async () => {
    const sections = [];
    const add = (title, text) => sections.push(`===== ${title} =====\n${text}\n`);
    add("Installer", `${new Date().toISOString()}\n${navigator.userAgent}\n${location.href}`);
    if (serial()?.isConnected()) {
      status.textContent = "Collecting…";
      for (const command of ["status", "config show", "help"]) {
        try {
          add(command, (await serial().command(command, 10000)).join("\n"));
        } catch (error) {
          add(command, error.message);
        }
      }
      try {
        status.textContent = "Reading the newest log file…";
        const log = await newestLog(/^log-\d+\.txt$/, 64 * 1024);
        add(log.name ? `SD card log ${log.name} from byte ${log.offset}` : "SD card log", log.text ?? log.note);
        // One line per cycle: result, duration, lowest free heap and stack.
        const cycles = await newestLog(/^cycles\.jsonl$/, 16 * 1024);
        add(cycles.name ? `Cycle journal ${cycles.name} from byte ${cycles.offset}` : "Cycle journal", cycles.text ?? cycles.note);
      } catch (error) {
        add("SD card log", `not available: ${error.message}`);
      }
    } else {
      add("Hub", "not connected");
    }
    const console = serial()?.outputText() ?? "";
    add("Serial log of this page (last 200 KB)", console.slice(-200 * 1024));
    const blob = new Blob([sections.join("\n")], { type: "text/plain" });
    const link = document.createElement("a");
    link.href = URL.createObjectURL(blob);
    link.download = `trailcamhub-support-${new Date().toISOString().replace(/[:.]/g, "-")}.txt`;
    link.click();
    setTimeout(() => URL.revokeObjectURL(link.href), 10000);
    status.textContent = "Support package downloaded. Check it before you share it.";
  });
})();
