// Badges (docs/badges.md): earned by a player from their ranked runs as they arrive (not retroactive), public. A
// level's (stage's) badges, an escalation's, a profile's and an author's; "played" rows count a level's players (the
// rarity), no badge of their own.

const now = () => Math.floor(Date.now() / 1000);
const MASTERY = [1, 3, 5, 7, 9];  // badges of a level for mastery 1..5

function give(env, user, badge, level = "", stage = 0, run = null) {
  return env.DB.prepare(
    "INSERT OR IGNORE INTO badges (user_id, badge, level_id, stage, run_id, earned) VALUES (?, ?, ?, ?, ?, ?)",
  ).bind(user, badge, level, stage, run, now());
}

const count = async (env, sql, ...args) => (await env.DB.prepare(sql).bind(...args).first()).n;

// a ranked run arrived (stored already): the badges it gives its player, their profile and the level's author
export async function awardRun(env, user, r) {
  const level = r.level, stage = r.stage || 0, c = r.counters;
  const ofStage = r.stage ? "AND stage = ?" : "AND stage IS NULL", at = r.stage ? [r.stage] : [];
  const earned = ["played"], profile = [];
  if (r.result === "completed") {
    earned.push("finished");
    if (r.rating === "SilentAssassin") earned.push("silent_assassin");
    if (c.alerts === 0 && !r.events.some((e) => e.kind === "spotted")) earned.push("ghost");
    if (c.saves === 0) earned.push("no_saves");
    if (c.innocents_killed === 0 && c.innocents_harmed === 0) earned.push("clean_hands");
    if (c.shots === 0) earned.push("no_shots");
    // the ones before it: the first; the par (their 25th percentile, from 10 of them)
    const before = `FROM runs WHERE level_id = ? ${ofStage} AND ranked = 1 AND result = 'completed' AND id != ?`;
    const n = await count(env, `SELECT COUNT(*) AS n ${before}`, level, ...at, r.run);
    if (!n) earned.push("first_to_finish");
    if (n >= 10) {
      const par = await env.DB.prepare(`SELECT time ${before} ORDER BY time LIMIT 1 OFFSET ?`)
        .bind(level, ...at, r.run, Math.floor((n - 1) / 4)).first();
      if (r.time <= par.time) earned.push("under_par");
    }
    if (r.rating === "SilentAssassin") {  // its place on the board (each player's fastest) as it arrives
      const { results } = await env.DB.prepare(
        `SELECT r.user_id, MIN(r.time) AS time FROM runs r JOIN users u ON u.id = r.user_id
          WHERE r.level_id = ? ${ofStage.replace("stage", "r.stage")} AND r.ranked = 1 AND r.result = 'completed'
            AND r.rating = 'SilentAssassin' AND u.blocked = 0 GROUP BY r.user_id ORDER BY time, r.user_id LIMIT 3`,
      ).bind(level, ...at).all();
      const place = results.findIndex((b) => b.user_id === user && b.time === r.time);
      if (place >= 0) earned.push("board_top3");
      if (place === 0) profile.push("board_holder");
    }
  }
  await env.DB.batch(earned.map((b) => give(env, user, b, level, stage, r.run)));

  // an escalation: every stage finished / Silent Assassin
  if (r.stage) {
    const v = await env.DB.prepare("SELECT stages FROM versions WHERE level_id = ? ORDER BY version DESC LIMIT 1").bind(level).first();
    const stages = v ? JSON.parse(v.stages || "[]").length : 0;
    const of = (b) => count(env, `SELECT COUNT(DISTINCT stage) AS n FROM badges WHERE user_id = ? AND level_id = ? AND badge = ? AND stage > 0`,
      user, level, b);
    const whole = [];
    if (stages && (await of("finished")) >= stages) whole.push("escalation_complete");
    if (stages && (await of("silent_assassin")) >= stages) whole.push("clean_sweep");
    if (whole.length) await env.DB.batch(whole.map((b) => give(env, user, b, level, 0, r.run)));
  }

  // the profile's, from the level badges
  const levels = (b) => count(env, "SELECT COUNT(DISTINCT level_id) AS n FROM badges WHERE user_id = ? AND badge = ?", user, b);
  const finished = await levels("finished"), sa = await levels("silent_assassin");
  const maps = await count(env, `SELECT COUNT(DISTINCT (SELECT v.base_scene FROM versions v WHERE v.level_id = b.level_id LIMIT 1)) AS n
                                   FROM badges b WHERE b.user_id = ? AND b.badge = 'finished'`, user);
  const escalations = await count(env, "SELECT COUNT(*) AS n FROM badges WHERE user_id = ? AND badge = 'escalation_complete'", user);
  [[finished, 1, "finished_1"], [finished, 10, "finished_10"], [finished, 50, "finished_50"], [sa, 5, "silent_assassin_5"],
    [sa, 25, "silent_assassin_25"], [maps, 5, "map_tourist_5"], [maps, 10, "map_tourist_10"], [escalations, 3, "escalation_master"]]
    .forEach(([n, from, b]) => { if (n >= from) profile.push(b); });
  if (profile.length) await env.DB.batch(profile.map((b) => give(env, user, b, "", 0, r.run)));

  // the level's author: players of their levels; a tough level
  const author = await env.DB.prepare(
    "SELECT a.user_id FROM levels l JOIN authors a ON a.key = l.author_key WHERE l.id = ?").bind(level).first();
  if (!author || !author.user_id || author.user_id === user) return;
  const players = await count(env, `SELECT COUNT(DISTINCT b.user_id) AS n FROM badges b JOIN levels l ON l.id = b.level_id
    JOIN authors a ON a.key = l.author_key WHERE a.user_id = ? AND b.badge = 'played' AND b.user_id != ?`, author.user_id, author.user_id);
  const here = await count(env, "SELECT COUNT(DISTINCT user_id) AS n FROM badges WHERE level_id = ? AND badge = 'played' AND user_id != ?",
    level, author.user_id);
  const finishedHere = await count(env,
    "SELECT COUNT(DISTINCT user_id) AS n FROM badges WHERE level_id = ? AND badge = 'finished' AND user_id != ?", level, author.user_id);
  const theirs = [];
  if (players >= 10) theirs.push(give(env, author.user_id, "played_by_10"));
  if (players >= 100) theirs.push(give(env, author.user_id, "played_by_100"));
  if (here >= 20 && finishedHere * 5 < here) theirs.push(give(env, author.user_id, "tough_nut", level));
  if (theirs.length) await env.DB.batch(theirs);
}

// a profile badge given by something else than a run (an upload, a verified video)
export async function awardProfile(env, user, badge) {
  if (user) await give(env, user, badge).run();
}

// a player's badges for their page: the profile's, and each level's with its mastery (1..5)
export async function playerBadges(env, user) {
  const { results } = await env.DB.prepare(
    `SELECT b.badge, b.level_id, b.stage, b.earned,
            (SELECT title FROM versions v WHERE v.level_id = b.level_id ORDER BY version DESC LIMIT 1) AS title
       FROM badges b LEFT JOIN levels l ON l.id = b.level_id
      WHERE b.user_id = ? AND b.badge != 'played' AND (b.level_id = '' OR l.taken_down = 0)
      ORDER BY b.earned, b.badge`).bind(user).all();
  const profile = [], levels = new Map();
  for (const b of results) {
    if (!b.level_id || b.badge === "tough_nut") { profile.push(b); continue; }
    if (!levels.has(b.level_id)) levels.set(b.level_id, { id: b.level_id, title: b.title, badges: [] });
    levels.get(b.level_id).badges.push({ badge: b.badge, stage: b.stage || null });
  }
  for (const l of levels.values())
    l.mastery = MASTERY.filter((n) => new Set(l.badges.map((b) => b.badge)).size >= n).length;
  return { profile, levels: [...levels.values()] };
}

// a level's badges for its page: how many of its players have each (every stage together, and per stage)
export async function levelBadges(env, level) {
  const [all, byStage] = await env.DB.batch([
    env.DB.prepare(`SELECT badge, COUNT(DISTINCT user_id) AS n FROM badges WHERE level_id = ? AND badge != 'tough_nut'
                     GROUP BY badge`).bind(level),
    env.DB.prepare(`SELECT stage, badge, COUNT(*) AS n FROM badges WHERE level_id = ? AND stage > 0 GROUP BY stage, badge`).bind(level),
  ]);
  const stages = {};
  for (const s of byStage.results) (stages[s.stage] ||= {})[s.badge] = s.n;
  return { all: Object.fromEntries(all.results.map((b) => [b.badge, b.n])), by_stage: stages };
}
