#!/usr/bin/env bash
# Stop: block a turn whose assistant text contains emoji or AI typography; forces an ASCII restatement.
in=$(cat)
[ "$(jq -r '.stop_hook_active // false' <<<"$in")" = "true" ] && exit 0
t=$(jq -r '.transcript_path // empty' <<<"$in")
txt=$(jq -r '.last_assistant_message // empty' <<<"$in")
if [ -f "$t" ]; then
  txt+=$'\n'$(jq -rs '
    (to_entries | map(select(.value.type=="user" and (.value.message.content|type)=="string")) | last.key // -1) as $u
    | .[$u+1:][] | select(.type=="assistant") | .message.content[]? | select(.type=="text") | .text' "$t")
fi
hits=$(grep -nP '[\x{1F000}-\x{1FAFF}\x{2600}-\x{27BF}\x{2010}-\x{2015}\x{2018}\x{2019}\x{201C}\x{201D}\x{2022}\x{2026}\x{2190}-\x{21FF}\x{00A0}\x{200B}-\x{200D}\x{FEFF}]' <<<"$txt" | head -5)
if [ -n "$hits" ]; then
  jq -n --arg h "$hits" '{decision:"block",reason:("Reply contained emoji or AI typography (em/en dash, curly quotes, ellipsis, arrows, bullets). Restate the affected text in plain ASCII. Offending lines:\n"+$h)}'
fi
exit 0
