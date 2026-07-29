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
HOST_IP=${HOST_IP:-192.168.100.26}
HOSTNAME_SHORT=$(hostname -s 2>/dev/null || echo guardian)
DAYS=${DAYS:-825}

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
IP.1  = $HOST_IP
IP.2  = 127.0.0.1
DNS.1 = $HOSTNAME_SHORT
DNS.2 = $HOSTNAME_SHORT.local
DNS.3 = localhost
EOF

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
