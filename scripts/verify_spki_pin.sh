#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <certificate.pem> <expected-sha256/base64-pin>" >&2
  exit 2
fi

certificate_path=$1
expected_pin=${2#sha256/}

if [[ ! -f "$certificate_path" ]]; then
  echo "certificate not found: $certificate_path" >&2
  exit 2
fi

actual_pin=$(openssl x509 -in "$certificate_path" -pubkey -noout \
  | openssl pkey -pubin -outform DER \
  | openssl dgst -sha256 -binary \
  | openssl base64 -A)

if [[ "$actual_pin" != "$expected_pin" ]]; then
  echo "SPKI pin mismatch" >&2
  echo "expected: sha256/$expected_pin" >&2
  echo "actual:   sha256/$actual_pin" >&2
  exit 1
fi

echo "SPKI pin verified: sha256/$actual_pin"
