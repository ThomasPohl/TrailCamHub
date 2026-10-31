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

let currentPath = "";

function showCrumbs(path) {
  const crumbs = $("crumbs");
  crumbs.replaceChildren();
  const parts = path ? path.split("/") : [];
  const add = (label, target) => {
    const link = element("a", "", label);
    link.onclick = () => openDir(target);
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

  const meta = element("div", "meta");
  if (!entry.dir) {
    meta.append(size(entry.size));
    if (entry.mtime > 1e9) {
      meta.append(element("span", "date",
        " · " + new Date(entry.mtime * 1000).toLocaleString()));
    }
  }

  row.append(icon, element("div", "name", entry.name), meta);
  if (!entry.dir && deletable(full)) {
    row.append(deleteButton(full, () => row.remove()));
  }
  row.onclick = () => (entry.dir ? openDir(full) : view(full, entry.name));
  return row;
}

async function openDir(path) {
  currentPath = path;
  showCrumbs(path);
  const list = $("files");
  list.replaceChildren();
  let data;
  try {
    data = await (await fetch("/api/files?path=" + encodeURIComponent(path))).json();
  } catch (e) {
    list.append(element("div", "empty", "Could not read the directory."));
    return;
  }
  const entries = data.entries.filter((e) => e.name !== "." && e.name !== "..");
  entries.sort((a, b) => (b.dir - a.dir) || (b.mtime - a.mtime) ||
    a.name.localeCompare(b.name, undefined, { numeric: true }));
  if (!entries.length) {
    list.append(element("div", "empty", "Empty."));
    return;
  }
  // Photos and videos in media/ have a thumbnail with the same base name in
  // thumb/ next to it.
  const thumbDir = /(^|\/)media$/.test(path) ? path.replace(/media$/, "thumb")
    : /(^|\/)thumb$/.test(path) ? path : null;
  for (const entry of entries) {
    list.append(fileRow(path, entry, thumbDir));
  }
}

// --- Viewer -----------------------------------------------------------------

function closeViewer() {
  $("viewer").hidden = true;
  $("vbody").replaceChildren();
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
      closeViewer();
      openDir(currentPath);
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

refreshStatus();
openDir("");
refreshLog();
setInterval(refreshStatus, 2000);
setInterval(refreshLog, 1500);
