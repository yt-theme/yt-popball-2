#!/usr/bin/env bash
#
# popball2 —— 一键提交并同时推送到 Gitee 与 GitHub
#
#   • Gitee : origin   (https://gitee.com/kettle_download/yt-popball-2.git)
#   • GitHub: github   (https://github.com/yt-theme/yt-popball-2.git)
#
# 用法:
#   ./push.sh "提交说明"            提交并推送到两个远端（当前分支）
#   ./push.sh -m "提交说明"         同上
#   ./push.sh "说明" --no-push      只提交，不推送
#   ./push.sh "说明" --dry-run      预览将要做的操作，不实际提交/推送
#   ./push.sh -h                    显示帮助
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
NO_PUSH=0; DRY=0; MSG=""
while [ $# -gt 0 ]; do
    case "$1" in
        -m)        MSG="${2:?需要提供提交说明}"; shift ;;
        --no-push) NO_PUSH=1 ;;
        --dry-run) DRY=1 ;;
        -h|--help) sed -n '3,11p' "$0"; exit 0 ;;
        -*)        die "未知选项: $1" ;;
        *)         [ -z "$MSG" ] && MSG="$1" || die "提交说明只需提供一次" ;;
    esac
    shift
done
[ -n "$MSG" ] || die "请提供提交说明，例如: ./push.sh \"修复网速计算\""

# ---------------------------------------------------------------- 前置检查
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || die "当前目录不是 git 仓库"
REPO_ROOT="$(git rev-parse --show-toplevel)"

# .workbuddy/ 是项目内部工作记忆，不应进入公开仓库
if ! grep -qxF '.workbuddy/' "$REPO_ROOT/.gitignore" 2>/dev/null; then
    printf '\n# 项目内部工作记忆（不公开）\n.workbuddy/\n' >> "$REPO_ROOT/.gitignore"
    ok "已将 .workbuddy/ 加入 .gitignore（避免内部记忆被提交）"
fi

# 确保两个远端都存在（origin=Gitee, github=GitHub），URL 不符则修正
ensure_remote() {
    local name="$1" url="$2"
    if git remote get-url "$name" >/dev/null 2>&1; then
        local cur; cur="$(git remote get-url "$name")"
        if [ "$cur" != "$url" ]; then
            git remote set-url "$name" "$url"
            warn "远端 $name 的 URL 已更新为 $url（原为: $cur）"
        fi
    else
        git remote add "$name" "$url"
        ok "已添加远端 $name -> $url"
    fi
}
ensure_remote origin "$GITEE_URL"
ensure_remote github "$GITHUB_URL"

BRANCH="$(git rev-parse --abbrev-ref HEAD)"
info "当前分支: $BRANCH"
info "Gitee : origin ($GITEE_URL)"
info "GitHub: github ($GITHUB_URL)"

# ---------------------------------------------------------------- 提交
if [ -z "$(git status --porcelain)" ]; then
    warn "工作区没有改动，无需提交"
    [ "$NO_PUSH" -eq 1 ] && exit 0
else
    step "暂存改动"
    git add -A
    if [ "$DRY" -eq 1 ]; then
        info "[dry-run] 将提交以下文件:"; git status --short
    else
        git commit -m "$MSG"
        ok "已提交: $MSG"
    fi
fi

[ "$NO_PUSH" -eq 1 ] && { ok "已跳过推送（--no-push）"; exit 0; }

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
