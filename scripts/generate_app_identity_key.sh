#!/usr/bin/env bash
set -euo pipefail

output_path=${1:-im_server/config/dev_app_identity_private.pem}
output_dir=$(dirname "$output_path")
mkdir -p "$output_dir"

if [[ -e "$output_path" ]]; then
  echo "refusing to overwrite existing key: $output_path" >&2
  exit 1
fi

openssl genpkey -algorithm ED25519 -out "$output_path"
chmod 600 "$output_path"

public_pin=$(openssl pkey -in "$output_path" -pubout -outform DER \
  | tail -c 32 \
  | openssl base64 -A)

echo "generated: $output_path"
echo "Android raw Ed25519 public key (base64): $public_pin"
