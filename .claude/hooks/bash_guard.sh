#!/usr/bin/env bash
# PreToolUse(Bash): enforce devbox for build/test, CHANGELOG.md on code commits, ASCII commit messages.
cmd=$(jq -r '.tool_input.command // empty')
deny() {
  jq -n --arg r "$1" '{hookSpecificOutput:{hookEventName:"PreToolUse",permissionDecision:"deny",permissionDecisionReason:$r}}'
  exit 0
}
if grep -Eq '(scripts/compile[a-z_]*\.sh|test/run_tests\.sh|test/test_l2\.sh)' <<<"$cmd" \
   && ! grep -Eq 'devbox run' <<<"$cmd"; then
  deny "Build/test scripts must run via: devbox run -- <cmd>"
fi
if grep -Eq '(^|[;&|[:space:]])git[[:space:]]+commit' <<<"$cmd"; then
  if grep -qP '[\x{1F000}-\x{1FAFF}\x{2600}-\x{27BF}\x{2010}-\x{2015}\x{2018}\x{2019}\x{201C}\x{201D}\x{2022}\x{2026}\x{2190}-\x{21FF}\x{00A0}\x{200B}-\x{200D}\x{FEFF}]' <<<"$cmd"; then
    deny "Commit message contains emoji or AI typography (em/en dash, curly quotes, ellipsis, arrows, bullets). Use ASCII."
  fi
  root=$(git rev-parse --show-toplevel 2>/dev/null) || exit 0
  staged=$(git -C "$root" diff --cached --name-only)
  if grep -q '^-a\b\|[[:space:]]-a[[:space:]]\|--all' <<<"$cmd"; then
    staged=$(printf '%s\n%s' "$staged" "$(git -C "$root" diff --name-only)")
  fi
  if grep -Eq '^(src|apps|scripts|test)/' <<<"$staged" && ! grep -qx 'CHANGELOG.md' <<<"$staged"; then
    deny "Code changes staged without CHANGELOG.md. Update CHANGELOG.md (or confirm with the user that this commit changes no behavior)."
  fi
fi
exit 0
