#!/usr/bin/env bash
# =============================================================================
#  Credential audit
#  Embedded Systems final project -- Mahan Majlesi (402170516)
#
#  Security requirement 3 of the brief: "No password or API key may be
#  hard-coded on the device."  This script is the evidence for that claim. It
#  checks three things:
#
#    1. no credential-shaped literal appears anywhere in the source tree;
#    2. the deployed binary contains no credential strings either -- a source
#      -level grep would miss a value baked in at build time;
#    3. the real secrets file has the right owner and mode, and is not readable
#       by anyone but root and the guardian group.
#
#  Exit status is non-zero if anything looks wrong, so it can gate a build.
# =============================================================================
set -uo pipefail

ROOT=${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
SECRETS=/etc/guardian/secrets.env
BINARY=/opt/guardian/bin/guardian
FAILED=0

pass() { printf '  \033[1;32mPASS\033[0m  %s\n' "$*"; }
fail() { printf '  \033[1;31mFAIL\033[0m  %s\n' "$*"; FAILED=1; }
info() { printf '  ----  %s\n' "$*"; }

echo "Credential audit of $ROOT"
echo

# --- 1. source tree ---------------------------------------------------------
echo "1. source tree contains no hard-coded credentials"

# Assignments of a literal to a credential-shaped name. Deliberately excludes
# getenv() lookups, the .example template, and this script itself.
PATTERN='(smtp_pass|smtp_password|mqtt_pass|mqtt_password|api_token|api_key|passwd|password|secret|token)[[:space:]]*[=:][[:space:]]*["'"'"'][^"'"'"']{4,}'

HITS=$(grep -rInE "$PATTERN" "$ROOT" \
        --include='*.c' --include='*.h' --include='*.py' \
        --include='*.sh' --include='*.conf' --include='*.service' \
        --include='*.socket' --include='*.html' \
        2>/dev/null \
     | grep -vE 'secrets\.env\.example' \
     | grep -vE 'check_secrets\.sh' \
     | grep -vE 'getenv|environ|os\.environ' \
     | grep -vE '=[[:space:]]*["'"'"'](change-me|xxxx|placeholder|CHANGEME)' \
     || true)

if [ -z "$HITS" ]; then
    pass "no credential literals in .c/.h/.py/.sh/.conf/.service/.html"
else
    fail "possible hard-coded credentials:"
    echo "$HITS" | sed 's/^/        /'
fi

# The C code must reach every secret through getenv().
GETENVS=$(grep -c 'require_env("GUARDIAN_' "$ROOT/src/config.c" 2>/dev/null || echo 0)
if [ "$GETENVS" -ge 3 ]; then
    pass "all $GETENVS secrets are resolved through the environment (src/config.c)"
else
    fail "expected at least 3 require_env() lookups in src/config.c, found $GETENVS"
fi

# --- 2. compiled binary -----------------------------------------------------
echo
echo "2. the deployed binary embeds no credential"
if [ -r "$BINARY" ]; then
    if [ -r "$SECRETS" ]; then
        LEAKED=0
        while IFS='=' read -r k v; do
            case "$k" in GUARDIAN_*) ;; *) continue ;; esac
            [ ${#v} -ge 8 ] || continue
            if strings "$BINARY" 2>/dev/null | grep -qF -- "$v"; then
                fail "the value of $k appears verbatim inside $BINARY"
                LEAKED=1
            fi
        done < <(grep -E '^GUARDIAN_[A-Z_]+=' "$SECRETS" 2>/dev/null)
        [ "$LEAKED" -eq 0 ] && pass "no live secret value found in $BINARY"
    else
        info "cannot read $SECRETS (run as root for this check)"
    fi
    # The names must be there -- that is how they are looked up.
    NAMES=$(strings "$BINARY" 2>/dev/null | grep -c '^GUARDIAN_[A-Z_]*$' || echo 0)
    info "$NAMES environment variable NAMES referenced by the binary (expected)"
else
    info "$BINARY not present or not readable; skipping"
fi

# --- 3. permissions on the real secrets file --------------------------------
echo
echo "3. the secrets file is protected"
if [ -e "$SECRETS" ]; then
    MODE=$(stat -c '%a' "$SECRETS")
    OWNER=$(stat -c '%U:%G' "$SECRETS")
    [ "$MODE" = "640" ] && pass "mode $MODE" || fail "mode is $MODE, expected 640"
    [ "$OWNER" = "root:guardian" ] && pass "owner $OWNER" \
        || fail "owner is $OWNER, expected root:guardian"
    if [ "$(stat -c '%a' "$SECRETS" | cut -c3)" = "0" ]; then
        pass "not world-readable"
    else
        fail "world-readable"
    fi
else
    info "$SECRETS does not exist on this machine"
fi

# --- 4. repository hygiene ---------------------------------------------------
echo
echo "4. repository hygiene"
if find "$ROOT" -name 'secrets.env' -not -name '*.example' 2>/dev/null | grep -q .; then
    fail "a real secrets.env is inside the source tree"
else
    pass "no real secrets.env inside the source tree"
fi

echo
if [ "$FAILED" -eq 0 ]; then
    printf '\033[1;32mAUDIT PASSED\033[0m -- no hard-coded credentials found.\n'
else
    printf '\033[1;31mAUDIT FAILED\033[0m -- see the FAIL lines above.\n'
fi
exit $FAILED
