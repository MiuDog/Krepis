#!/usr/bin/env node
// readback.mjs — 交付前確定性自查：檔案存在、非空、markdown 相對連結可解析。
// 用法：node readback.mjs <檔案或目錄> [...]     exit 0=全過，1=有問題（逐條列出）
import { readFileSync, existsSync, statSync, readdirSync } from "node:fs";
import { join, dirname, resolve } from "node:path";

const targets = process.argv.slice(2);
if (!targets.length) { console.error("用法：node readback.mjs <檔案或目錄> [...]"); process.exit(1); }

const problems = [];
function* walk(p) {
  const st = statSync(p);
  if (st.isDirectory()) { for (const e of readdirSync(p)) if (![".git", "node_modules"].includes(e)) yield* walk(join(p, e)); }
  else yield p;
}
for (const t of targets) {
  if (!existsSync(t)) { problems.push(`不存在：${t}`); continue; }
  for (const f of walk(resolve(t))) {
    if (statSync(f).size === 0) { problems.push(`空檔案：${f}`); continue; }
    if (!/\.md$/i.test(f)) continue;
    const text = readFileSync(f, "utf8");
    for (const m of text.matchAll(/\[[^\]]*\]\(([^)]+)\)/g)) {
      const link = m[1].split("#")[0].trim();
      if (!link || /^(https?|mailto):/.test(link) || link.includes("<")) continue;
      if (!existsSync(resolve(dirname(f), link))) problems.push(`${f}: 壞連結 → ${link}`);
    }
  }
}
if (problems.length) { problems.forEach((p) => console.error("FAIL:", p)); process.exit(1); }
console.log(`read-back 通過（${targets.length} 個目標）`);
