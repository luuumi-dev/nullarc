#!/bin/bash
# findmac.sh [seconds]: prints the Mac's IP once a host on $NET (default 192.168.31) answers SSH with the Mac's host key
# (~/.ssh/known_hosts_firebat_mac). MAC addresses are no use: macOS gives each Wi-Fi network a private MAC.
NET=${NET:-192.168.31}; LIM=${1:-900}; T0=$(date +%s); K=$HOME/.ssh/known_hosts_firebat_mac
[ -f "$K" ] || { echo "no $K: ssh to the Mac once with -o UserKnownHostsFile=$K" >&2; exit 2; }
KEYS=$(awk '{print $3}' "$K" | sort -u)
H=$(mktemp)
while [ $(( $(date +%s) - T0 )) -lt "$LIM" ]; do
    : > "$H"
    for i in $(seq 2 254); do (timeout 1 bash -c "echo > /dev/tcp/$NET.$i/22" 2>/dev/null && echo "$NET.$i" >> "$H") & done; wait
    for ip in $(cat "$H"); do
        for x in $(ssh-keyscan -T 3 "$ip" 2>/dev/null | awk '{print $3}'); do
            grep -qxF "$x" <<<"$KEYS" && { echo "$ip"; rm -f "$H"; exit 0; }
        done
    done
    sleep 5
done
rm -f "$H"; exit 1
