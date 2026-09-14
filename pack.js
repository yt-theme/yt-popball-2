#!/usr/bin/env node
'use strict';

// PopBall 打包的 Node 友好入口。
// 本脚本只是 package.sh 的薄封装：把参数原样转发给 package.sh，
// 由它完成「本机编译 + Docker 多架构打包(x86_64 + arm64) + 容错」。
// 这样既能 `node pack.js <目标>`，也能 `npm run <目标>`，且打包逻辑只有一份实现。

const { spawnSync } = require('child_process');
const path = require('path');
const fs = require('fs');

const ROOT = __dirname;
const SCRIPT = path.join(ROOT, 'package.sh');

// 找一个能跑 package.sh 的 bash（macOS/Linux 自带；Windows 需 WSL / Git Bash）
function findBash() {
  const candidates = ['bash', '/bin/bash', '/usr/bin/bash'];
  for (const c of candidates) {
    try {
      const r = spawnSync(c, ['--version'], { stdio: 'ignore' });
      if (r.status === 0) return c;
    } catch (_) {
      /* ignore */
    }
  }
  return null;
}

function printHelp() {
  console.log(`\
PopBall 打包（Node 友好入口）

本脚本把参数原样转发给 package.sh，由它完成
「本机编译 + Docker 多架构打包(x86_64 + arm64) + 容错」。
所有参数与 package.sh 完全一致，详见： bash ./package.sh --help

用法:
  node pack.js [目标...] [选项]
  npm run <目标>            # 见 package.json 的 scripts

常用 npm 脚本:
  npm run mac               # macOS 本机 dmg + zip
  npm run deb               # Linux deb（默认 x86_64 + arm64，经 Docker）
  npm run deb:x64           # 只要 x86_64 的 .deb
  npm run deb:arm           # 只要 arm64 的 .deb
  npm run rpm               # Linux rpm（仅 Linux 本机）
  npm run appimage          # Linux AppImage（仅 Linux 本机）
  npm run all               # 当前平台支持的全部

示例:
  node pack.js deb --linux-arch arm
  node pack.js mac --version 1.2.0
  node pack.js deb --no-docker
`);
}

const args = process.argv.slice(2);

// 无参数时只打印帮助（退出码 1，提示用户给了目标）
if (args.length === 0) {
  printHelp();
  process.exit(1);
}

const bash = findBash();
if (!bash) {
  console.error(
    '错误: 找不到 bash。package.sh 需要 bash 才能运行（macOS / Linux 自带；Windows 请用 WSL 或 Git Bash）。'
  );
  process.exit(1);
}

if (!fs.existsSync(SCRIPT)) {
  console.error(`错误: 找不到 ${SCRIPT}（请确认 pack.js 与 package.sh 在同一目录）`);
  process.exit(1);
}

const res = spawnSync(bash, [SCRIPT, ...args], { stdio: 'inherit', cwd: ROOT });
// spawnSync 失败（如 bash 崩溃）时 res.status 为 null，统一映射为 1
process.exit(res.status === null ? 1 : res.status);
