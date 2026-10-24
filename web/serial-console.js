const connectButton = document.querySelector("#connect-serial");
const disconnectButton = document.querySelector("#disconnect-serial");
const output = document.querySelector("#serial-output");
const status = document.querySelector("#serial-status");

let port;
let reader;
let reading = false;

function appendOutput(text) {
  output.textContent += text;
  if (output.textContent.length > 100_000) {
    output.textContent = output.textContent.slice(-80_000);
  }
  output.scrollTop = output.scrollHeight;
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
}

async function connect() {
  try {
    port = await navigator.serial.requestPort();
    await port.open({ baudRate: 115200 });
    reading = true;
    connectButton.disabled = true;
    disconnectButton.disabled = false;
    status.textContent = "Connected at 115200 baud";

    const decoder = new TextDecoder();
    while (port.readable && reading) {
      reader = port.readable.getReader();
      try {
        while (reading) {
          const { value, done } = await reader.read();
          if (done) {
            break;
          }
          appendOutput(decoder.decode(value, { stream: true }));
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

connectButton.addEventListener("click", connect);
disconnectButton.addEventListener("click", handleDisconnect);
