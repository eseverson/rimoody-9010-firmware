#!/usr/bin/env bash
# Head unit power through Home Assistant (the owner's switch on the unit's supply; same as an ACC cycle).
#   tools/hu_power.sh state | on | off | cycle [OFF_SECONDS, default 8]
# Needs tools/ha.env (gitignored): HA_URL, HA_TOKEN (long-lived token "claude"), HA_SWITCH.
set -eu
cd "$(dirname "$0")/.."
source tools/ha.env
api() { curl -s -m 15 -H "Authorization: Bearer $HA_TOKEN" -H "Content-Type: application/json" "$@"; }
state() { api "$HA_URL/api/states/$HA_SWITCH" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("state"))'; }
svc() { api -X POST -d "{\"entity_id\": \"$HA_SWITCH\"}" "$HA_URL/api/services/switch/$1" >/dev/null; }
case "${1:-state}" in
    state) state ;;
    on) svc turn_on; sleep 1; state ;;
    off) svc turn_off; sleep 1; state ;;
    cycle) svc turn_off; sleep "${2:-8}"; svc turn_on; sleep 1; state ;;
    *) echo "usage: $0 state|on|off|cycle [OFF_SECONDS]" >&2; exit 2 ;;
esac
