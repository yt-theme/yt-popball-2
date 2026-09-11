#!/usr/bin/env bash
#
# popball2 —— 一键同时推送到 Gitee 与 GitHub
#
#   本脚本只负责「推送」，不执行 git commit / git add。
#   提交请先用 git 命令完成，例如:
#       git add -A
#       git commit -m "你的提交说明"
#       ./push.sh
#
#   • Gitee : origin   (https://gitee.com/kettle_download/yt-popball-2.git)
#   • GitHub: github   (https://github.com/yt-theme/yt-popball-2.git)
#
# 用法:
#   ./push.sh                  推送当前分支到两个远端
#   ./push.sh <branch>         推送指定分支到两个远端
#   ./push.sh --dry-run        预览将要推送的目标（不实际推送）
#   ./push.sh -h               显示帮助
#
set -euo pipefail

GITEE_URL="https://gitee.com/kettle_download/yt-popball-2.git"
GITHUB_URL="https://github.com/yt-theme/yt-popball-2.git"

# ---------------------------------------------------------------- 输出样式
if [ -t 1 ]; then
    C_R=$'\033[0m'; C_B=$'\033[1m'; C_G=$'\033[32m'; C_Y=$'\033[33m'; C_RD=$'\033[31m'; C_C=$'\033[36m'
else
    C_R=; C_B=; C_G=; C_Y=; C_RD=; C_C=
fi
step() { printf '\n%s==> %s%s\n' "$C_B$C_C" "$*" "$C_R"; }
ok()   { printf '    %s✓%s %s\n' "$C_G" "$C_R" "$*"; }
warn() { printf '    %s!%s %s\n' "$C_Y" "$C_R" "$*"; }
die()  { printf '    %s✗ %s%s\n' "$C_RD" "$*" "$C_R" >&2; exit 1; }
info() { printf '    %s\n' "$*"; }

# ---------------------------------------------------------------- 参数解析
DRY=0; BRANCH_ARG=""
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY=1 ;;
        -h|--help) sed -n '3,16p' "$0"; exit 0 ;;
        -*)        die "未知选项: $1" ;;
        *)         [ -z "$BRANCH_ARG" ] && BRANCH_ARG="$1" || die "分支只能指定一次" ;;
    esac
    shift
done

# ---------------------------------------------------------------- 前置检查
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || die "当前目录不是 git 仓库"
REPO_ROOT="$(git rev-parse --show-toplevel)"

# 确保两个远端都存在（origin=Gitee, github=GitHub），URL 不符则修正
ensure_remote() {
    local name="$1" url="$2"
    if git remote get-url "$name" >/dev/null 2>&1; then
        local cur; cur="$(git remote get-url "$name")"
        if [ "$cur" != "$url" ]; then
            git remote set-url "$name" "$url"
            warn "远端 $name 的 URL 已更新为 ${url}（原为: ${cur}）"
        fi
    else
        git remote add "$name" "$url"
        ok "已添加远端 $name -> $url"
    fi
}
ensure_remote origin "$GITEE_URL"
ensure_remote github "$GITHUB_URL"

# 确定要推送的分支
if [ -n "$BRANCH_ARG" ]; then
    git show-ref --verify --quiet "refs/heads/$BRANCH_ARG" \
        || die "本地不存在分支: $BRANCH_ARG"
    BRANCH="$BRANCH_ARG"
else
    BRANCH="$(git rev-parse --abbrev-ref HEAD)"
fi

# 检查本地分支是否有「尚未推送」的提交（避免推送空内容）
LOCAL="$(git rev-parse "$BRANCH")"
UPSTREAM="$(git rev-parse --abbrev-ref "$BRANCH@{upstream}" 2>/dev/null || true)"
if [ -n "$UPSTREAM" ] && [ "$(git rev-parse "$UPSTREAM")" = "$LOCAL" ]; then
    warn "分支 $BRANCH 没有需要推送的新提交（远端已是最新）"
elif [ -z "$(git log "$BRANCH" -1 --format='%H' 2>/dev/null)" ]; then
    die "分支 $BRANCH 没有任何提交，无法推送"
fi

info "当前分支: $BRANCH"
info "Gitee : origin ($GITEE_URL)"
info "GitHub: github ($GITHUB_URL)"

# ---------------------------------------------------------------- 推送（一个失败不阻断另一个）
push_to() {
    local name="$1"
    if [ "$DRY" -eq 1 ]; then info "[dry-run] 将推送 $BRANCH 到 $name"; return 0; fi
    if git push -u "$name" "$BRANCH" 2>&1; then
        ok "已推送到 $name ($BRANCH)"
    else
        warn "推送到 $name 失败（检查网络 / 权限 / 凭据），其它远端不受影响"
    fi
}
step "推送（Gitee + GitHub）"
push_to origin
push_to github

printf '\n%s完成。%s\n' "$C_G" "Gitee: $GITEE_URL    GitHub: $GITHUB_URL"
