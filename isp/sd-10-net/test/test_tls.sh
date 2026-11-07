#!/bin/bash
# Host test of aa_tls.c against openssl s_server, with a throwaway CA and RSA-2048 client/server keys (never real keys).
set -e
cd "$(dirname "$0")"
T=$(mktemp -d); trap 'kill $SRV 2>/dev/null || true; rm -rf $T' EXIT
openssl req -x509 -newkey rsa:2048 -nodes -keyout $T/ca.key -out $T/ca.crt -days 1 -subj /CN=test-ca 2>/dev/null
for n in srv cli; do
  openssl req -newkey rsa:2048 -nodes -keyout $T/$n.key -out $T/$n.csr -subj /CN=$n 2>/dev/null
  openssl x509 -req -in $T/$n.csr -CA $T/ca.crt -CAkey $T/ca.key -CAcreateserial -out $T/$n.crt -days 1 2>/dev/null
done
M=../mbedtls
gcc -O1 -DAA_TLS_HOST -I.. -I$M/include '-DMBEDTLS_CONFIG_FILE="aa_tls_config.h"' -o tls_host aatls.c ../aa_tls.c $M/library/*.c
PORT=$((20000 + RANDOM % 20000))
openssl s_server -accept $PORT -cert $T/srv.crt -key $T/srv.key -CAfile $T/ca.crt -Verify 1 -tls1_2 -rev -quiet >$T/srv.log 2>&1 &
SRV=$!; sleep 0.5
./tls_host $PORT $T/cli.crt $T/cli.key
grep -q "CN=cli\|CN = cli" $T/srv.log && echo "PASS server saw client certificate CN=cli" || { echo "server log:"; cat $T/srv.log; }
