// The badges' rules (src/badges.js, docs/badges.md) on an in-memory database made by the site's own migrations
// (node:sqlite behind a D1-like adapter): a level's badges, the par, the board, an escalation's, a profile's, an
// author's, the player's and the level's views.
//   node test/badges_test.mjs
import { DatabaseSync } from "node:sqlite";
import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { awardProfile, awardRun, levelBadges, playerBadges } from "../src/badges.js";

let failed = 0;
const check = (name, ok, detail = "") => { console.log(`${ok ? "PASS" : "FAIL"} ${name} ${detail}`); if (!ok) failed++; };

const db = new DatabaseSync(":memory:");
const dir = join(import.meta.dirname, "..", "migrations");
for (const f of readdirSync(dir).filter((f) => f.endsWith(".sql")).sort()) db.exec(readFileSync(join(dir, f), "utf8"));
const statement = (sql, args = []) => ({
  bind: (...a) => statement(sql, a),
  first: async () => db.prepare(sql).get(...args) ?? null,
  all: async () => ({ results: db.prepare(sql).all(...args) }),
  run: async () => ({ meta: { changes: Number(db.prepare(sql).run(...args).changes) } }),
  // (in a batch: a query's rows, a change's count)
  step: () => /^\s*SELECT/i.test(sql) ? { results: db.prepare(sql).all(...args) }
                                      : { meta: { changes: Number(db.prepare(sql).run(...args).changes) } },
});
const env = { DB: { prepare: (sql) => statement(sql), batch: async (list) => list.map((s) => s.step()) } };

// accounts 1..30 (1: the author), levels of the author on maps, an escalation of 3 stages
for (let i = 1; i <= 30; i++) db.prepare("INSERT INTO users (id, provider, provider_id, name, created) VALUES (?, 'github', ?, ?, 0)").run(i, "p" + i, "P" + i);
db.prepare("INSERT INTO authors (key, user_id) VALUES ('k1', 1)").run();
const level = (id, scene, stages = 0) => {
  db.prepare("INSERT INTO levels (id, game, author_key) VALUES (?, 'h2', 'k1')").run(id);
  db.prepare(`INSERT INTO versions (level_id, version, title, base_scene, license, author_name, path, size, sha256, uploaded_by,
              created, stages) VALUES (?, 1, ?, ?, 'CC BY 4.0', 'A', 'x', 1, 'h', 1, 0, ?)`)
    .run(id, "Level " + id, scene, JSON.stringify(Array.from({ length: stages }, (_, i) => ({ name: "S" + (i + 1) }))));
};
level("lvl00001", "C1-1\\C1-1__MAIN");
["C2-1", "C3-1", "C4-1", "C5-1", "C6-1"].forEach((m, i) => level("map0000" + i, `${m}\\${m}__MAIN`));
level("esc00001", "C1-1\\C1-1__MAIN", 3);

let n = 0;
const zero = { alerts: 0, close_encounters: 0, enemies_harmed: 0, enemies_killed: 0, headshots: 0, innocents_harmed: 0,
  innocents_killed: 0, saves: 0, shots: 0 };
async function play(user, lvl, { result = "completed", time = 600, rating = null, counters = {}, events = [], stage } = {}) {
  const r = { run: (++n).toString(16).padStart(32, "0"), level: lvl, result, time, rating: result === "completed" ? rating : null,
    counters: { ...zero, ...counters }, events, ...(stage && { stage }) };
  db.prepare(`INSERT INTO runs (id, level_id, version, received, result, time, rating, counters, stage, events, dll, user_id, ranked)
              VALUES (?, ?, 1, ?, ?, ?, ?, '{}', ?, '[]', '0', ?, 1)`).run(r.run, lvl, n, result, time, r.rating, stage ?? null, user);
  await awardRun(env, user, r);
}
const has = (user, lvl = "", stage = 0) => db.prepare(
  "SELECT badge FROM badges WHERE user_id = ? AND level_id = ? AND stage = ? ORDER BY badge").all(user, lvl, stage).map((b) => b.badge).join(",");

// 1. a level's badges
await play(2, "lvl00001", { time: 300, rating: "SilentAssassin" });
check("a clean Silent Assassin run: every badge it earns, the first, top of the board", has(2, "lvl00001") ===
  "board_top3,clean_hands,finished,first_to_finish,ghost,no_saves,no_shots,played,silent_assassin", has(2, "lvl00001"));
check("... the profile: a level finished, the board's first", has(2) === "board_holder,finished_1", has(2));
await play(3, "lvl00001", { result: "failed", time: 100, counters: { alerts: 3 } });
check("a failed run: played only", has(3, "lvl00001") === "played", has(3, "lvl00001"));
await play(3, "lvl00001", { time: 900, rating: "Professional", counters: { alerts: 2, saves: 1, shots: 5, innocents_harmed: 1 },
  events: [{ kind: "spotted", t: 10 }] });
check("a loud finish: Finished only (not the first any more)", has(3, "lvl00001") === "finished,played", has(3, "lvl00001"));
await play(4, "lvl00001", { time: 700, counters: { alerts: 0 }, events: [{ kind: "spotted", t: 5 }] });
check("spotted with no alert: not Ghost", !has(4, "lvl00001").includes("ghost"), has(4, "lvl00001"));
await play(5, "lvl00001", { time: 250, rating: "SilentAssassin" });
check("a faster Silent Assassin: first on the board", has(5, "lvl00001").includes("board_top3") && has(5).includes("board_holder"));
check("... the one before keeps what it earned", has(2).includes("board_holder") && has(2, "lvl00001").includes("board_top3"));

// the par: from 10 finished runs before, their 25th percentile
for (let u = 6; u <= 12; u++) await play(u, "lvl00001", { time: 1000 + u * 10 });
check("no par before 10 finished runs", !db.prepare("SELECT 1 FROM badges WHERE badge = 'under_par'").get());
// before: 300, 900, 700, 250, 1060..1120 (11 runs): sorted 250 300 700 900 1060 ...; offset (11-1)/4 = 2: par 700
await play(13, "lvl00001", { time: 700, counters: { shots: 1 } });
await play(14, "lvl00001", { time: 701, counters: { shots: 1 } });
check("Under Par: at the par time", has(13, "lvl00001").includes("under_par"), has(13, "lvl00001"));
check("... not a second over it", !has(14, "lvl00001").includes("under_par"), has(14, "lvl00001"));

// 2. an escalation: every stage finished, every stage Silent Assassin
await play(2, "esc00001", { stage: 1, rating: "SilentAssassin" });
await play(2, "esc00001", { stage: 2, rating: "SilentAssassin" });
check("two of three stages: not complete yet", !has(2, "esc00001").includes("escalation_complete"), has(2, "esc00001"));
await play(2, "esc00001", { stage: 3, rating: "SilentAssassin" });
check("the third: Escalation Complete and Clean Sweep", has(2, "esc00001") === "clean_sweep,escalation_complete", has(2, "esc00001"));
check("... the stages' own badges per stage", has(2, "esc00001", 3).includes("silent_assassin") && has(2, "esc00001", 1).includes("first_to_finish"));
await play(3, "esc00001", { stage: 1, rating: "SilentAssassin" });
await play(3, "esc00001", { stage: 2 });
await play(3, "esc00001", { stage: 3, rating: "SilentAssassin" });
check("every stage finished, one not Silent Assassin: complete, no Clean Sweep", has(3, "esc00001") === "escalation_complete", has(3, "esc00001"));

// 3. the profile's: maps
for (let i = 0; i < 3; i++) await play(2, "map0000" + i);
check("4 maps finished: no Map Tourist", !has(2).includes("map_tourist_5"), has(2));
await play(2, "map00003");
check("5 maps: Map Tourist", has(2).includes("map_tourist_5"), has(2));
await awardProfile(env, 2, "verified_runner");
check("a profile badge given by something else", has(2).includes("verified_runner"));

// 4. the author's: players of their levels, a tough level (20 players, under a fifth finished)
check("the author: 13 players of their levels: Played by 10", has(1).includes("played_by_10") && !has(1).includes("played_by_100"), has(1));
await play(1, "lvl00001", { time: 100 });
check("... the author's own run counts for nobody's author badges", !has(1, "lvl00001").includes("tough_nut"));
for (let u = 14; u <= 30; u++) await play(u, "map00004", { result: "failed" });
await play(2, "map00004", { result: "failed" });
await play(3, "map00004", { result: "failed" });
check("19 players, none finished: not yet Tough Nut", !has(1, "map00004").includes("tough_nut"));
await play(4, "map00004", { time: 500 });
check("20 players, one finished (5 %): Tough Nut", has(1, "map00004").includes("tough_nut"), has(1, "map00004"));

// the views
const p2 = await playerBadges(env, 2);
const l1 = p2.levels.find((l) => l.id === "lvl00001");
check("the player's page: a level's badges and mastery (8 badges: 4)", l1 && l1.badges.length === 8 && l1.mastery === 4 &&
  !l1.badges.some((b) => b.badge === "played"), JSON.stringify(l1));
check("... the escalation's: its stages' badges with their stage", p2.levels.find((l) => l.id === "esc00001").badges.some((b) => b.stage === 3));
check("... the profile's, in the order earned", p2.profile[0].badge === "board_holder" && p2.profile.length >= 4, p2.profile.map((b) => b.badge).join());
check("... an author's Tough Nut names its level", (await playerBadges(env, 1)).profile.some((b) => b.badge === "tough_nut" && b.title === "Level map00004"));
db.prepare("UPDATE levels SET taken_down = 1 WHERE id = 'map00000'").run();
check("... a level taken down: its badges not shown", !(await playerBadges(env, 2)).levels.some((l) => l.id === "map00000"));
const lb = await levelBadges(env, "lvl00001");
check("the level's page: players and each badge's (the author's own run too)", lb.all.played === 14 && lb.all.finished === 14 && lb.all.silent_assassin === 2, JSON.stringify(lb.all));
const eb = await levelBadges(env, "esc00001");
check("... an escalation's per stage", eb.by_stage[2].played === 2 && eb.by_stage[2].silent_assassin === 1 && eb.all.clean_sweep === 1,
  JSON.stringify(eb.by_stage));

console.log(failed ? `\n${failed} failed` : "\nall passed");
process.exit(failed ? 1 : 0);
