#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <ed25519-private.pem> <expected-raw-public-base64>" >&2
  exit 2
fi

private_key_path=$1
expected_public=$2
actual_public=$(openssl pkey -in "$private_key_path" -pubout -outform DER \
  | tail -c 32 \
  | openssl base64 -A)

if [[ "$actual_public" != "$expected_public" ]]; then
  echo "application identity public key mismatch" >&2
  echo "expected: $expected_public" >&2
  echo "actual:   $actual_public" >&2
  exit 1
fi

echo "application identity pin verified: $actual_public"
