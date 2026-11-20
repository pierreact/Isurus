#!/usr/bin/env bash
# PreToolUse(Write|Edit): flag emojis and typical AI typography in newly written text.
in=$(cat)
f=$(jq -r '.tool_input.file_path // empty' <<<"$in")
txt=$(jq -r '.tool_input.content // .tool_input.new_string // empty' <<<"$in")
hits=$(grep -nP '[\x{1F000}-\x{1FAFF}\x{2600}-\x{27BF}\x{2010}-\x{2015}\x{2018}\x{2019}\x{201C}\x{201D}\x{2022}\x{2026}\x{2190}-\x{21FF}\x{00A0}\x{200B}-\x{200D}\x{FEFF}]' <<<"$txt" | head -5)
if [ -n "$hits" ]; then
  echo "Disallowed characters in edit to $f (emoji, em/en dash, curly quotes, ellipsis, arrows, bullets, nbsp, zero-width). Use ASCII: - -- ' \" ... -> *:" >&2
  echo "$hits" >&2
  exit 2
fi
exit 0
