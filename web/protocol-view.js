// Live protocol view: taps the serial line stream (dispatched by
// serial-console.js as "serial-line" events) and turns the firmware's log
// lines into a structured picture of the camera session — a stage timeline,
// the parsed media list, and the raw bulk (thumbnail/photo) frames.
//
// It is read-only and best-effort: unknown lines are ignored, so it degrades
// gracefully as the firmware's logging evolves. A "Replay demo log" button
// feeds a bundled sample through the same parser, so the view can be seen
// without a board attached.
(function () {
  "use strict";

  // --- Stage timeline --------------------------------------------------------

  var STAGES = [
    { id: "discovery", label: "Discovery" },
    { id: "prelogin", label: "Pre-Login" },
    { id: "login", label: "Login" },
    { id: "session", label: "Session" },
    { id: "devinfo", label: "Device Info" },
    { id: "list", label: "Media List" },
    { id: "thumb", label: "Thumbnail" },
  ];

  var stageIndex = {};
  STAGES.forEach(function (s, i) {
    stageIndex[s.id] = i;
  });

  var timelineEl = document.getElementById("pv-timeline");
  var mediaWrap = document.getElementById("pv-media");
  var mediaBody = document.getElementById("pv-media-body");
  var mediaCaption = document.getElementById("pv-media-caption");
  var framesWrap = document.getElementById("pv-frames");
  var framesList = document.getElementById("pv-frames-list");
  var replayButton = document.getElementById("pv-replay");

  if (!timelineEl) {
    return; // The section is not on the page.
  }

  var reached = -1; // Highest stage index reached so far.
  var stageNotes = {};
  var currentFrame = null;

  function renderTimeline() {
    timelineEl.innerHTML = "";
    STAGES.forEach(function (stage, i) {
      var li = document.createElement("li");
      li.className =
        "pv-stage " +
        (i < reached ? "done" : i === reached ? "active" : "pending");
      var dot = document.createElement("span");
      dot.className = "pv-dot";
      var label = document.createElement("span");
      label.className = "pv-stage-label";
      label.textContent = stage.label;
      li.appendChild(dot);
      li.appendChild(label);
      if (stageNotes[stage.id]) {
        var note = document.createElement("span");
        note.className = "pv-stage-note";
        note.textContent = stageNotes[stage.id];
        li.appendChild(note);
      }
      timelineEl.appendChild(li);
    });
  }

  // Marks a stage (and everything before it) as reached.
  function reach(id, note) {
    var i = stageIndex[id];
    if (i === undefined) {
      return;
    }
    if (i > reached) {
      reached = i;
    }
    if (note) {
      stageNotes[id] = note;
    }
    renderTimeline();
  }

  function resetView() {
    reached = -1;
    stageNotes = {};
    currentFrame = null;
    currentThumb = null;
    mediaFiles = [];
    mediaListCnt = null;
    thumbs = {};
    renderTimeline();
    mediaWrap.hidden = true;
    mediaBody.innerHTML = "";
    mediaCaption.textContent = "";
    framesWrap.hidden = true;
    framesList.innerHTML = "";
  }

  // --- Media list ------------------------------------------------------------

  function fmtTime(unixSeconds) {
    if (!unixSeconds) {
      return "—";
    }
    var d = new Date(unixSeconds * 1000);
    if (isNaN(d.getTime())) {
      return String(unixSeconds);
    }
    return d.toISOString().replace("T", " ").replace(".000Z", " UTC");
  }

  // The camera lists a capture's photo (fileType 0) and video (fileType 2) as
  // two items with neighbouring numbers in the same directory. Without a
  // capture id in the list, items are paired by that adjacency and a close
  // capture time; ambiguous neighbours go to the pair with the smaller gap.
  var PAIR_MAX_GAP_S = 120;

  var mediaFiles = []; // last media list, in the camera's order
  var mediaListCnt = null; // "cnt" of the last media list
  var thumbs = {}; // "dir_num" -> data URL of a received thumbnail

  function isPhoto(f) {
    return Number(f.fileType) === 0;
  }

  function mediaKey(dir, num) {
    return dir + "_" + num;
  }

  function timeGap(a, b) {
    var ta = Number(a.mediaTime) || 0;
    var tb = Number(b.mediaTime) || 0;
    // Unknown times neither confirm nor rule out a pair.
    return ta > 0 && tb > 0 ? Math.abs(ta - tb) : PAIR_MAX_GAP_S;
  }

  // Returns groups of 1–2 items ({ items: [...] }), in the list's order.
  function groupMedia(files) {
    var entries = files.map(function (f, i) {
      return { f: f, idx: i, group: null };
    });
    var sorted = entries.slice().sort(function (a, b) {
      return (
        (Number(a.f.mediaDirNum) || 0) - (Number(b.f.mediaDirNum) || 0) ||
        (Number(a.f.mediaNum) || 0) - (Number(b.f.mediaNum) || 0)
      );
    });
    var candidates = [];
    for (var k = 0; k + 1 < sorted.length; k++) {
      var a = sorted[k];
      var b = sorted[k + 1];
      if (
        a.f.mediaDirNum != null &&
        a.f.mediaDirNum === b.f.mediaDirNum &&
        Math.abs(Number(a.f.mediaNum) - Number(b.f.mediaNum)) === 1 &&
        isPhoto(a.f) !== isPhoto(b.f) &&
        timeGap(a.f, b.f) <= PAIR_MAX_GAP_S
      ) {
        candidates.push({ a: a, b: b, gap: timeGap(a.f, b.f) });
      }
    }
    candidates.sort(function (x, y) {
      return x.gap - y.gap;
    });
    var groups = [];
    candidates.forEach(function (c) {
      if (c.a.group || c.b.group) {
        return;
      }
      var g = { items: [], first: Math.min(c.a.idx, c.b.idx) };
      c.a.group = c.b.group = g;
      groups.push(g);
    });
    entries.forEach(function (e) {
      if (!e.group) {
        e.group = { items: [], first: e.idx };
        groups.push(e.group);
      }
      e.group.items.push(e.f);
    });
    groups.forEach(function (g) {
      // Video first: the photo is the video's still.
      g.items.sort(function (x, y) {
        return Number(isPhoto(x)) - Number(isPhoto(y));
      });
    });
    return groups.sort(function (x, y) {
      return x.first - y.first;
    });
  }

  function groupThumb(group) {
    for (var i = 0; i < group.items.length; i++) {
      var f = group.items[i];
      var url = thumbs[mediaKey(f.mediaDirNum, f.mediaNum)];
      if (url) {
        return { url: url, item: f };
      }
    }
    return null;
  }

  function typeLabel(group) {
    if (group.items.length === 2) {
      return "🎬 Video + 📷 Photo";
    }
    return isPhoto(group.items[0]) ? "📷 Photo" : "🎬 Video";
  }

  function joined(group, field) {
    return group.items
      .map(function (f) {
        return f[field] != null ? String(f[field]) : "—";
      })
      .join(" + ");
  }

  function groupTime(group) {
    var times = group.items
      .map(function (f) {
        return Number(f.mediaTime) || 0;
      })
      .filter(function (t) {
        return t > 0;
      });
    return times.length ? Math.min.apply(null, times) : 0;
  }

  function groupDuration(group) {
    for (var i = 0; i < group.items.length; i++) {
      var ms = Number(group.items[i].durationMs) || 0;
      if (ms > 0) {
        return Math.round(ms / 1000) + " s";
      }
    }
    return "";
  }

  function thumbCell(group) {
    var td = document.createElement("td");
    td.className = "pv-thumb-cell";
    var thumb = groupThumb(group);
    if (thumb) {
      var img = document.createElement("img");
      img.className = "pv-thumb";
      img.src = thumb.url;
      img.alt =
        "Thumbnail " + mediaKey(thumb.item.mediaDirNum, thumb.item.mediaNum);
      img.title = "Click to enlarge";
      img.addEventListener("click", function () {
        img.classList.toggle("enlarged");
      });
      td.appendChild(img);
    } else {
      var ph = document.createElement("span");
      ph.className = "pv-thumb-placeholder";
      ph.textContent = "no preview";
      td.appendChild(ph);
    }
    return td;
  }

  function renderMedia() {
    mediaBody.innerHTML = "";
    var groups = groupMedia(mediaFiles);
    groups.forEach(function (group, i) {
      var tr = document.createElement("tr");
      if (group.items.length === 2) {
        tr.className = "pv-pair";
      }
      tr.appendChild(thumbCell(group));
      var duration = groupDuration(group);
      var cells = [
        String(i + 1),
        typeLabel(group) + (duration ? " · " + duration : ""),
        group.items[0].mediaDirNum != null
          ? String(group.items[0].mediaDirNum)
          : "—",
        joined(group, "mediaNum"),
        joined(group, "mediaId"),
        fmtTime(groupTime(group)),
      ];
      cells.forEach(function (text) {
        var td = document.createElement("td");
        td.textContent = text;
        tr.appendChild(td);
      });
      mediaBody.appendChild(tr);
    });
    var photos = mediaFiles.filter(isPhoto).length;
    var pairs = groups.filter(function (g) {
      return g.items.length === 2;
    }).length;
    var shown = Object.keys(thumbs).length;
    mediaCaption.textContent =
      mediaFiles.length +
      " files in " +
      groups.length +
      " captures (" +
      pairs +
      " video + photo) · " +
      photos +
      " photos · " +
      (mediaFiles.length - photos) +
      " videos" +
      (shown ? " · " + shown + " thumbnail" + (shown === 1 ? "" : "s") : "") +
      (mediaListCnt != null ? " · camera reports cnt " + mediaListCnt : "");
    mediaWrap.hidden = mediaFiles.length === 0;
  }

  function renderMediaList(json) {
    var data;
    try {
      data = JSON.parse(json);
    } catch (e) {
      return false;
    }
    mediaFiles = (data && data.mediaFiles) || [];
    mediaListCnt = data && data.cnt != null ? data.cnt : null;
    renderMedia();
    return true;
  }

  // --- Thumbnail preview -----------------------------------------------------
  // With CONFIG_CAMERA_THUMBNAIL_SERIAL_PREVIEW the firmware prints the first
  // thumbnail of a session as "THUMB JPEG dir=… num=… bytes=…", Base64 lines
  // "THUMB b64:…" and "THUMB b64:END".

  var currentThumb = null;

  function finishThumb() {
    var t = currentThumb;
    currentThumb = null;
    if (!t || !t.b64) {
      return;
    }
    thumbs[mediaKey(t.dir, t.num)] = "data:image/jpeg;base64," + t.b64;
    stageNotes.thumb = mediaKey(t.dir, t.num) + " shown";
    renderTimeline();
    renderMedia();
  }

  // --- Bulk frames -----------------------------------------------------------

  var HEX_LINE = /^(?:[0-9a-fA-F]{2}[ :]?)+$/;
  var ARTEMIS_SIG = [0x41, 0x52, 0x54, 0x45, 0x4d, 0x49, 0x53, 0x00];

  function u16le(bytes, off) {
    return bytes[off] | (bytes[off + 1] << 8);
  }
  function u32le(bytes, off) {
    return (
      (bytes[off] |
        (bytes[off + 1] << 8) |
        (bytes[off + 2] << 16) |
        (bytes[off + 3] << 24)) >>>
      0
    );
  }

  function startFrame(subtype, seq, bodyLen, payloadLen) {
    currentFrame = {
      subtype: subtype,
      seq: seq,
      bodyLen: bodyLen,
      payloadLen: payloadLen,
      bytes: [],
    };
    reach("thumb", "receiving bulk data");
  }

  function finishFrame() {
    if (!currentFrame) {
      return;
    }
    var f = currentFrame;
    currentFrame = null;

    var card = document.createElement("div");
    card.className = "pv-frame";

    var head = document.createElement("div");
    head.className = "pv-frame-head";
    var subtypeHex = f.subtype.toString(16).padStart(2, "0");
    var kind =
      f.subtype === 0x04
        ? "thumbnail"
        : f.subtype === 0x03
        ? "photo"
        : f.subtype === 0x02
        ? "video"
        : "?";
    head.textContent =
      "subtype 0x" +
      subtypeHex +
      " (" +
      kind +
      ") · seq " +
      f.seq +
      " · payload " +
      f.payloadLen +
      " B";
    card.appendChild(head);

    var b = f.bytes;
    var notes = [];
    var hasSig =
      b.length >= 8 &&
      ARTEMIS_SIG.every(function (v, i) {
        return b[i] === v;
      });
    if (hasSig) {
      var ver = u32le(b, 8);
      var typ = u32le(b, 12);
      var len = u32le(b, 16);
      notes.push("ARTEMIS record: ver " + ver + ", typ " + typ + ", len " + len);
      // Record payload = 72-byte media header + JPEG.
      var hdr = 20;
      if (b.length >= hdr + 0x34) {
        notes.push(
          "media header: dir " +
            u16le(b, hdr + 0x20) +
            ", num " +
            u16le(b, hdr + 0x22) +
            ", id " +
            u32le(b, hdr + 0x30)
        );
      }
      var jpeg = hdr + 72;
      if (b.length >= jpeg + 2) {
        notes.push(
          b[jpeg] === 0xff && b[jpeg + 1] === 0xd8
            ? "JPEG SOI present at offset " + jpeg + " ✓"
            : "no JPEG SOI at offset " + jpeg
        );
      }
    } else if (b.length) {
      notes.push("no ARTEMIS signature in first bytes");
    }

    notes.forEach(function (text) {
      var n = document.createElement("div");
      n.className = "pv-frame-note";
      n.textContent = text;
      card.appendChild(n);
    });

    if (b.length) {
      var hex = document.createElement("pre");
      hex.className = "pv-hex";
      hex.textContent = hexDump(b);
      card.appendChild(hex);
    }

    framesWrap.hidden = false;
    framesList.insertBefore(card, framesList.firstChild);
    // Keep the list bounded.
    while (framesList.children.length > 24) {
      framesList.removeChild(framesList.lastChild);
    }
  }

  function hexDump(bytes) {
    var lines = [];
    for (var i = 0; i < bytes.length; i += 16) {
      var slice = bytes.slice(i, i + 16);
      var hex = slice
        .map(function (v) {
          return v.toString(16).padStart(2, "0");
        })
        .join(" ");
      var ascii = slice
        .map(function (v) {
          return v >= 0x20 && v < 0x7f ? String.fromCharCode(v) : ".";
        })
        .join("");
      lines.push(
        i.toString(16).padStart(4, "0") + "  " + hex.padEnd(47, " ") + "  " + ascii
      );
    }
    return lines.join("\n");
  }

  // --- Line parsing ----------------------------------------------------------

  // Strips ANSI color codes and the ESP-IDF "I (12345) tag:" prefix, returning
  // the bare message.
  function messageOf(rawLine) {
    var line = rawLine.replace(/\u001b\[[0-9;]*m/g, "").trimEnd();
    var m = line.match(/^[VDIWE] \(\d+\) [^:]+:\s?(.*)$/);
    return m ? m[1] : line.trim();
  }

  var patterns = [
    [/^Camera discovery target/, function () { reach("discovery", "searching"); }],
    [/^Camera UDP response accepted/, function () { reach("prelogin"); stageNotes.discovery = "found"; }],
    [/^PRE_LOGIN response received/, function () { reach("login"); stageNotes.prelogin = "ok"; }],
    [/^ARTEMIS login succeeded; token (\d+)/, function (m) { reach("session"); stageNotes.login = "token " + m[1]; }],
    [/^Heartbeat index \d+ acknowledged/, function () { reach("devinfo"); stageNotes.session = "settled"; }],
    [/^ARTEMIS response cmdId 512/, function () { reach("list"); stageNotes.devinfo = "ok"; }],
    [/^Media list parsed: (\d+) items/, function (m) { stageNotes.list = m[1] + " items"; renderTimeline(); }],
    [/^Sent ARTEMIS GetThumbnail \(cmdId 772\)/, function () { reach("thumb", "requested"); }],
    [
      /^THUMB JPEG dir=(-?\d+) num=(-?\d+) bytes=(\d+)/,
      function (m) {
        reach("thumb", "receiving preview");
        currentThumb = { dir: Number(m[1]), num: Number(m[2]), b64: "" };
      },
    ],
    [/^THUMB b64:END$/, finishThumb],
    [
      /^THUMB b64:([A-Za-z0-9+/=]+)$/,
      function (m) {
        if (currentThumb) {
          currentThumb.b64 += m[1];
        }
      },
    ],
    [
      /^ARTEMIS response cmdId 768:\s*(\{.*\})\s*$/,
      function (m) {
        reach("list");
        if (renderMediaList(m[1])) {
          stageNotes.list = "loaded";
          renderTimeline();
        }
      },
    ],
    [
      /^BULK frame: subtype 0x([0-9a-fA-F]+) seq (\d+) body_len (\d+) payload (\d+) bytes/,
      function (m) {
        finishFrame(); // flush any previous frame
        startFrame(parseInt(m[1], 16), Number(m[2]), Number(m[3]), Number(m[4]));
      },
    ],
  ];

  function handleLine(rawLine) {
    var msg = messageOf(rawLine);
    if (!msg) {
      return;
    }

    // While collecting a bulk frame, consume its hex-dump lines.
    if (currentFrame && HEX_LINE.test(msg)) {
      msg
        .replace(/[: ]/g, " ")
        .trim()
        .split(/\s+/)
        .forEach(function (pair) {
          if (pair.length === 2) {
            currentFrame.bytes.push(parseInt(pair, 16));
          }
        });
      return;
    }
    // A non-hex line ends the current frame.
    if (currentFrame) {
      finishFrame();
    }

    for (var i = 0; i < patterns.length; i++) {
      var m = msg.match(patterns[i][0]);
      if (m) {
        patterns[i][1](m);
        return;
      }
    }
  }

  window.addEventListener("serial-line", function (e) {
    handleLine(String(e.detail || ""));
  });
  window.addEventListener("serial-reset", resetView);

  // --- Demo replay -----------------------------------------------------------

  var DEMO_LINES = buildDemoLog();

  function buildDemoLog() {
    var g = "\u001b[0;32m"; // ESP-IDF INFO color, to exercise ANSI stripping
    var r = "\u001b[0m";
    var T = "trailcamhub_protocol";
    function info(ts, msg) {
      return g + "I (" + ts + ") " + T + ": " + msg + r;
    }
    var mediaJson = JSON.stringify({
      mediaFiles: [
        { fileType: 0, mediaDirNum: 100, mediaNum: 36, durationMs: 0, mediaId: 4136037234, mediaTime: 1766505692 },
        { fileType: 2, mediaDirNum: 100, mediaNum: 35, durationMs: 10000, mediaId: 3987654321, mediaTime: 1766505600 },
        { fileType: 0, mediaDirNum: 100, mediaNum: 34, durationMs: 0, mediaId: 2871466130, mediaTime: 1766490135 },
        { fileType: 2, mediaDirNum: 100, mediaNum: 33, durationMs: 10000, mediaId: 2765310048, mediaTime: 1766490043 },
        { fileType: 0, mediaDirNum: 100, mediaNum: 1, durationMs: 0, mediaId: 1343182691, mediaTime: 1766400000 },
      ],
      cnt: 36,
      pageNo: 0,
      result: 0,
      cmdId: 768,
    });
    // A small night-vision style JPEG standing in for the camera's thumbnail
    // of 100_35, as the firmware's serial preview prints it.
    var demoThumb =
      "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";
    var thumbLines = [info(3550, "THUMB JPEG dir=100 num=35 bytes=1727")];
    for (var t = 0; t < demoThumb.length; t += 120) {
      thumbLines.push(info(3560, "THUMB b64:" + demoThumb.slice(t, t + 120)));
    }
    thumbLines.push(info(3570, "THUMB b64:END"));
    // A plausible first thumbnail frame: ARTEMIS header + 72-byte media header
    // (dir 100, num 36, id 4136037234) + JPEG SOI.
    var payload = new Array(94).fill(0);
    [0x41, 0x52, 0x54, 0x45, 0x4d, 0x49, 0x53, 0x00].forEach(function (v, i) { payload[i] = v; });
    payload[8] = 0x04; // ver
    payload[12] = 0x04; // typ
    payload[16] = 0x80; payload[17] = 0x02; // len (lo bytes)
    payload[20 + 0x20] = 100 & 0xff; payload[20 + 0x21] = (100 >> 8) & 0xff;
    payload[20 + 0x22] = 36 & 0xff; payload[20 + 0x23] = (36 >> 8) & 0xff;
    var id = 4136037234;
    payload[20 + 0x30] = id & 0xff;
    payload[20 + 0x31] = (id >>> 8) & 0xff;
    payload[20 + 0x32] = (id >>> 16) & 0xff;
    payload[20 + 0x33] = (id >>> 24) & 0xff;
    payload[92] = 0xff; payload[93] = 0xd8; // JPEG SOI
    var hexLines = [];
    for (var i = 0; i < payload.length; i += 16) {
      hexLines.push(
        info(
          "41" + i,
          payload
            .slice(i, i + 16)
            .map(function (v) { return v.toString(16).padStart(2, "0"); })
            .join(" ")
        )
      );
    }
    return [
      info(1200, "Camera discovery target 192.168.43.1, source UDP port 16734, payload 24 bytes"),
      info(1420, "Camera UDP response accepted; using source port 32108 for PRE_LOGIN"),
      info(1650, "PRE_LOGIN response received +230 ms after send / handshake is not verified"),
      info(1980, "ARTEMIS login succeeded; token 2043399292"),
      info(2010, "Starting ARTEMIS heartbeat loop"),
      info(2240, "Heartbeat index 1 acknowledged"),
      info(2480, "ARTEMIS response cmdId 512: {\"model\":\"TC100\",\"result\":0,\"cmdId\":512}"),
      info(2600, "Sent ARTEMIS GetMediaList (cmdId 768), index 3"),
      info(2900, "ARTEMIS response cmdId 768: " + mediaJson),
      info(2910, "Media list parsed: 5 items (3 photos, 2 videos), cnt 36, pageNo 0, result 0"),
      info(2915, "  first item: dir 100 num 36 id 4136037234 time 1766505692"),
      info(3100, "Sent ARTEMIS GetThumbnail (cmdId 772), index 4"),
      info(3400, "BULK frame: subtype 0x04 seq 0 body_len 98 payload 94 bytes"),
    ].concat(hexLines, thumbLines, [info(3600, "Heartbeat index 2 acknowledged")]);
  }

  function replayDemo() {
    resetView();
    var i = 0;
    replayButton.disabled = true;
    var timer = setInterval(function () {
      if (i >= DEMO_LINES.length) {
        clearInterval(timer);
        finishFrame();
        replayButton.disabled = false;
        return;
      }
      handleLine(DEMO_LINES[i++]);
    }, 180);
  }

  if (replayButton) {
    replayButton.addEventListener("click", replayDemo);
  }

  renderTimeline();
})();
