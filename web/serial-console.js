const connectButton = document.querySelector("#connect-serial");
const disconnectButton = document.querySelector("#disconnect-serial");
// Opens the picker although the port used last is known.
const otherPortButton = document.querySelector("#other-port");
const clearButton = document.querySelector("#clear-serial");
const copyButton = document.querySelector("#copy-serial");
const output = document.querySelector("#serial-output");
const status = document.querySelector("#serial-status");

let port;
let reader;
let reading = false;
let lineBuffer = "";
const encoder = new TextEncoder();

// --- Connecting again without the port picker ---
//
// The browser remembers which ports this page may use. The page remembers
// which of them was the hub (its USB ids) and whether a connection is
// wanted, and then opens the port on its own:
// - when the connection is lost (the hub went to sleep, was reset or
//   unplugged) and the port comes back,
// - when the page is loaded again while it was connected,
// - on "Connect", if the hub's port is there (otherwise the picker opens).
// "Disconnect" and the firmware installer end that: the port stays closed
// until the next "Connect".

// The user wants a connection; a lost one is taken up again.
let wanted = false;
// USB ids of the port used last (getInfo()), or null.
let lastInfo = null;
// An open() is running: the retry timer and a click must not start another.
let opening = false;
let retryTimer;
const PORT_KEY = "trailcamhub.serial.port";
const RETRY_MS = 1000;

function rememberPort(auto) {
  try {
    if (lastInfo) {
      window.localStorage.setItem(PORT_KEY, JSON.stringify({ usbVendorId: lastInfo.usbVendorId, usbProductId: lastInfo.usbProductId, auto }));
    }
  } catch {
    // Storage blocked: connecting again then needs the picker after a reload.
  }
}

function rememberedPort() {
  try {
    return JSON.parse(window.localStorage.getItem(PORT_KEY) ?? "null");
  } catch {
    return null;
  }
}

// "Other port…" only while not connected and a port is remembered, that is
// when "Connect" would not ask.
function showOtherPort() {
  otherPortButton.hidden = connectButton.disabled || !(lastInfo ?? rememberedPort());
}

// A port this page may use that is the same kind of device as `info`.
async function grantedPort(info) {
  if (!info) {
    return undefined;
  }
  const ports = await navigator.serial.getPorts();
  return ports.find((candidate) => {
    const other = candidate.getInfo();
    return other.usbVendorId === info.usbVendorId && other.usbProductId === info.usbProductId;
  });
}

// One attempt to take the connection up again; quiet if the port is not
// there (yet).
async function tryReopen() {
  if (!wanted || opening || (port && reading)) {
    return;
  }
  const match = await grantedPort(lastInfo);
  if (match && wanted && !opening && !(port && reading)) {
    await openPort(match, true);
  }
}

function waitForPort() {
  clearInterval(retryTimer);
  retryTimer = setInterval(() => void tryReopen().catch(() => {}), RETRY_MS);
  status.textContent = "Connection lost. Waiting for the hub to come back. If it sleeps, press reset on the board (or plug it in again).";
  connectButton.disabled = true;
  disconnectButton.disabled = false;
  disconnectButton.textContent = "Stop waiting";
  showOtherPort();
}

// How close to the end (in pixels) still counts as "at the newest line".
const FOLLOW_THRESHOLD = 24;
// The view keeps about this much text; older chunks are dropped.
const MAX_OUTPUT_CHARS = 500_000;
let outputChars = 0;
// appendOutput() keeps the position itself when old text is dropped; the
// browser's scroll anchoring (not in every browser) would correct it a
// second time.
output.style.overflowAnchor = "none";

// Each chunk is its own text node, so appending does not copy the whole log
// and the oldest text can be dropped chunk by chunk.
//
// New output is followed only while the view is at the end. Scrolled up (to
// read or search the log), the view stays where it is; scrolling back to the
// end follows again.
function appendOutput(text) {
  const following =
    output.scrollHeight - output.scrollTop - output.clientHeight <=
    FOLLOW_THRESHOLD;
  output.append(text);
  outputChars += text.length;
  if (outputChars > MAX_OUTPUT_CHARS) {
    // Dropping the oldest text moves everything up; keep the lines in view
    // where they are.
    // (Removing the text can clamp scrollTop, so it is taken first.)
    const top = output.scrollTop;
    const before = output.scrollHeight;
    while (outputChars > MAX_OUTPUT_CHARS * 0.8 && output.firstChild) {
      outputChars -= output.firstChild.textContent.length;
      output.firstChild.remove();
    }
    if (!following) {
      output.scrollTop = top - (before - output.scrollHeight);
    }
  }
  if (following) {
    output.scrollTop = output.scrollHeight;
  }
  updateOutputButtons();
}

// Clear and Copy only make sense while there is output to act on.
function updateOutputButtons() {
  const empty = outputChars === 0;
  clearButton.disabled = empty;
  copyButton.disabled = empty;
}

// Empties only the console view: the connection, the partial-line buffer and
// everything fed from "serial-line" events (device setup, protocol view) are
// left untouched.
function clearOutput() {
  output.textContent = "";
  outputChars = 0;
  updateOutputButtons();
}

let copyFeedbackTimer;

// Shows the copy result on the button itself, so the connection status next to
// it keeps its meaning.
function showCopyFeedback(label) {
  clearTimeout(copyFeedbackTimer);
  copyButton.textContent = label;
  copyFeedbackTimer = setTimeout(() => {
    copyButton.textContent = "Copy";
  }, 2000);
}

async function copyOutput() {
  try {
    if (!navigator.clipboard) {
      throw new Error("Clipboard is not available");
    }
    await navigator.clipboard.writeText(output.textContent);
    showCopyFeedback("Copied");
  } catch (error) {
    showCopyFeedback("Copy failed");
    status.textContent = `Could not copy the output: ${error.message}`;
  }
}

// Splits the incoming stream into complete lines and dispatches each as a
// "serial-line" event, so the live protocol view can decode it. The partial
// trailing line is held until its newline arrives.
function dispatchLines(text) {
  lineBuffer += text;
  let newline;
  while ((newline = lineBuffer.indexOf("\n")) !== -1) {
    const line = lineBuffer.slice(0, newline).replace(/\r$/, "");
    lineBuffer = lineBuffer.slice(newline + 1);
    window.dispatchEvent(new CustomEvent("serial-line", { detail: line }));
  }
}

// Resolves when the read loop has stopped and released its reader. Only the
// loop takes and releases the reader lock; disconnect() cancels the read and
// waits for this, then closes the port.
let readLoopDone = Promise.resolve();

// Closes the port. `keepWanted` is for a lost connection, which is taken up
// again; every other caller ends the connection for good.
async function disconnect(keepWanted = false) {
  if (!keepWanted) {
    wanted = false;
    clearInterval(retryTimer);
    rememberPort(false);
  }
  reading = false;
  const activeReader = reader;
  if (activeReader) {
    try {
      await activeReader.cancel();
    } catch {
      // The stream is already errored or closed; the loop ends either way.
    }
  }
  await readLoopDone;
  if (port) {
    const closing = port;
    port = undefined;
    await closing.close();
  }
  connectButton.disabled = !("serial" in navigator);
  disconnectButton.disabled = true;
  disconnectButton.textContent = "Disconnect";
  showOtherPort();
  status.textContent = "Not connected";
  window.dispatchEvent(new CustomEvent("serial-disconnected"));
}

async function readLoop() {
  const decoder = new TextDecoder();
  while (port && port.readable && reading) {
    reader = port.readable.getReader();
    try {
      while (reading) {
        const { value, done } = await reader.read();
        if (done) {
          break;
        }
        const chunk = decoder.decode(value, { stream: true });
        appendOutput(chunk);
        dispatchLines(chunk);
      }
    } finally {
      reader.releaseLock();
      reader = undefined;
    }
  }
}

// Text for a port that cannot be opened: usually the firmware installer (or
// another tab or program) still holds it.
function openErrorText(error) {
  if (error.name === "InvalidStateError" || error.name === "NetworkError") {
    return "The serial port is in use. Close the installer dialog (or the other program or tab using the port) and connect again.";
  }
  return `Serial connection failed: ${error.message}`;
}

// Opens a port the user picked (or allowed before) and reads it in the
// background; returns false if it could not be opened. `quiet` (a retry in
// the background) leaves the status text alone on a failure.
async function openPort(selected, quiet = false) {
  if (opening) {
    return false;
  }
  opening = true;
  try {
    await selected.open({ baudRate: 115200 });
  } catch (error) {
    if (!quiet) {
      status.textContent = openErrorText(error);
    }
    return false;
  } finally {
    opening = false;
  }
  port = selected;
  reading = true;
  wanted = true;
  lastInfo = selected.getInfo();
  rememberPort(true);
  clearInterval(retryTimer);
  disconnectButton.textContent = "Disconnect";
  lineBuffer = "";
  window.dispatchEvent(new CustomEvent("serial-reset"));
  connectButton.disabled = true;
  disconnectButton.disabled = false;
  showOtherPort();
  status.textContent = "Connected at 115200 baud";
  window.dispatchEvent(new CustomEvent("serial-connected"));
  void watchPort();
  return true;
}

async function watchPort() {
  let failure;
  readLoopDone = readLoop().catch((error) => {
    failure = error;
  });
  await readLoopDone;
  if (!reading) {
    // disconnect() stopped the loop and closes the port.
    return;
  }
  // The device went away (unplugged, reset over USB, asleep) or the read
  // failed: close the port and wait for it to come back.
  try {
    await disconnect(true);
  } catch (cleanupError) {
    wanted = false;
    status.textContent = `Serial connection lost${failure ? `: ${failure.message}` : ""}. Could not close the port: ${cleanupError.message}`;
    return;
  }
  waitForPort();
}

// Connects to the hub: to the port used last if it is there, otherwise (or
// with `choose`) to the one the user picks. Call it from a click, the picker
// needs one.
async function connect(choose = false) {
  if (port && reading) {
    return true;
  }
  if (!choose) {
    const known = await grantedPort(lastInfo ?? rememberedPort());
    if (known && (await openPort(known, true))) {
      return true;
    }
  }
  let selected;
  try {
    selected = await navigator.serial.requestPort();
  } catch (error) {
    if (error.name !== "NotFoundError") {
      status.textContent = `Serial connection failed: ${error.message}`;
    }
    return false;
  }
  return openPort(selected);
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// Waits until the connection is back after a reset: the USB-Serial-JTAG of
// the ESP32-S3/C5 goes away and comes back as a new device, which the retry
// above opens. `info` (from reset()) is the port to wait for, in case the
// loss was not noticed yet. Returns false after `timeoutMs`.
async function reconnect(info, timeoutMs = 15000) {
  const deadline = Date.now() + timeoutMs;
  if (!wanted) {
    // Not lost but closed (or never open): take it up here.
    wanted = true;
    lastInfo = info ?? lastInfo;
  }
  while (Date.now() < deadline) {
    if (port && reading) {
      return true;
    }
    await tryReopen().catch(() => {});
    if (!(port && reading)) {
      await sleep(250);
    }
  }
  return Boolean(port && reading);
}

// Resets the board over the serial lines like esptool's hard reset (RTS
// pulls EN low while DTR stays high). Works with the USB-Serial-JTAG of the
// S3/C5 and with USB-UART bridges that have the auto-reset circuit. Returns
// the port's USB ids for reconnect().
async function reset() {
  if (!port) {
    throw new Error("Serial port is not connected");
  }
  const info = port.getInfo();
  await port.setSignals({ dataTerminalReady: false, requestToSend: true });
  await sleep(120);
  await port.setSignals({ dataTerminalReady: false, requestToSend: false });
  return info;
}

// Commands with their answers: the hub ends each answer with "TCH DONE".
// One at a time, so that answers do not mix.
let commandChain = Promise.resolve();

function command(line, timeoutMs = 10000) {
  const run = () =>
    new Promise((resolve, reject) => {
      const lines = [];
      let timer;
      const onLine = (event) => {
        const text = event.detail;
        if (!text.startsWith("TCH ")) {
          return;
        }
        if (text === "TCH DONE" || text.startsWith("TCH DONE ")) {
          finish();
          resolve(lines);
          return;
        }
        lines.push(text.slice(4));
      };
      const finish = () => {
        clearTimeout(timer);
        window.removeEventListener("serial-line", onLine);
      };
      window.addEventListener("serial-line", onLine);
      timer = setTimeout(() => {
        finish();
        const error = new Error(`No answer to '${line.split(" ")[0]}'`);
        error.lines = lines;
        reject(error);
      }, timeoutMs);
      sendLine(line).catch((error) => {
        finish();
        reject(error);
      });
    });
  const result = commandChain.then(run, run);
  commandChain = result.catch(() => {});
  return result;
}

// The text in the console view (for the support package).
function outputText() {
  return output.textContent;
}

async function handleDisconnect() {
  try {
    await disconnect();
  } catch (error) {
    status.textContent = `Could not disconnect from serial port: ${error.message}`;
  }
}

// The firmware installer (ESP Web Tools) opens the port on its own. A click on
// an install button while this console holds the port would make it fail, so
// the console lets go of the port first and then passes the click on; the
// browser still counts it as the user's gesture for the port picker.
let passingInstallClick = false;
for (const installButton of document.querySelectorAll(
  "esp-web-install-button",
)) {
  installButton.addEventListener(
    "click",
    async (event) => {
      if (passingInstallClick || !(port && reading)) {
        return;
      }
      event.preventDefault();
      event.stopImmediatePropagation();
      try {
        await disconnect();
      } catch (error) {
        status.textContent = `Could not release the serial port for the installer: ${error.message}`;
        return;
      }
      status.textContent =
        "Disconnected for the installer. Connect again after the installation.";
      passingInstallClick = true;
      try {
        event.target.click();
      } finally {
        passingInstallClick = false;
      }
    },
    { capture: true },
  );
}

if (!("serial" in navigator)) {
  connectButton.disabled = true;
  status.textContent = "Web Serial is not supported by this browser.";
}

// When the last line was sent to the hub (Date.now()).
let lastSentAt = 0;

// Sends one command line to the device. One line at a time: the stream has
// a single writer, and a second getWriter() while the first is held throws
// (the dock's "status" and a tab's command right after connecting).
let writeChain = Promise.resolve();

function sendLine(line) {
  const write = async () => {
    if (!port || !port.writable) {
      throw new Error("Serial port is not connected");
    }
    const writer = port.writable.getWriter();
    try {
      await writer.write(encoder.encode(`${line}\n`));
      lastSentAt = Date.now();
    } finally {
      writer.releaseLock();
    }
  };
  const result = writeChain.then(write, write);
  writeChain = result.catch(() => {});
  return result;
}

// Every command keeps a hub on battery awake for a few minutes
// (CONFIG_POWER_AWAKE_AFTER_ACTIVITY_MIN, 5 by default), so while this page
// is connected it sends "ping" after two quiet minutes. Firmware without
// "ping" answers with an error, which keeps it awake just the same.
const KEEP_AWAKE_MS = 2 * 60 * 1000;
// Off while the camera mock is shown: it has no "ping" and no battery.
let keepAwake = true;
setInterval(() => {
  if (keepAwake && port && reading && Date.now() - lastSentAt >= KEEP_AWAKE_MS) {
    command("ping", 5000).catch(() => {});
  }
}, 30 * 1000);

window.trailcamSerial = {
  connect,
  reconnect,
  reset,
  command,
  sendLine,
  disconnect: handleDisconnect,
  setKeepAwake: (on) => {
    keepAwake = Boolean(on);
  },
  outputText,
  isConnected: () => Boolean(port && reading),
};

connectButton.addEventListener("click", () => connect());
otherPortButton.addEventListener("click", () => connect(true));
showOtherPort();

if ("serial" in navigator) {
  // A port this page may use was plugged in (or woke up).
  navigator.serial.addEventListener("connect", () => void tryReopen().catch(() => {}));
  // The page was connected when it was closed or reloaded: connect again,
  // once every script listens for the connection.
  window.addEventListener("DOMContentLoaded", async () => {
    const stored = rememberedPort();
    if (!stored?.auto) {
      return;
    }
    const known = await grantedPort(stored).catch(() => undefined);
    if (known && !(port && reading)) {
      await openPort(known, true);
    }
  });
}
disconnectButton.addEventListener("click", handleDisconnect);
clearButton.addEventListener("click", clearOutput);
copyButton.addEventListener("click", copyOutput);
