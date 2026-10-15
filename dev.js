#!/usr/bin/env node
'use strict';

// npm run dev —— 编译 debug 版并直接运行。
// 本脚本只是 run.sh 的薄封装：把参数原样转发给 run.sh --debug -y，
// 由它完成「依赖检查 → debug 构建（CONFIG+=debug）→ 运行」，逻辑只有一份实现。
//   node dev.js               # debug 构建 + 运行（默认构建目录 build-debug）
//   node dev.js --no-run      # 只编译，不运行
//   node dev.js --clean       # 先清空 build-debug 再编译
//   node dev.js -b /tmp/bd    # 指定构建目录
// 其余参数与 ./run.sh --help 一致。

const { spawnSync } = require('child_process');
const path = require('path');
const fs = require('fs');

const ROOT = __dirname;
const SCRIPT = path.join(ROOT, 'run.sh');

// 找一个能跑 run.sh 的 bash（macOS/Linux 自带；Windows 需 WSL / Git Bash）
function findBash() {
  const candidates = ['bash', '/bin/bash', '/usr/bin/bash'];
  // Windows：Git Bash 通常不把 bash.exe 放进 PATH（且 PATH 里的 bash 可能是 WSL 的存根，
  // 用 WSL 跑会因路径/换行差异踩坑），这里直接按常见安装位置找 Git 自带的 bash.exe。
  if (process.platform === 'win32') {
    const pf = process.env['ProgramFiles'] || 'C:\\Program Files';
    const pf86 = process.env['ProgramFiles(x86)'] || 'C:\\Program Files (x86)';
    const local = process.env['LOCALAPPDATA'] || '';
    candidates.push(
      path.join(pf, 'Git', 'bin', 'bash.exe'),
      path.join(pf86, 'Git', 'bin', 'bash.exe'),
      local ? path.join(local, 'Programs', 'Git', 'bin', 'bash.exe') : ''
    );
    // MSYS2 也常被用来跑这个脚本
    candidates.push('C:\\msys64\\usr\\bin\\bash.exe');
  }
  for (const c of candidates) {
    if (!c) continue;
    try {
      const r = spawnSync(c, ['--version'], { stdio: 'ignore' });
      if (r.status === 0) return c;
    } catch (_) {
      /* ignore */
    }
  }
  return null;
}

const bash = findBash();
if (!bash) {
  console.error(
    '错误: 找不到 bash。run.sh 需要 bash 才能运行（macOS / Linux 自带；Windows 请用 WSL 或 Git Bash）。'
  );
  process.exit(1);
}

if (!fs.existsSync(SCRIPT)) {
  console.error(`错误: 找不到 ${SCRIPT}（请确认 dev.js 与 run.sh 在同一目录）`);
  process.exit(1);
}

const args = process.argv.slice(2);
const res = spawnSync(bash, [SCRIPT, '--debug', '-y', ...args], { stdio: 'inherit', cwd: ROOT });
// spawnSync 失败（如 bash 崩溃）时 res.status 为 null，统一映射为 1
process.exit(res.status === null ? 1 : res.status);
