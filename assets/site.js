/* Shared navigation, theme toggle, on-page TOC and diagram tooltips.
   Works from file:// (no fetch, no modules). */
(function () {
  const PAGES = [
    { group: "Start here" },
    { href: "index.html", title: "Overview", dot: "var(--text-muted)" },
    { group: "The hardware" },
    { href: "hardware.html", title: "Dreamcast audio hardware", dot: "var(--c-aica)" },
    { href: "channel.html", title: "How an AICA channel makes sound", dot: "var(--c-aica)" },
    { href: "dsp.html", title: "The DSP and effects", dot: "var(--c-dsp)" },
    { href: "communication.html", title: "SH-4 ↔ ARM: interrupts & mailboxes", dot: "var(--c-host)" },
    { group: "The Manatee driver" },
    { href: "driver.html", title: "Driver architecture", dot: "var(--c-arm)" },
    { href: "music.html", title: "The MIDI engine & voices", dot: "var(--c-arm)" },
    { href: "sequencer.html", title: "The sequencer", dot: "var(--c-arm)" },
    { href: "sfx.html", title: "One-shots & PCM streams", dot: "var(--c-arm)" },
    { group: "Reference" },
    { href: "quirks.html", title: "Bugs, quirks & confidence", dot: "var(--critical)" },
    { href: "glossary.html", title: "Glossary & sources", dot: "var(--text-muted)" },
  ];

  // ---- theme ----
  const root = document.documentElement;
  const urlTheme = new URLSearchParams(location.search).get("theme");
  const saved = urlTheme || localStorage.getItem("sc-theme");
  if (saved) root.setAttribute("data-theme", saved);
  else root.setAttribute("data-theme", window.matchMedia && window.matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark");

  document.addEventListener("DOMContentLoaded", () => {
    const here = (location.pathname.split("/").pop() || "index.html");

    // ---- sidebar ----
    const side = document.querySelector(".sidebar");
    if (side) {
      let n = 0, html = "";
      for (const p of PAGES) {
        if (p.group) { html += `<h4>${p.group}</h4>`; continue; }
        n++;
        const act = p.href === here ? " active" : "";
        html += `<a class="${act.trim()}" href="${p.href}"><span class="dot" style="background:${p.dot}"></span>${p.title}</a>`;
      }
      side.innerHTML = html;
    }

    // ---- pager ----
    const pager = document.querySelector(".pager");
    const flat = PAGES.filter(p => p.href);
    const idx = flat.findIndex(p => p.href === here);
    if (pager && idx >= 0) {
      const prev = flat[idx - 1], next = flat[idx + 1];
      pager.innerHTML =
        (prev ? `<a class="prev" href="${prev.href}"><span>← Previous</span>${prev.title}</a>` : "<span></span>") +
        (next ? `<a class="next" href="${next.href}"><span>Next →</span>${next.title}</a>` : "<span></span>");
    }

    // ---- theme toggle ----
    const tbtn = document.getElementById("theme-toggle");
    const syncLabel = () => { if (tbtn) tbtn.textContent = root.getAttribute("data-theme") === "light" ? "☾ Dark" : "☀ Light"; };
    syncLabel();
    if (tbtn) tbtn.addEventListener("click", () => {
      const next = root.getAttribute("data-theme") === "light" ? "dark" : "light";
      root.setAttribute("data-theme", next);
      localStorage.setItem("sc-theme", next);
      syncLabel();
    });

    // ---- mobile nav ----
    const nbtn = document.getElementById("nav-toggle");
    if (nbtn) nbtn.addEventListener("click", () => document.body.classList.toggle("nav-open"));

    // ---- on-page TOC with scrollspy ----
    const toc = document.querySelector(".toc");
    const heads = [...document.querySelectorAll(".content h2, .content h3")];
    if (toc && heads.length) {
      let html = "<h5>On this page</h5>";
      heads.forEach((h, i) => {
        if (!h.id) h.id = h.textContent.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/(^-|-$)/g, "") || "s" + i;
        html += `<a class="${h.tagName.toLowerCase()}" href="#${h.id}">${h.textContent}</a>`;
      });
      toc.innerHTML = html;
      const links = [...toc.querySelectorAll("a")];
      const spy = () => {
        let cur = 0;
        heads.forEach((h, i) => { if (h.getBoundingClientRect().top < 120) cur = i; });
        links.forEach((a, i) => a.classList.toggle("active", i === cur));
      };
      document.addEventListener("scroll", spy, { passive: true });
      spy();
    }

    // ---- diagram tooltips: any SVG element with data-tip="Title|Body" ----
    const tip = document.createElement("div");
    tip.className = "tip";
    document.body.appendChild(tip);
    document.querySelectorAll("[data-tip]").forEach(el => {
      el.addEventListener("mouseenter", () => {
        const [t, b] = el.getAttribute("data-tip").split("|");
        tip.innerHTML = `<b>${t}</b>${b || ""}`;
        tip.classList.add("show");
      });
      el.addEventListener("mousemove", e => {
        const x = Math.min(e.clientX + 14, window.innerWidth - 320);
        tip.style.left = x + "px";
        tip.style.top = (e.clientY + 16) + "px";
      });
      el.addEventListener("mouseleave", () => tip.classList.remove("show"));
    });
  });
})();
