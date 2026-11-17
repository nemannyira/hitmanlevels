// What every page of the site shares: the maps' names, dates, sizes, a few DOM helpers that put text in as
// text (never as HTML: the levels' texts are their authors'), the signed-in name in the bar.

var MAPS = {  // Hitman 2's maps (h2/src/editor/launcher.cpp kMaps)
  "C0-1": "The Gontranno Sanctuary", "C1-1": "Anathema", "C2-1": "St. Petersburg Stakeout",
  "C2-2": "Kirov Park Meeting", "C2-3": "Tubeway Torpedo", "C2-4": "Invitation to a Party",
  "C3-1": "Tracking Hayamoto", "C3-2a": "Hidden Valley", "C3-2b": "At the Gates", "C3-3": "Shogun Showdown",
  "C4-1": "Basement Killing", "C4-2": "The Graveyard Shift", "C4-3": "The Jacuzzi Job",
  "C5-1": "Murder at the Bazaar", "C5-2": "The Motorcade Interception", "C5-3": "Tunnel Rat",
  "C6-1": "Temple City Ambush", "C6-2": "The Death of Hannelore", "C6-3": "Terminal Hospitality",
  "C7-1": "St. Petersburg Revisited", "C8-1": "Redemption at Gontranno",
};

function mapName(scene) {
  var part = String(scene || "").split("\\")[0];
  return MAPS[part] || part || "a map of its own";
}

function day(seconds) {
  return new Date(seconds * 1000).toLocaleDateString("en-GB", { day: "numeric", month: "short", year: "numeric" });
}

function size(bytes) {
  return bytes >= 1048576 ? (bytes / 1048576).toFixed(1) + " MB" : Math.max(1, Math.round(bytes / 1024)) + " KB";
}

function percent(n, of) { return of ? Math.round(100 * n / of) + "%" : "-"; }

// game seconds as the statistics screen shows them (h:mm:ss, m:ss)
function clock(s) {
  var h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60, sec = String(s % 60).padStart(2, "0");
  return h ? h + ":" + String(m).padStart(2, "0") + ":" + sec : m + ":" + sec;
}

// the game's name of a rating as words ("SilentAssassin": "Silent Assassin")
function ratingName(key) { return String(key).replace(/([a-z])([A-Z])/g, "$1 $2"); }

// the badges (docs/badges.md): their names and what earns them
var BADGES = {
  finished: ["Finished", "Finished the level"],
  silent_assassin: ["Silent Assassin", "Rated Silent Assassin"],
  ghost: ["Ghost", "Finished with no alert, never spotted"],
  no_saves: ["No Saves", "Finished without saving"],
  clean_hands: ["Clean Hands", "Finished with no innocent killed or harmed"],
  no_shots: ["No Shots Fired", "Finished without firing a shot"],
  under_par: ["Under Par", "Finished in the par time: as fast as the quickest quarter of the finished runs"],
  first_to_finish: ["First to Finish", "The first to finish it"],
  board_top3: ["Board Top 3", "In the first three of its Silent Assassin board"],
  suit_only: ["Suit Only", "Finished without changing clothes"],
  precision: ["Precision", "Finished with only targets killed"],
  no_bodies: ["No Bodies Found", "Finished with no body found by anyone"],
  escalation_complete: ["Escalation Complete", "Finished every stage"],
  clean_sweep: ["Clean Sweep", "Silent Assassin on every stage"],
  finished_1: ["Finished 1", "Finished a level"],
  finished_10: ["Finished 10", "Finished 10 levels"],
  finished_50: ["Finished 50", "Finished 50 levels"],
  silent_assassin_5: ["Silent Assassin 5", "Silent Assassin on 5 levels"],
  silent_assassin_25: ["Silent Assassin 25", "Silent Assassin on 25 levels"],
  map_tourist_5: ["Map Tourist", "Finished levels on 5 of the game's maps"],
  map_tourist_10: ["Map Tourist 10", "Finished levels on 10 of the game's maps"],
  escalation_master: ["Escalation Master", "3 escalations complete"],
  verified_runner: ["Verified Runner", "A run on a verified board"],
  board_holder: ["Board Holder", "First on a level's Silent Assassin board"],
  first_level: ["First Level", "Uploaded a level"],
  played_by_10: ["Played by 10", "10 players played their levels"],
  played_by_100: ["Played by 100", "100 players played their levels"],
  tough_nut: ["Tough Nut", "A level 20 players played and fewer than a fifth finished"]
};

// a level's picture: the one its author uploaded on the site, else its package's (preview.png), else its map's
// (maps/<map>.jpg: a picture of the game's map, the same for every level on it)
function pictureOf(l) {
  if (l.cover) return l.cover;
  if (l.preview) return l.preview;
  var map = String(l.base_scene || "").split("\\")[0];
  return MAPS[map] ? "maps/" + map + ".jpg" : null;
}

// ---- charts (no library: HTML bars, SVG columns) ----

// rows [[label, value, colour]] as bars of the largest: "label  ████  value"
function barChart(rows, total) {
  var box = el("div", "bars"), max = Math.max.apply(null, rows.map(function (r) { return r[1]; }).concat([1]));
  rows.forEach(function (r) {
    var row = el("div", "bar-row"), track = el("div", "bar-track"), fill = el("div", "bar-fill");
    fill.style.width = (100 * r[1] / max) + "%";
    if (r[2]) fill.style.background = r[2];
    track.appendChild(fill);
    row.appendChild(el("span", "bar-label", r[0]));
    row.appendChild(track);
    row.appendChild(el("span", "bar-value", String(r[1]) + (total ? " · " + percent(r[1], total) : "")));
    box.appendChild(row);
  });
  return box;
}

// columns [[label, value]]: an SVG stretched to its box (no text in it: it scales), the first and last labels under
// it in HTML; `title`: each column's hover
function columnChart(cols, title) {
  var W = 600, H = 120, n = Math.max(cols.length, 1), w = W / n, gap = Math.min(2, w * 0.15);
  var max = Math.max.apply(null, cols.map(function (c) { return c[1]; }).concat([1]));
  var ns = "http://www.w3.org/2000/svg", s = document.createElementNS(ns, "svg");
  s.setAttribute("viewBox", "0 0 " + W + " " + H);
  s.setAttribute("preserveAspectRatio", "none");
  s.setAttribute("class", "columns");
  s.setAttribute("role", "img");
  cols.forEach(function (c, i) {
    var h = c[1] ? Math.max(2, (H - 2) * c[1] / max) : 0, r = document.createElementNS(ns, "rect");
    r.setAttribute("x", i * w + gap);
    r.setAttribute("width", Math.max(1, w - 2 * gap));
    r.setAttribute("y", H - h);
    r.setAttribute("height", h);
    var t = document.createElementNS(ns, "title");
    t.textContent = title(c);
    r.appendChild(t);
    s.appendChild(r);
  });
  var box = el("div"), axis = el("div", "axis");
  box.appendChild(s);
  if (cols.length) {
    axis.appendChild(el("span", null, cols[0][0]));
    axis.appendChild(el("span", null, "most: " + max));
    axis.appendChild(el("span", null, cols[cols.length - 1][0]));
  }
  box.appendChild(axis);
  return box;
}

// the finished runs' times in about 10 columns of whole minutes (or seconds for short levels)
function timesChart(times) {
  if (!times.length) return el("p", "muted", "No finished run yet.");
  var lo = Math.min.apply(null, times), hi = Math.max.apply(null, times);
  var steps = [5, 10, 15, 30, 60, 120, 300, 600, 900, 1800], step = steps[steps.length - 1];
  for (var i = 0; i < steps.length; i++) if ((hi - lo) / steps[i] <= 12) { step = steps[i]; break; }
  var from = Math.floor(lo / step) * step, cols = [];
  for (var t = from; t <= hi; t += step) cols.push([clock(t), times.filter(function (x) { return x >= t && x < t + step; }).length, t]);
  return columnChart(cols, function (c) { return c[0] + " - " + clock(c[2] + step) + ": " + c[1] + (c[1] === 1 ? " run" : " runs"); });
}

// the plays a day ([[day number, plays]]) over the last 30 days, the empty days too
function daysChart(days) {
  var today = Math.floor(Date.now() / 86400000), by = {}, cols = [];
  days.forEach(function (d) { by[d[0]] = d[1]; });
  for (var d = today - 29; d <= today; d++)
    cols.push([day(d * 86400).replace(/ \d{4}$/, ""), by[d] || 0]);
  return columnChart(cols, function (c) { return c[0] + ": " + c[1] + (c[1] === 1 ? " play" : " plays"); });
}

// a badge as a chip: its name, what earned it on hover; `stage`: an escalation's
function badgeChip(id, stage) {
  var b = BADGES[id] || [id, ""];
  var chip = el("span", "badge", b[0] + (stage ? " · stage " + stage : ""));
  chip.title = b[1];
  return chip;
}

// an element with text, its class, its children
function el(tag, cls, text) {
  var e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined && text !== null) e.textContent = text;
  return e;
}

// the key's fingerprint as the editor and levelpack show it: 16 hex digits of its SHA-256 in groups of four
async function fingerprint(keyHex) {
  var bytes = new Uint8Array(keyHex.match(/../g).map(function (h) { return parseInt(h, 16); }));
  var d = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  var hex = Array.from(d.slice(0, 8)).map(function (b) { return b.toString(16).padStart(2, "0"); }).join("").toUpperCase();
  return hex.match(/..../g).join("-");
}

// the bar's Sign in: the account's name once signed in
(async function () {
  var who = document.querySelector(".top .signin");
  if (!who) return;
  try {
    var r = await fetch("/api/me", { credentials: "same-origin" });
    if (r.status === 200) {
      who.textContent = (await r.json()).name;
      who.href = "upload.html";
    }
  } catch (e) { /* (signed out, or no API: the page as it is) */ }
})();
