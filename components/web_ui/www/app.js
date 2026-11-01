"use strict";

const $ = (id) => document.getElementById(id);

const PHASES = {
  idle: ["READY", "idle", "Press “Search camera” to look for the camera."],
  searching: ["SEARCHING", "busy", "Looking for the camera via Bluetooth."],
  connecting: ["CONNECTING", "busy", "Joining the camera Wi-Fi."],
  connected: ["CONNECTED", "ok", "Session running."],
  retry_wait: ["NO LINK", "warn", ""],
  uploading: ["UPLOADING", "busy", "Syncing with the backend over the home Wi-Fi."],
  finished: ["FINISHED", "ok", ""],
};

// Path of the log file being written; it cannot be deleted.
let activeLog = "";

function size(bytes) {
  if (bytes < 1024) return bytes + " B";
  const units = ["KB", "MB", "GB"];
  let value = bytes / 1024;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  return value.toFixed(value < 10 ? 1 : 0) + " " + units[unit];
}

function duration(seconds) {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor(seconds / 60) % 60;
  return h ? `${h} h ${m} min` : `${m} min ${seconds % 60} s`;
}

function fileUrl(path, download) {
  return "/files/" + path.split("/").map(encodeURIComponent).join("/") +
    (download ? "?download=1" : "");
}

function ext(name) {
  const match = name.match(/\.([^.]+)$/);
  return match ? match[1].toUpperCase().slice(0, 4) : "FILE";
}

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

// Files the firmware is still writing; the server refuses to delete them.
function deletable(path) {
  return path !== activeLog && !/\.(part|tmp)$/i.test(path);
}

// --- Status -----------------------------------------------------------------

async function post(url) {
  try {
    await fetch(url, { method: "POST" });
  } catch (e) {
    // Shown by the next status poll.
  }
  refreshStatus();
}

async function refreshStatus() {
  try {
    const s = await (await fetch("/api/status")).json();
    $("offline").hidden = true;
    const [label, tone, text] = PHASES[s.phase] || [s.phase, "idle", ""];
    $("phase").textContent = label;
    $("phase").className = "pill " + tone;
    const total = s.media_total;
    const done = Math.min(s.media_done, total);
    const stored = Math.min(s.media_stored, total);
    if (s.phase === "retry_wait") {
      $("detail").textContent = `Next attempt in ${Math.ceil(s.retry_ms / 1000)} s.`;
    } else if (s.phase === "finished") {
      $("detail").textContent = total
        ? `Saved ${stored} of ${total} files. Search again for new media.`
        : "Camera synced. Search again for new media.";
    } else {
      $("detail").textContent = text;
    }
    $("camera").textContent = s.camera || "–";
    $("media").textContent = total ? `${stored} of ${total} saved` : "–";
    $("progress").hidden = !total || s.phase === "finished";
    $("progress").max = total || 1;
    $("progress").value = done;
    $("sd").textContent = s.sd_total_mb
      ? `${size(s.sd_free_mb * 1048576)} of ${size(s.sd_total_mb * 1048576)}`
      : "no card";
    $("uptime").textContent = duration(s.uptime_s);
    showSync(s);
    syncClock(s);
    $("ap").textContent = s.ap;
    activeLog = s.log;
    const busy = ["searching", "connecting", "connected", "uploading"].includes(
      s.phase,
    );
    $("search").disabled = busy;
    $("stop").disabled = s.phase === "idle";
  } catch (e) {
    $("offline").hidden = false;
  }
}

// --- Last sync --------------------------------------------------------------

// directory of a camera on the card -> time of its last sync
let cameraSync = {};

function ago(seconds) {
  if (seconds < 90) return "just now";
  if (seconds < 5400) return Math.round(seconds / 60) + " min ago";
  if (seconds < 172800) return Math.round(seconds / 3600) + " h ago";
  return Math.round(seconds / 86400) + " days ago";
}

// t: Unix seconds; -1: synced but the time was not recorded; 0: never.
function syncText(t) {
  if (t > 0) {
    return new Date(t * 1000).toLocaleString() +
      " (" + ago(Date.now() / 1000 - t) + ")";
  }
  return t < 0 ? "done, time not recorded" : "never";
}

function showSync(s) {
  const before = JSON.stringify(cameraSync);
  cameraSync = {};
  for (const camera of s.cameras || []) cameraSync[camera.dir] = camera.last_sync;
  // The camera directories in the root of the card show their last sync.
  if (currentPath === "" && JSON.stringify(cameraSync) !== before) renderList();
  const cameras = s.cameras || [];
  // The most recent sync of any camera; with several cameras, say which.
  let latest = null;
  for (const camera of cameras) {
    if (!latest || camera.last_sync > latest.last_sync) latest = camera;
  }
  $("lastsync").textContent = latest ? syncText(latest.last_sync) : "–";
  $("lastsync").title = latest && cameras.length > 1 ? latest.id : "";
  $("clocknote").hidden = s.clock_source !== "none";
}

// --- Clock ------------------------------------------------------------------

const CLOCK_SOURCES = {
  sntp: "network time",
  backend: "backend",
  browser: "this browser",
  camera: "camera",
  none: "not set",
};

let timeSent = 0;

// The dongle has no clock of its own. If it got none from SNTP or the backend,
// this page tells it the time of this device (every half hour while open),
// and the dongle passes it on to the camera at the next login.
function syncClock(s) {
  const source = s.clock_source;
  $("clock").textContent = s.time
    ? `${new Date(s.time * 1000).toLocaleString()} (${CLOCK_SOURCES[source] || source})`
    : CLOCK_SOURCES[source] || source;
  if (source === "sntp" || source === "backend") return;
  const now = Date.now();
  if (now - timeSent < 30 * 60 * 1000) return;
  timeSent = now;
  fetch("/api/time?unix=" + Math.floor(now / 1000), { method: "POST" }).catch(
    () => {
      timeSent = 0;  // try again with the next status
    },
  );
}

// --- Delete -----------------------------------------------------------------

// Returns a two-step delete button: the first press asks, a second press
// within three seconds deletes.
function deleteButton(path, onDeleted) {
  const button = element("button", "danger small", "Delete");
  let timer = 0;
  button.onclick = async (event) => {
    event.stopPropagation();
    if (!button.classList.contains("armed")) {
      button.classList.add("armed");
      button.textContent = "Delete?";
      timer = setTimeout(() => {
        button.classList.remove("armed");
        button.textContent = "Delete";
      }, 3000);
      return;
    }
    clearTimeout(timer);
    button.disabled = true;
    try {
      const response = await fetch(
        "/api/delete?path=" + encodeURIComponent(path), { method: "POST" });
      if (!response.ok) throw new Error(await response.text());
      onDeleted();
    } catch (e) {
      button.disabled = false;
      button.classList.remove("armed");
      button.textContent = "Failed";
    }
  };
  return button;
}

// --- Files ------------------------------------------------------------------

// The address names what is shown: "/" is the root of the card, "/browse/a/b"
// the directory or file a/b, and "?q=" the filter text. It can be bookmarked
// or sent to someone else.

const SORT_LABELS = { name: "Name", size: "Size", mtime: "Date" };

let currentPath = "";
let addressPath = "";
let entries = [];
let sortKey = "";
let sortDesc = false;
let filterText = "";

function browseUrl(path, filter) {
  const base = path ? "/browse/" + path.split("/").map(encodeURIComponent).join("/") : "/";
  return base + (filter ? "?q=" + encodeURIComponent(filter) : "");
}

function locationPath() {
  const match = location.pathname.match(/^\/browse\/(.*)$/);
  return match
    ? match[1].split("/").filter(Boolean).map(decodeURIComponent).join("/")
    : "";
}

function locationFilter() {
  return new URLSearchParams(location.search).get("q") || "";
}

function parentOf(path) {
  const slash = path.lastIndexOf("/");
  return slash < 0 ? "" : path.slice(0, slash);
}

function nameOf(path) {
  return path.slice(path.lastIndexOf("/") + 1);
}

// Puts the address in the history (replace: instead of a new entry).
function setAddress(path, replace, state) {
  addressPath = path;
  const url = browseUrl(path, filterText);
  if (location.pathname + location.search === url) return;
  history[replace ? "replaceState" : "pushState"](state || {}, "", url);
}

function showCrumbs(path) {
  const crumbs = $("crumbs");
  crumbs.replaceChildren();
  const parts = path ? path.split("/") : [];
  const add = (label, target) => {
    const link = element("a", "", label);
    link.href = browseUrl(target);
    link.onclick = (event) => {
      event.preventDefault();
      navigate(target);
    };
    crumbs.append(link);
  };
  add("SD card", "");
  parts.forEach((part, i) => {
    crumbs.append(" / ");
    add(part, parts.slice(0, i + 1).join("/"));
  });
}

function fileRow(path, entry, thumbDir) {
  const full = path ? path + "/" + entry.name : entry.name;
  const row = element("div", "row");

  const icon = element("div", "icon");
  if (!entry.dir && thumbDir && !/\.part$/i.test(entry.name)) {
    const img = element("img");
    img.loading = "lazy";
    img.alt = "";
    img.src = fileUrl(thumbDir + "/" + entry.name.replace(/\.[^.]+$/, "") + ".jpg");
    img.onerror = () => {
      icon.textContent = ext(entry.name);
    };
    icon.append(img);
  } else {
    icon.textContent = entry.dir ? "DIR" : ext(entry.name);
  }

  const name = element("div", "name");
  const link = element("a", "", entry.name);
  link.href = browseUrl(full);
  name.append(link);

  const meta = element("div", "meta");
  if (!entry.dir) {
    meta.append(size(entry.size));
    if (entry.mtime > 1e9) {
      meta.append(element("span", "date",
        " · " + new Date(entry.mtime * 1000).toLocaleString()));
    }
  } else if (path === "" && full in cameraSync) {
    meta.append("synced: " + syncText(cameraSync[full]));
  }

  row.append(icon, name, meta);
  if (!entry.dir) {
    const buttons = element("div", "buttons");
    const download = element("a", "button small", "Download");
    download.href = fileUrl(full, true);
    download.onclick = (event) => event.stopPropagation();
    buttons.append(download);
    if (deletable(full)) {
      buttons.append(deleteButton(full, () => {
        entries = entries.filter((e) => e !== entry);
        row.remove();
        showCount();
      }));
    }
    row.append(buttons);
  }
  const open = () => (entry.dir ? navigate(full) : openFile(full, entry.name));
  row.onclick = open;
  link.onclick = (event) => {
    event.preventDefault();
    event.stopPropagation();
    open();
  };
  return row;
}

function visibleEntries() {
  const needle = filterText.trim().toLowerCase();
  const list = entries.filter((e) => !needle || e.name.toLowerCase().includes(needle));
  const byName = (a, b) => a.name.localeCompare(b.name, undefined, { numeric: true });
  const direction = sortDesc ? -1 : 1;
  list.sort((a, b) => {
    if (a.dir !== b.dir) return b.dir - a.dir;  // directories first
    if (sortKey === "name") return direction * byName(a, b);
    if (sortKey === "size") return direction * (a.size - b.size) || byName(a, b);
    if (sortKey === "mtime") return direction * (a.mtime - b.mtime) || byName(a, b);
    return (b.mtime - a.mtime) || byName(a, b);
  });
  return list;
}

function showCount() {
  const shown = document.querySelectorAll("#files .row").length;
  $("count").textContent = filterText.trim()
    ? `${shown} of ${entries.length}`
    : `${entries.length} ${entries.length === 1 ? "item" : "items"}`;
}

function renderList() {
  const list = $("files");
  list.replaceChildren();
  for (const button of document.querySelectorAll("#listhead [data-sort]")) {
    const active = button.dataset.sort === sortKey;
    button.classList.toggle("sorted", active);
    button.textContent = SORT_LABELS[button.dataset.sort] +
      (active ? (sortDesc ? " ▼" : " ▲") : "");
  }
  $("filter").value !== filterText && ($("filter").value = filterText);
  const visible = visibleEntries();
  // Photos and videos in media/ have a thumbnail with the same base name in
  // thumb/ next to it.
  const thumbDir = /(^|\/)media$/.test(currentPath) ? currentPath.replace(/media$/, "thumb")
    : /(^|\/)thumb$/.test(currentPath) ? currentPath : null;
  for (const entry of visible) {
    list.append(fileRow(currentPath, entry, thumbDir));
  }
  if (!visible.length) {
    list.append(element("div", "empty", entries.length ? "No file matches." : "Empty."));
  }
  showCount();
}

// The listing of a directory; null if there is none, undefined if the dongle
// could not be reached.
async function listDirectory(path) {
  try {
    const response = await fetch("/api/files?path=" + encodeURIComponent(path));
    return response.ok ? await response.json() : null;
  } catch (e) {
    return undefined;
  }
}

// Shows what `path` names: a directory, or a file in a directory (which opens
// in the viewer).
async function show(path) {
  const data = await listDirectory(path);
  const dir = data ? path : parentOf(path);
  const listing = data || (await listDirectory(dir));
  currentPath = dir;
  showCrumbs(dir);
  entries = listing
    ? listing.entries.filter((e) => e.name !== "." && e.name !== "..")
    : [];
  renderList();
  if (data) {
    hideViewer();
    return;
  }
  const entry = entries.find((e) => e.name === nameOf(path) && !e.dir);
  if (entry) {
    view(path, entry.name);
  } else {
    hideViewer();
    $("files").replaceChildren(element("div", "empty",
      listing === undefined ? "Could not read the directory." : `Not found: ${path}`));
  }
}

// A click on a directory: new history entry, filter cleared.
function navigate(path) {
  filterText = "";
  setAddress(path, false);
  return show(path);
}

function openDir(path) {
  return show(path);
}

function openFile(path, name) {
  setAddress(path, false, { viewer: true });
  return view(path, name);
}

// --- Viewer -----------------------------------------------------------------

function hideViewer() {
  $("viewer").hidden = true;
  $("vbody").replaceChildren();
}

// The close button: back to the directory's address.
function closeViewer() {
  if (history.state && history.state.viewer) {
    history.back();  // popstate hides the viewer
    return;
  }
  setAddress(currentPath, true);
  hideViewer();
}

async function view(path, name) {
  const body = $("vbody");
  body.replaceChildren();
  $("vname").textContent = name;
  $("vdownload").href = fileUrl(path, true);
  const remove = $("vdelete");
  remove.replaceChildren();
  if (deletable(path)) {
    remove.append(deleteButton(path, () => {
      setAddress(currentPath, true);
      show(currentPath);
    }));
  }
  const lower = name.toLowerCase();
  if (/\.jpe?g$/.test(lower)) {
    const img = element("img");
    img.src = fileUrl(path);
    body.append(img);
  } else if (/\.(mp4|mov)$/.test(lower)) {
    const video = element("video");
    video.src = fileUrl(path);
    video.controls = true;
    body.append(video);
  } else if (/\.aac$/.test(lower)) {
    const audio = element("audio");
    audio.src = fileUrl(path);
    audio.controls = true;
    body.append(audio);
  } else if (/\.(txt|csv|log|json)$/.test(lower)) {
    const pre = element("pre", "", "Loading…");
    body.append(pre);
    try {
      pre.textContent = await (await fetch(fileUrl(path))).text();
    } catch (e) {
      pre.textContent = "Could not load the file.";
    }
  } else {
    // Raw H.264 streams play in VLC; see the README for making an MP4.
    body.append(element("p", "note", /\.h264$/.test(lower)
      ? "Raw H.264 video: download it and open it with VLC."
      : "No preview for this file type."));
  }
  $("viewer").hidden = false;
}

// --- Live log ---------------------------------------------------------------

let logOffset = -1;
let logPath = "";

async function refreshLog() {
  if (!$("follow").checked) return;
  try {
    const response = await fetch("/api/log?offset=" + logOffset);
    const path = response.headers.get("X-Log-Path") || "";
    const text = await response.text();
    const pre = $("log");
    if (path !== logPath) {
      if (logPath) pre.textContent += `\n--- ${path} ---\n`;
      logPath = path;
      $("logname").textContent = path;
      if (logOffset >= 0) {
        logOffset = -1;
        return;
      }
    }
    logOffset = parseInt(response.headers.get("X-Log-Size") || "0", 10);
    if (text) {
      const atBottom = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 20;
      pre.textContent += text;
      // Keep the view small on phones.
      if (pre.textContent.length > 200000) {
        pre.textContent = pre.textContent.slice(-150000);
      }
      if (atBottom) pre.scrollTop = pre.scrollHeight;
    }
  } catch (e) {
    // Retried with the next poll.
  }
}

// --- Start ------------------------------------------------------------------

$("search").onclick = () => post("/api/search");
$("stop").onclick = () => post("/api/stop");
$("vclose").onclick = closeViewer;

$("filter").oninput = () => {
  filterText = $("filter").value;
  renderList();
  // The filter is part of the address, without a history entry of its own.
  history.replaceState(history.state, "", browseUrl(addressPath, filterText));
};
for (const button of document.querySelectorAll("#listhead [data-sort]")) {
  button.onclick = () => {
    const key = button.dataset.sort;
    sortDesc = sortKey === key ? !sortDesc : key !== "name";
    sortKey = key;
    renderList();
  };
}

window.onpopstate = () => {
  filterText = locationFilter();
  addressPath = locationPath();
  show(addressPath);
};

refreshStatus();
filterText = locationFilter();
addressPath = locationPath();
show(addressPath);
refreshLog();
setInterval(refreshStatus, 2000);
setInterval(refreshLog, 1500);
