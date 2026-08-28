#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
SERVER_CONFIG="${REPO_DIR}/im_server/config"
ANDROID_CA="${REPO_DIR}/jitong_android/app/src/main/res/raw/im_dev_ca.pem"

mkdir -p "${SERVER_CONFIG}" "$(dirname "${ANDROID_CA}")"

openssl req -x509 -newkey rsa:2048 -sha256 -nodes \
  -keyout "${SERVER_CONFIG}/dev_server.key" \
  -out "${SERVER_CONFIG}/dev_server.crt" \
  -days 365 \
  -subj "/CN=im.example.com" \
  -addext "subjectAltName=DNS:im.example.com,DNS:localhost,IP:127.0.0.1,IP:10.0.2.2"

cp "${SERVER_CONFIG}/dev_server.crt" "${ANDROID_CA}"
chmod 600 "${SERVER_CONFIG}/dev_server.key"
echo "Development TLS certificate generated; rebuild the Android app so it trusts the new CA."
