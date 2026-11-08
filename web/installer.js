// Populates the build-version selector from versions.json and points the
// install buttons and download links at the chosen build.
(function () {
  "use strict";

  var select = document.getElementById("version");
  var info = document.getElementById("build-info");
  var devButton = document.getElementById("install-dev");
  var relButton = document.getElementById("install-release");
  // The assistant installs the release build of the chosen version.
  var wizardButton = document.getElementById("install-wizard");
  var dlDev = document.getElementById("dl-dev");
  var dlRel = document.getElementById("dl-release");
  var dlSha = document.getElementById("dl-sha");

  function dirOf(path) {
    return path.slice(0, path.lastIndexOf("/") + 1);
  }

  function apply(build) {
    if (!build) {
      return;
    }
    devButton.setAttribute("manifest", "./" + build.dev);
    relButton.setAttribute("manifest", "./" + build.release);
    if (wizardButton) {
      wizardButton.setAttribute("manifest", "./" + build.release);
    }
    dlDev.setAttribute("href", "./" + build.devBin);
    dlRel.setAttribute("href", "./" + build.releaseBin);
    dlSha.setAttribute("href", "./" + dirOf(build.dev) + "SHA256SUMS");

    var parts = [build.date];
    if (build.branch) {
      parts.push(build.branch);
    }
    parts.push(build.commit);
    if (build.message) {
      parts.push(build.message);
    }
    info.textContent = parts.join(" · ");
  }

  fetch("./versions.json", { cache: "no-store" })
    .then(function (response) {
      if (!response.ok) {
        throw new Error("HTTP " + response.status);
      }
      return response.json();
    })
    .then(function (data) {
      var builds = (data && data.builds) || [];
      select.innerHTML = "";
      if (builds.length === 0) {
        var empty = document.createElement("option");
        empty.textContent = "No builds published yet";
        select.appendChild(empty);
        select.disabled = true;
        info.textContent = "Run the Firmware Package workflow to publish a build.";
        return;
      }
      builds.forEach(function (build, index) {
        var option = document.createElement("option");
        option.value = String(index);
        option.textContent =
          build.date + " (" + build.commit + ")" +
          (index === 0 ? " — latest" : "");
        select.appendChild(option);
      });
      select.addEventListener("change", function () {
        apply(builds[Number(select.value)]);
      });
      apply(builds[0]);
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
