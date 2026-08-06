#!/usr/bin/env bash
# =============================================================================
#  Shared helpers for the mandatory experiments
#  Embedded Systems final project -- Mahan Majlesi (402170516)
#
#  Every experiment writes into results/<id>/ and leaves behind:
#
#      cmd.log      the exact commands run, with their output
#      *.csv        sampled data, ready to plot
#      result.md    a short verdict block that the report quotes directly
#
#  Keeping the raw command transcript matters: the brief asks for the test
#  number and the result in the report, and a transcript is what makes a claim
#  checkable rather than asserted.
# =============================================================================
set -uo pipefail

# Bonjour name rather than a literal IP: the board is a DHCP client and its
# address moves (.26 -> .33 on 2026-08-02), which would make every experiment
# here fail at the SSH step for a reason that has nothing to do with what is
# being measured. Override with PI_HOST to target a specific address.
#
# Note that the transcripts already in results/ record the literal .26 they were
# taken against. That is deliberate -- they are evidence of what ran at the time
# and are not rewritten when the address changes.
PI_HOST=${PI_HOST:-mahan.local}
PI_USER=${PI_USER:-mahan}
STUDENT_ID=${STUDENT_ID:-402170516}
TESTS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
RESULTS=$TESTS_DIR/results

# Password auth is off on the board by design (security requirement 1), so
# every remote call goes over the key. BatchMode makes a broken key fail loudly
# instead of hanging on a prompt.
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new)

pi()  { ssh "${SSH_OPTS[@]}" "$PI_USER@$PI_HOST" "$@"; }
pish() { ssh "${SSH_OPTS[@]}" "$PI_USER@$PI_HOST" bash -s; }

# The API token lives in a root-only file on the board and must never reach
# this laptop, so token-bearing requests are issued from the board itself.
pi_api() {
    local method=$1 path=$2 body=${3:-}
    local q="TOKEN=\$(sudo grep -E '^GUARDIAN_API_TOKEN=' /etc/guardian/secrets.env | cut -d= -f2- | tr -d '\"'\"'\"'');"
    if [ "$method" = GET ]; then
        pi "$q curl -sk -H \"Authorization: Bearer \$TOKEN\" https://127.0.0.1$path"
    else
        pi "$q curl -sk -X $method -H \"Authorization: Bearer \$TOKEN\" -H 'Content-Type: application/json' -d '$body' https://127.0.0.1$path"
    fi
}

expdir() {
    local id=$1
    local d=$RESULTS/$id
    mkdir -p "$d"
    printf '%s' "$d"
}

# Echo a command into the transcript, run it, and capture its output there too.
run_logged() {
    local log=$1; shift
    { printf '\n$ %s\n' "$*"; } >>"$log"
    "$@" >>"$log" 2>&1
    local rc=$?
    printf '[exit %d]\n' "$rc" >>"$log"
    return $rc
}

hdr() { printf '\033[1;36m[%s]\033[0m %s\n' "${EXP_ID:-exp}" "$*"; }
ok()  { printf '  \033[1;32mPASS\033[0m  %s\n' "$*"; }
bad() { printf '  \033[1;31mFAIL\033[0m  %s\n' "$*"; }
note(){ printf '  ----  %s\n' "$*"; }

iso_now() { date -u +%Y-%m-%dT%H:%M:%SZ; }

# Wait until a shell condition succeeds, up to a timeout. Used instead of fixed
# sleeps so the experiments stay honest about what they actually waited for.
wait_until() {
    local timeout=$1 desc=$2; shift 2
    local deadline=$(( $(date +%s) + timeout ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if "$@" >/dev/null 2>&1; then return 0; fi
        sleep 2
    done
    bad "timed out after ${timeout}s waiting for $desc"
    return 1
}
