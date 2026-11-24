#!/bin/bash
# 보드 자체 서명 인증서(docs/https.md 3장). 시작 셸이 HTTPS가 켜져 있고 사용자 인증서가 없을 때 부른다(시험도 같이 씀).
#
#   make-cert.sh <폴더>          → <폴더>/workspace.crt, workspace.key(0600), workspace.san(만들 때의 이름 목록)
#
# 이름(SAN): 호스트 이름, <호스트>.local, localhost, 127.0.0.1, 지금 보드의 IPv4 주소(hostname -I). 주소는 저장소에 적지 않고
# 만들 때 알아낸다. 다시 만드는 때: 파일이 없을 때, 만료까지 CERT_RENEW_DAYS(기본 30)일 안일 때, 보드 주소·이름이 바뀌었을 때.
# 그대로면 아무것도 안 한다. 시험용 덮어쓰기: CERT_IPS(주소 목록), CERT_DAYS(유효 기간, 기본 825일).
set -euo pipefail
dir="$1"
days="${CERT_DAYS:-825}"
renew_days="${CERT_RENEW_DAYS:-30}"
crt="$dir/workspace.crt" key="$dir/workspace.key" sanf="$dir/workspace.san"
host="$(hostname -s 2>/dev/null || hostname)"
ips="${CERT_IPS-$(hostname -I 2>/dev/null || true)}"
san="DNS:$host,DNS:$host.local,DNS:localhost,IP:127.0.0.1"
for ip in $ips; do
  [[ "$ip" == *:* || "$ip" == 127.0.0.1 ]] && continue  # IPv6는 넣지 않음(바뀌기 쉬움)
  san+=",IP:$ip"
done
if [[ -f "$crt" && -f "$key" && "$(cat "$sanf" 2>/dev/null)" == "$san" ]] &&
   openssl x509 -checkend $((renew_days * 86400)) -noout -in "$crt" >/dev/null 2>&1; then
  echo "certificate ok: $crt"
  exit 0
fi
mkdir -p "$dir"
chmod 700 "$dir"
tmp="$(mktemp -d "$dir/.new.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days "$days" \
  -subj "/CN=$host" -addext "subjectAltName=$san" -keyout "$tmp/key" -out "$tmp/crt" 2>/dev/null
chmod 600 "$tmp/key"
mv -f "$tmp/key" "$key"
mv -f "$tmp/crt" "$crt"
echo "$san" > "$sanf"
echo "certificate made: $crt ($san, $days days)"
