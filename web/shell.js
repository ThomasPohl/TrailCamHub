// Page shell: switches between the two views ("Setup assistant" and
// "Advanced") and between the tabs of the advanced view, shows the connection
// in the top bar and adds a "show" button to every password field.
//
// The view is part of the address (#wizard, #advanced, #advanced/console), so
// that it can be linked and survives a reload; the last one is also
// remembered in this browser.
(function () {
  "use strict";

  const VIEWS = ["wizard", "advanced"];
  const STORAGE_KEY = "trailcamhub.installer.view";
  const viewTabs = [...document.querySelectorAll(".view-switch [role=tab]")];
  const advTabs = [...document.querySelectorAll(".adv-tabs [role=tab]")];
  const advMain = document.querySelector(".adv-main");
  if (viewTabs.length === 0) {
    return;
  }
  const TABS = advTabs.map((tab) => tab.dataset.tab);

  let view = "wizard";
  let tab = TABS[0];

  function remember() {
    try {
      window.localStorage.setItem(STORAGE_KEY, view === "advanced" ? `advanced/${tab}` : view);
    } catch {
      // Storage blocked: the address still carries the view.
    }
  }

  function remembered() {
    try {
      return window.localStorage.getItem(STORAGE_KEY) ?? "";
    } catch {
      return "";
    }
  }

  // "advanced/console" → { view: "advanced", tab: "console" }; null for
  // anything else (for example an anchor of an older link).
  function parse(text) {
    const [name, sub] = text.replace(/^#/, "").split("/");
    if (!VIEWS.includes(name)) {
      return null;
    }
    return { view: name, tab: TABS.includes(sub) ? sub : null };
  }

  function select(tabs, attribute, value) {
    for (const button of tabs) {
      const on = button.dataset[attribute] === value;
      button.setAttribute("aria-selected", String(on));
      button.tabIndex = on ? 0 : -1;
      document.getElementById(button.getAttribute("aria-controls")).hidden = !on;
    }
  }

  function apply(next, updateAddress = true) {
    const viewChanged = next.view !== view;
    view = next.view;
    tab = next.tab ?? tab;
    document.body.dataset.view = view;
    select(viewTabs, "view", view);
    select(advTabs, "tab", tab);
    if (viewChanged) {
      window.scrollTo({ top: 0 });
    }
    advMain.scrollTop = 0;
    if (view === "advanced" && tab === "console") {
      // Hidden, the console could not follow its output.
      const output = document.getElementById("serial-output");
      output.scrollTop = output.scrollHeight;
    }
    remember();
    if (updateAddress) {
      const hash = view === "advanced" ? `#advanced/${tab}` : "#wizard";
      if (window.location.hash !== hash) {
        window.history.replaceState(null, "", hash);
      }
    }
  }

  // Arrow keys move within a tab list, as in every tab control.
  function wire(tabs, toTarget) {
    tabs.forEach((button, index) => {
      button.addEventListener("click", () => apply(toTarget(button)));
      button.addEventListener("keydown", (event) => {
        const step = { ArrowRight: 1, ArrowLeft: -1 }[event.key];
        if (!step) {
          return;
        }
        event.preventDefault();
        const other = tabs[(index + step + tabs.length) % tabs.length];
        other.focus();
        apply(toTarget(other));
      });
    });
  }
  wire(viewTabs, (button) => ({ view: button.dataset.view, tab: null }));
  wire(advTabs, (button) => ({ view: "advanced", tab: button.dataset.tab }));

  // Links like <a href="#advanced/firmware"> inside the page.
  window.addEventListener("hashchange", () => {
    const target = parse(window.location.hash);
    if (target) {
      apply(target, false);
    }
  });

  window.trailcamShell = {
    // show("advanced", "console"), show("wizard")
    show: (name, sub) => apply({ view: name, tab: sub ?? null }),
  };

  apply(parse(window.location.hash) ?? parse(remembered()) ?? { view: "wizard", tab: null }, false);

  // --- Connection in the top bar ---

  const pill = document.getElementById("conn-pill");
  function showConnection(on) {
    pill.textContent = on ? "Hub connected" : "Not connected";
    pill.dataset.connected = String(on);
  }
  window.addEventListener("serial-connected", () => showConnection(true));
  window.addEventListener("serial-disconnected", () => showConnection(false));
  showConnection(false);
  if (!("serial" in navigator)) {
    document.getElementById("unsupported").hidden = false;
  }

  // --- Password fields ---

  for (const input of document.querySelectorAll('input[type="password"]')) {
    const wrap = document.createElement("span");
    wrap.className = "secret";
    const button = document.createElement("button");
    button.type = "button";
    button.className = "secret-toggle";
    button.textContent = "Show";
    button.setAttribute("aria-label", "Show what was typed");
    button.setAttribute("aria-pressed", "false");
    button.addEventListener("click", () => {
      const show = input.type === "password";
      input.type = show ? "text" : "password";
      button.textContent = show ? "Hide" : "Show";
      button.setAttribute("aria-pressed", String(show));
    });
    input.replaceWith(wrap);
    wrap.append(input, button);
  }
})();
