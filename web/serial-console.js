const connectButton = document.querySelector("#connect-serial");
const disconnectButton = document.querySelector("#disconnect-serial");
const clearButton = document.querySelector("#clear-serial");
const copyButton = document.querySelector("#copy-serial");
const output = document.querySelector("#serial-output");
const status = document.querySelector("#serial-status");

let port;
let reader;
let reading = false;
let lineBuffer = "";
const encoder = new TextEncoder();

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

async function disconnect() {
  reading = false;
  if (reader) {
    await reader.cancel();
    reader.releaseLock();
    reader = undefined;
  }
  if (port) {
    await port.close();
    port = undefined;
  }
  connectButton.disabled = !("serial" in navigator);
  disconnectButton.disabled = true;
  status.textContent = "Not connected";
  window.dispatchEvent(new CustomEvent("serial-disconnected"));
}

async function connect() {
  try {
    port = await navigator.serial.requestPort();
    await port.open({ baudRate: 115200 });
    reading = true;
    lineBuffer = "";
    window.dispatchEvent(new CustomEvent("serial-reset"));
    connectButton.disabled = true;
    disconnectButton.disabled = false;
    status.textContent = "Connected at 115200 baud";
    window.dispatchEvent(new CustomEvent("serial-connected"));

    const decoder = new TextDecoder();
    while (port.readable && reading) {
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
    if (reading) {
      await disconnect();
    }
  } catch (error) {
    if (reading) {
      try {
        await disconnect();
      } catch (cleanupError) {
        status.textContent = `Serial connection failed: ${error.message}. Could not close the port: ${cleanupError.message}`;
        return;
      }
      status.textContent = `Serial connection failed: ${error.message}`;
    } else if (error.name !== "NotFoundError") {
      status.textContent = `Serial connection failed: ${error.message}`;
    }
  }
}

async function handleDisconnect() {
  try {
    await disconnect();
  } catch (error) {
    status.textContent = `Could not disconnect from serial port: ${error.message}`;
  }
}

if (!("serial" in navigator)) {
  connectButton.disabled = true;
  status.textContent = "Web Serial is not supported by this browser.";
}

// Sends one command line to the device (used by the device setup panel).
async function sendLine(line) {
  if (!port || !port.writable) {
    throw new Error("Serial port is not connected");
  }
  const writer = port.writable.getWriter();
  try {
    await writer.write(encoder.encode(`${line}\n`));
  } finally {
    writer.releaseLock();
  }
}

window.trailcamSerial = {
  connect,
  sendLine,
  isConnected: () => Boolean(port && reading),
};

connectButton.addEventListener("click", connect);
disconnectButton.addEventListener("click", handleDisconnect);
clearButton.addEventListener("click", clearOutput);
copyButton.addEventListener("click", copyOutput);
