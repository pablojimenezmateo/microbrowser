#!/usr/bin/env bash
# Drive docs/roadmap-to-any-page.md one session at a time, each in a brand-new agent process.
#
# The point is the process boundary. A long conversation degrades: the context fills with the
# debris of work already committed, and the agent starts reasoning about its own transcript rather
# than about the repository. Every iteration here starts from zero and rebuilds its understanding
# from the only things that survive — the git log, docs/session-log.md, and
# docs/roadmap-sessions.json. If a session cannot hand off through those three files, it has not
# finished, and that is the constraint that keeps them honest.
#
#   tools/agent-loop.sh                 # one session, interactive, you approve each tool call
#   tools/agent-loop.sh -n 5            # five sessions back to back
#   tools/agent-loop.sh -n 5 --unattended   # ...with no approval prompts. Read the warning below.
#
# --unattended passes --dangerously-skip-permissions. It lets the agent run any command in this
# repository without asking, for hours, unwatched. That is the only way an overnight run works and
# it is a real decision: run it against a checkout you can throw away, on a branch you can delete,
# and read the transcripts afterwards.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
repo_root="$PWD"
ledger="docs/roadmap-sessions.json"
log_dir="${MICROBROWSER_AGENT_LOG_DIR:-/tmp/microbrowser-agent}"

sessions=1
unattended=0
timeout_s="${MICROBROWSER_AGENT_TIMEOUT:-14400}"   # 4h per session, then give up on it

while [[ $# -gt 0 ]]; do
  case "$1" in
    -n|--sessions)  sessions="$2"; shift 2 ;;
    --unattended)   unattended=1; shift ;;
    -t|--timeout)   timeout_s="$2"; shift 2 ;;
    -h|--help)      sed -n '2,20p' "${BASH_SOURCE[0]}" | cut -c3-; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

command -v claude >/dev/null || { echo "claude CLI not on PATH" >&2; exit 1; }
[[ -f "$ledger" ]] || { echo "no $ledger — run this from the microbrowser checkout" >&2; exit 1; }

mkdir -p "$log_dir"

# The number of sessions left. Parsed without jq so this works on a bare machine.
remaining() {
  grep -c '"status": "not_started"\|"status": "in_progress"' "$ledger" || true
}

# Uncommitted work belongs to whoever left it there — very possibly a parallel session. Refuse to
# start on top of it rather than let an agent decide what to do with someone else's files.
if [[ -n "$(git status --porcelain)" ]]; then
  echo "the working tree is dirty:" >&2
  git status --short >&2
  echo >&2
  echo "commit, stash or clean it first. An agent starting here cannot tell your files from its own." >&2
  exit 1
fi

branch="$(git rev-parse --abbrev-ref HEAD)"
echo "repo:     $repo_root"
echo "branch:   $branch"
echo "sessions: $sessions requested, $(remaining) unfinished in the ledger"
echo "logs:     $log_dir"
echo "mode:     $([[ $unattended == 1 ]] && echo 'unattended (no approval prompts)' || echo 'interactive')"
echo

claude_args=(--print --output-format stream-json --verbose)
[[ $unattended == 1 ]] && claude_args+=(--dangerously-skip-permissions)

for ((i = 1; i <= sessions; i++)); do
  left="$(remaining)"
  if [[ "$left" -eq 0 ]]; then
    echo "== the ledger has no unfinished sessions. Done."
    break
  fi

  stamp="$(date +%Y%m%d-%H%M%S)"
  transcript="$log_dir/session-$stamp.jsonl"
  before="$(git rev-parse HEAD)"

  echo "== iteration $i/$sessions  ($left unfinished)  -> $transcript"

  # A fresh `claude --print` is a fresh process with a fresh context. That is the whole mechanism.
  if ! timeout "$timeout_s" claude "${claude_args[@]}" "/next-session" >"$transcript" 2>&1; then
    status=$?
    echo "!! iteration $i exited $status (timeout is ${timeout_s}s). Transcript: $transcript" >&2
    echo "!! stopping. The tree is left exactly as the agent left it." >&2
    exit "$status"
  fi

  after="$(git rev-parse HEAD)"
  if [[ "$before" == "$after" ]]; then
    echo "!! iteration $i committed nothing. Either it is stuck or the ledger is wrong." >&2
    echo "!! read $transcript before running again." >&2
    exit 1
  fi

  echo "-- committed:"
  git log --oneline "$before..$after" | sed 's/^/     /'

  # An agent that leaves the tree dirty has broken the handoff for the next one. Say so loudly
  # rather than letting iteration i+1 inherit an ambiguous working tree.
  if [[ -n "$(git status --porcelain)" ]]; then
    echo "!! iteration $i left uncommitted files:" >&2
    git status --short >&2
    echo "!! stopping — the next iteration could not tell whose they are." >&2
    exit 1
  fi
  echo
done

echo "== $(remaining) sessions still unfinished. Transcripts in $log_dir."
