#!/bin/bash
# m.sh <script> [args]: copies a local script to the Mac and runs it there with bash.
# Mac IP from $MAC (else findmac.sh), SSH password from $MAC_PASS (kept out of the repo: see CLAUDE.local.md).
# Scripts that need root use: echo "$MAC_PASS" | sudo -S -p "" ... (the password is passed in as MAC_PASS too).
D=$(cd "$(dirname "$0")" && pwd)
: "${MAC_PASS:?set MAC_PASS (see CLAUDE.local.md)}"
M=${MAC:-$("$D/findmac.sh" 60)} || { echo "Mac not found" >&2; exit 1; }
K=$HOME/.ssh/known_hosts_firebat_mac; O=(-o UserKnownHostsFile="$K" -o HostKeyAlias=192.168.31.239 -o ConnectTimeout=8)
f=$1; shift; b=/tmp/$(basename "$f")
sshpass -p "$MAC_PASS" scp "${O[@]}" -q "$f" "dripside@$M:$b" && sshpass -p "$MAC_PASS" ssh "${O[@]}" "dripside@$M" "MAC_PASS='$MAC_PASS' bash $b $*"
