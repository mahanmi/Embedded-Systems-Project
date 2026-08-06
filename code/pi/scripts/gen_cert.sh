#!/usr/bin/env bash
# =============================================================================
#  Self-signed TLS certificate for the board's web server (part 1-b)
#
#  The brief requires the certificate's CN field to be the student number, so
#  CN=402170516. Two extra details matter in practice:
#
#    * Modern browsers ignore CN entirely and match the hostname against the
#      subjectAltName extension. A certificate with only a CN would make the
#      page unreachable without click-through on every request and would show
#      "missing_subjectAltName" rather than a normal certificate dialog. SANs
#      for the board's IP and hostnames are therefore included as well -- the
#      CN is still the student number, which is what experiment 1-5 inspects.
#
#    * basicConstraints=CA:true lets the certificate be imported as a trust
#      anchor if a clean padlock is wanted for the screenshots.
# =============================================================================
set -euo pipefail

STUDENT_ID=${STUDENT_ID:-402170516}
CERTDIR=${CERTDIR:-/etc/guardian/certs}
# The board's own LAN address, included in the SAN list so browsing by IP still
# validates. Detected rather than hardcoded: this board is a DHCP client and its
# address moves (.26 -> .33 on 2026-08-02). A literal default is worse than none
# here -- regenerating with a stale one produces a certificate that quietly fails
# to match the address it is actually served on, while the DNS names keep working
# and hide it. Override explicitly when generating for a different host.
#
# May legitimately come out empty (no default route at generation time); the IP
# entry is then omitted and the hostnames below carry the certificate, which is
# what public_host and the mac/ scripts address the board by anyway.
HOST_IP=${HOST_IP:-$(ip -4 route get 1 2>/dev/null | sed -n 's/.*src \([0-9.]*\).*/\1/p')}
HOSTNAME_SHORT=$(hostname -s 2>/dev/null || echo guardian)
DAYS=${DAYS:-825}

# The name the board answers to from outside its own LAN -- a DDNS hostname
# once the dashboard is reached over the internet rather than by LAN IP.
# Without it in the SAN list every remote request is a name mismatch stacked
# on top of the self-signed warning, which is a different and much noisier
# browser dialog. Empty by default, so a LAN-only install is unchanged.
#
#   sudo HOST_IP=<lan ip> PUBLIC_NAME=<ddns name> ./gen_cert.sh
PUBLIC_NAME=${PUBLIC_NAME:-}

install -d -m 0755 "$CERTDIR"

CONF=$(mktemp)
trap 'rm -f "$CONF"' EXIT

cat >"$CONF" <<EOF
[req]
default_bits       = 2048
prompt             = no
default_md         = sha256
distinguished_name = dn
x509_extensions    = v3

[dn]
CN = $STUDENT_ID
O  = Sharif University of Technology
OU = Department of Electrical Engineering
L  = Tehran
C  = IR

[v3]
subjectAltName         = @alt
basicConstraints       = critical, CA:true
keyUsage               = critical, digitalSignature, keyEncipherment, keyCertSign
extendedKeyUsage       = serverAuth
subjectKeyIdentifier   = hash

[alt]
IP.1  = 127.0.0.1
DNS.1 = $HOSTNAME_SHORT
DNS.2 = $HOSTNAME_SHORT.local
DNS.3 = localhost
EOF

# Appended rather than written inside the heredoc: an entry with an empty value
# (`IP.2 = `, `DNS.4 = `) is a hard openssl parse error, so the line has to be
# absent entirely when the value is not set. 127.0.0.1 takes IP.1 because it is
# the only address that is always there -- the board's own address is detected
# and may legitimately be missing.
if [ -n "$HOST_IP" ]; then
    echo "IP.2 = $HOST_IP" >>"$CONF"
    echo "[cert] including LAN address $HOST_IP in the SAN list"
else
    echo "[cert] no LAN address detected; SAN covers the hostnames only"
fi
if [ -n "$PUBLIC_NAME" ]; then
    echo "DNS.4 = $PUBLIC_NAME" >>"$CONF"
    echo "[cert] including public name $PUBLIC_NAME in the SAN list"
fi

echo "[cert] generating a $DAYS-day self-signed certificate, CN=$STUDENT_ID"
openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$CERTDIR/guardian.key" \
        -out    "$CERTDIR/guardian.crt" \
        -days "$DAYS" -config "$CONF"

chmod 0640 "$CERTDIR/guardian.key"
chmod 0644 "$CERTDIR/guardian.crt"
chown root:guardian "$CERTDIR/guardian.key" "$CERTDIR/guardian.crt" 2>/dev/null || true

echo
echo "[cert] subject / validity / SANs:"
openssl x509 -in "$CERTDIR/guardian.crt" -noout \
        -subject -issuer -dates -ext subjectAltName
echo
echo "[cert] SHA-256 fingerprint:"
openssl x509 -in "$CERTDIR/guardian.crt" -noout -fingerprint -sha256
