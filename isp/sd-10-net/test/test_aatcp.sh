#!/bin/bash
# aa.c over aa_tcp.c over lwIP against a scripted phone (test/aatcp.c), with a throwaway certificate.
set -e
cd "$(dirname "$0")"
T=$(mktemp -d); trap 'rm -rf $T' EXIT
openssl req -x509 -newkey rsa:2048 -nodes -keyout $T/k.pem -out $T/c.pem -days 1 -subj /CN=hu-test 2>/dev/null
L=../lwip; M=../mbedtls
gcc -O1 -w -DAA_TLS_HOST -I.. -I$L -I$L/arch -I$L/src/include -I$M/include '-DMBEDTLS_CONFIG_FILE="aa_tls_config.h"' \
  -o aatcp_host aatcp.c ../aa.c ../aa_tcp.c ../aa_tls.c $L/src/core/*.c $L/src/core/ipv4/*.c $L/src/netif/ethernet.c $M/library/*.c
./aatcp_host $T/c.pem $T/k.pem
