// "Camera" tab of the advanced view: take a photo or a video, list the
// recordings on the camera, delete one, erase the camera's SD card, and the
// queue of these actions with their results. Everything goes through the
// console commands of docs/camera-actions.md (photo, video start|stop,
// camlist, delete, format, actions); the hub runs them in a camera session.
(function () {
  "use strict";

  const panel = document.getElementById("adv-panel-camera");
  if (!panel) {
    return;
  }
  const serial = () => window.trailcamSerial;
  const $ = (selector) => panel.querySelector(selector);

  const status = $("#cam-status");
  const listButton = $("#cam-list");
  const listCaption = $("#cam-list-caption");
  const tableWrap = $("#cam-media");
  const tableBody = $("#cam-media-body");
  const actionsList = $("#cam-actions");
  const dialog = document.getElementById("cam-confirm");

  // "fileType" of the media list (media_list.hpp): 0 and 2 were observed.
  const TYPE_TEXT = { 0: "Photo", 2: "Video" };

  // mediaId → { type, dir, num, id, time } of the last listing.
  let media = new Map();
  let listing = false;

  function say(text, bad = false) {
    status.textContent = text;
    status.classList.toggle("wz-problem", bad);
  }

  function setConnected(on) {
    // Not the dialog's own buttons.
    for (const button of panel.querySelectorAll("section button")) {
      button.disabled = !on;
    }
    if (!on) {
      listing = false;
      say("Connect to the hub to use the camera.");
    } else {
      say("");
      // Not while the camera mock is shown: it has no camera actions.
      if (document.body.dataset.view !== "mock") {
        void refreshActions();
      }
    }
  }

  // Sends one command; the answer's "OK" text, or an Error with the hub's
  // reason ("busy (SESSION)", "no camera known", …).
  async function run(line) {
    const answer = await serial().command(line, 10000);
    const error = answer.find((text) => text.startsWith("ERR "));
    if (error) {
      throw new Error(error.slice(4));
    }
    return (answer.find((text) => text.startsWith("OK ")) ?? "OK ").slice(3);
  }

  // --- Photo and video ---

  for (const button of panel.querySelectorAll("[data-action]")) {
    button.addEventListener("click", async () => {
      say("Sending…");
      try {
        say(await run(button.dataset.action));
      } catch (error) {
        say(`${button.textContent}: ${error.message}`, true);
      }
      void refreshActions();
    });
  }

  // --- Recordings on the camera ---

  // "type=0 dir=100 num=36 id=4136037234 time=2025-12-23T16:01:32"
  function parseMedia(text) {
    const fields = Object.fromEntries(text.split(" ").map((part) => part.split("=")));
    if (!fields.id) {
      return null;
    }
    return { type: Number(fields.type), dir: fields.dir, num: fields.num, id: fields.id, time: fields.time ?? "" };
  }

  function describe(item) {
    // The time is the camera's wall clock (docs/clock.md): shown as it is.
    return `${(TYPE_TEXT[item.type] ?? `type ${item.type}`).toLowerCase()} of ${item.time.replace("T", " ")}`;
  }

  function renderMedia() {
    // Newest first; the camera lists them in its own order.
    const items = [...media.values()].sort((a, b) => b.time.localeCompare(a.time) || Number(b.num) - Number(a.num));
    tableBody.replaceChildren();
    for (const item of items) {
      const row = document.createElement("tr");
      const cell = (text) => {
        const td = document.createElement("td");
        td.textContent = text;
        row.append(td);
        return td;
      };
      cell(TYPE_TEXT[item.type] ?? `type ${item.type}`);
      cell(item.time.replace("T", " "));
      cell(`${item.dir}/${item.num}`);
      cell(item.id);
      const button = document.createElement("button");
      button.type = "button";
      button.className = "secondary-button danger";
      button.textContent = "Delete";
      button.disabled = !serial()?.isConnected();
      button.addEventListener("click", () => void deleteRecording(item));
      cell("").append(button);
      tableBody.append(row);
    }
    tableWrap.hidden = items.length === 0;
  }

  function countText() {
    const items = [...media.values()];
    const photos = items.filter((item) => item.type === 0).length;
    const videos = items.filter((item) => item.type === 2).length;
    return `${items.length} recording${items.length === 1 ? "" : "s"} (${photos} photo${photos === 1 ? "" : "s"}, ${videos} video${videos === 1 ? "" : "s"})`;
  }

  listButton.addEventListener("click", async () => {
    say("");
    try {
      await run("camlist");
    } catch (error) {
      listCaption.textContent = `Could not list: ${error.message}`;
      return;
    }
    listing = true;
    media = new Map();
    renderMedia();
    listCaption.textContent = "Waking the camera and reading its list… (up to a few minutes)";
  });

  function handleLine(line) {
    if (!line.startsWith("TCH EVENT ")) {
      return;
    }
    const text = line.slice("TCH EVENT ".length);
    if (text.startsWith("media ")) {
      const item = parseMedia(text.slice(6));
      if (item) {
        media.set(item.id, item);
        if (listing) {
          listCaption.textContent = `Reading the list… ${media.size} so far`;
        }
      }
    } else if (text.startsWith("list done") || text.startsWith("list truncated")) {
      listing = false;
      renderMedia();
      listCaption.textContent =
        `${countText()}, read at ${new Date().toLocaleTimeString()}` +
        (text.startsWith("list truncated") ? ". The list is longer; the hub stopped at its page limit." : media.size === 0 ? ". The camera's card is empty." : "");
    } else if (text.startsWith("list failed")) {
      listing = false;
      renderMedia();
      listCaption.textContent = `The camera did not give its list: ${text.slice("list failed ".length)}`;
    } else if (text.startsWith("done ")) {
      // A camera session ended: queued actions have a result now.
      if (listing) {
        listing = false;
        renderMedia();
        listCaption.textContent = text.includes("unreachable")
          ? "The camera was not reached: is it switched on and nearby?"
          : `The session ended without a list (${text.slice(5)}).`;
      }
      void refreshActions();
    }
  }

  // --- Delete and format: asked twice, here and by the hub's code ---

  // Shows the dialog; resolves true if confirmed. With `word`, the button
  // stays disabled until that word is typed.
  function confirmDialog(title, text, buttonText, word) {
    dialog.querySelector("h3").textContent = title;
    dialog.querySelector("p").textContent = text;
    const input = dialog.querySelector("input");
    const label = dialog.querySelector("label");
    const ok = dialog.querySelector("[value=ok]");
    ok.textContent = buttonText;
    label.hidden = !word;
    input.value = "";
    ok.disabled = Boolean(word);
    input.oninput = () => {
      ok.disabled = input.value.trim().toLowerCase() !== word;
    };
    dialog.returnValue = "";
    dialog.showModal();
    return new Promise((resolve) => {
      dialog.addEventListener("close", () => resolve(dialog.returnValue === "ok"), { once: true });
    });
  }

  // The hub answers the first request with a code that the second one must
  // repeat within a minute (ConfirmGate).
  async function confirmed(command) {
    const first = await run(command);
    const code = first.match(/ confirm (\d+)'/);
    if (!code) {
      throw new Error(`no confirmation code in the hub's answer (${first})`);
    }
    return run(`${command} confirm ${code[1]}`);
  }

  async function deleteRecording(item) {
    const yes = await confirmDialog(
      "Delete this recording?",
      `The ${describe(item)} (${item.dir}/${item.num}, mediaId ${item.id}) is deleted from the camera's SD card. Copies on the hub and at your destination stay.`,
      "Delete recording",
    );
    if (!yes) {
      return;
    }
    try {
      say(await confirmed(`delete ${item.id}`));
    } catch (error) {
      say(`Delete: ${error.message}`, true);
    }
    void refreshActions();
  }

  $("#cam-format").addEventListener("click", async () => {
    const yes = await confirmDialog(
      "Erase the camera's SD card?",
      "Every recording on the camera is deleted. This cannot be undone. Recordings the hub has not read yet are lost.",
      "Erase the card",
      "erase",
    );
    if (!yes) {
      return;
    }
    try {
      say(await confirmed("format"));
    } catch (error) {
      say(`Erase: ${error.message}`, true);
    }
    void refreshActions();
  });

  // --- The queue ---

  // "action 3 delete 2653775860: failed: the camera does not list this file any more"
  function parseAction(text) {
    const match = text.match(/^action (\d+) ([a-z-]+)(?: (\d+))?: (\w+)(?:: (.*))?$/);
    return match ? { id: match[1], type: match[2], mediaId: match[3], state: match[4], reason: match[5] ?? "" } : null;
  }

  let refreshing = false;
  async function refreshActions() {
    if (refreshing || !serial()?.isConnected()) {
      return;
    }
    refreshing = true;
    try {
      const answer = await serial().command("actions", 10000);
      const error = answer.find((text) => text.startsWith("ERR "));
      const actions = answer
        .filter((text) => text.startsWith("INFO action "))
        .map((text) => parseAction(text.slice(5)))
        .filter(Boolean);
      actionsList.replaceChildren();
      let removed = false;
      // Newest first.
      for (const action of actions.reverse()) {
        const item = document.createElement("li");
        item.className = `cam-${action.state}`;
        const state = document.createElement("span");
        state.className = "cam-state";
        state.textContent = action.state;
        const target = action.mediaId ? ` ${action.mediaId}` : "";
        item.append(state, `${action.type}${target}${action.reason ? ` — ${action.reason}` : ""}`);
        actionsList.append(item);
        if (action.type === "delete" && action.state === "done" && media.delete(action.mediaId)) {
          removed = true;
        }
      }
      if (actions.length === 0) {
        const item = document.createElement("li");
        item.textContent = error ? `Not available: ${error.slice(4)} (older firmware?)` : "No actions yet.";
        actionsList.append(item);
      }
      if (removed) {
        renderMedia();
        listCaption.textContent = `${countText()} after the deletion`;
      }
    } catch {
      // No answer (busy console, older firmware): the list stays as it is.
    } finally {
      refreshing = false;
    }
  }

  $("#cam-actions-refresh").addEventListener("click", () => void refreshActions());

  window.addEventListener("serial-line", (event) => handleLine(event.detail));
  window.addEventListener("serial-connected", () => setConnected(true));
  window.addEventListener("serial-disconnected", () => setConnected(false));
  setConnected(false);
})();
