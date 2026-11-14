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
