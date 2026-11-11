// Populates the build-version and board selectors from versions.json and
// boards.json and points the install buttons and download links at the chosen
// build of the chosen board. The assistant picks the board through
// window.trailcamInstaller.setBoard().
(function () {
  "use strict";

  var select = document.getElementById("version");
  var boardSelect = document.getElementById("installer-board");
  var info = document.getElementById("build-info");
  var missing = document.getElementById("build-missing");
  var devButton = document.getElementById("install-dev");
  var relButton = document.getElementById("install-release");
  // The assistant installs the release build of the chosen version.
  var wizardButton = document.getElementById("install-wizard");
  var dlDev = document.getElementById("dl-dev");
  var dlRel = document.getElementById("dl-release");
  var dlSha = document.getElementById("dl-sha");

  var builds = [];
  var current = null;
  var board = "esp32s3";
  var listeners = [];

  function dirOf(path) {
    return path.slice(0, path.lastIndexOf("/") + 1);
  }

  // The files of `id` in a build; builds before the per-board installer
  // (#156) have only the ESP32-S3 main board at the top level.
  function filesOf(build, id) {
    if (!build) {
      return null;
    }
    if (build.boards) {
      return build.boards[id] || null;
    }
    return id === "esp32s3" ? build : null;
  }

  // The release tag of a build ("v0.1.0"), or "". Builds published by hand
  // from a tag before "version" existed have the tag in "branch".
  function versionOf(build) {
    var name = build.version || build.branch || "";
    return /^v\d+\.\d+\.\d+/.test(name) ? name : "";
  }

  // "v0.1.0 (ebc98a1) — 2026-11-08" for a tag build, otherwise date and
  // commit. The newest build and the newest release (a tag without a suffix)
  // are marked.
  function labelOf(build, index, latestRelease) {
    var version = versionOf(build);
    var label = version
      ? version + " (" + build.commit + ") — " + String(build.date).slice(0, 10)
      : build.date + " (" + build.commit + ")";
    var marks = [];
    if (index === 0) {
      marks.push("latest");
    }
    if (build === latestRelease) {
      marks.push("latest release");
    }
    return marks.length ? label + " — " + marks.join(", ") : label;
  }

  function setManifest(button, path) {
    if (!button) {
      return;
    }
    if (path) {
      button.setAttribute("manifest", "./" + path);
      button.hidden = false;
    } else {
      button.removeAttribute("manifest");
      button.hidden = true;
    }
  }

  function apply() {
    var files = filesOf(current, board);
    setManifest(devButton, files && files.dev);
    setManifest(relButton, files && files.release);
    setManifest(wizardButton, files && files.release);
    if (files) {
      dlDev.setAttribute("href", "./" + files.devBin);
      dlRel.setAttribute("href", "./" + files.releaseBin);
      dlSha.setAttribute("href", "./" + dirOf(files.dev) + "SHA256SUMS");
    }
    if (missing) {
      missing.hidden = Boolean(files) || !current;
    }
    if (current) {
      var parts = [current.date];
      if (current.branch) {
        parts.push(current.branch);
      }
      parts.push(current.commit);
      if (current.message) {
        parts.push(current.message);
      }
      info.textContent = parts.join(" · ");
    }
    listeners.forEach(function (listener) {
      listener(Boolean(files));
    });
  }

  window.trailcamInstaller = {
    // Points every install button at board `id` (an id of boards.json).
    setBoard: function (id) {
      board = id;
      if (boardSelect) {
        boardSelect.value = id;
      }
      apply();
    },
    // True if the chosen build has an image of board `id`.
    hasImage: function (id) {
      return Boolean(filesOf(current, id));
    },
    // `listener(available)` after every change of build or board.
    onChange: function (listener) {
      listeners.push(listener);
    },
  };

  if (boardSelect) {
    boardSelect.addEventListener("change", function () {
      window.trailcamInstaller.setBoard(boardSelect.value);
    });
    fetch("./boards.json", { cache: "no-store" })
      .then(function (response) {
        return response.ok ? response.json() : Promise.reject();
      })
      .then(function (data) {
        var list = (data && data.boards) || [];
        if (list.length === 0) {
          return;
        }
        boardSelect.innerHTML = "";
        list.forEach(function (entry) {
          var option = document.createElement("option");
          option.value = entry.id;
          option.textContent = entry.name;
          boardSelect.appendChild(option);
        });
        boardSelect.value = board;
      })
      .catch(function () {});
  }

  fetch("./versions.json", { cache: "no-store" })
    .then(function (response) {
      if (!response.ok) {
        throw new Error("HTTP " + response.status);
      }
      return response.json();
    })
    .then(function (data) {
      builds = (data && data.builds) || [];
      select.innerHTML = "";
      if (builds.length === 0) {
        var empty = document.createElement("option");
        empty.textContent = "No builds published yet";
        select.appendChild(empty);
        select.disabled = true;
        info.textContent = "Run the Firmware Package workflow to publish a build.";
        return;
      }
      var latestRelease = builds.find(function (build) {
        return /^v\d+\.\d+\.\d+$/.test(versionOf(build));
      });
      builds.forEach(function (build, index) {
        var option = document.createElement("option");
        option.value = String(index);
        option.textContent = labelOf(build, index, latestRelease);
        select.appendChild(option);
      });
      select.addEventListener("change", function () {
        current = builds[Number(select.value)];
        apply();
      });
      current = builds[0];
      apply();
    })
    .catch(function () {
      select.innerHTML = "";
      var option = document.createElement("option");
      option.textContent = "Build list unavailable";
      select.appendChild(option);
      select.disabled = true;
      info.textContent = "Could not load versions.json.";
    });
})();
