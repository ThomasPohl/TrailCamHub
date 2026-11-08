// "Advanced" tools: reset, restart, a command line and the support package.
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
    status.textContent = "Connect first (Device setup or the serial console).";
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

  document.getElementById("adv-command").addEventListener("submit", async (event) => {
    event.preventDefault();
    const line = event.target.elements.line.value.trim();
    if (!line || !connected()) {
      return;
    }
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

  // The last `maxBytes` of the newest log file on the SD card, if the build
  // has the diagnostic commands (ls, cat).
  async function newestLog(maxBytes) {
    const listing = await serial().command("ls logs", 10000);
    if (listing.some((line) => line.startsWith("ERR "))) {
      return { note: listing.join("\n") };
    }
    const files = listing
      .map((line) => line.match(/^INFO f (\d+) (\d+) (\S+)$/))
      .filter(Boolean)
      .map((m) => ({ size: Number(m[1]), time: Number(m[2]), name: m[3] }))
      .sort((a, b) => b.time - a.time || b.name.localeCompare(a.name));
    if (files.length === 0) {
      return { note: "no log files on the SD card" };
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
        const log = await newestLog(64 * 1024);
        add(log.name ? `SD card log ${log.name} from byte ${log.offset}` : "SD card log", log.text ?? log.note);
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
