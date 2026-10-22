#!/bin/bash
# Software replacement for physically replugging the UE200 (USB Ethernet, 98:03:8e:79:ea:42).
# - The dongle itself: link up but no DHCP lease (no address, or a 169.254 one) for a minute -> ask DHCP again;
#   still none a minute later -> re-enumerate the dongle. Checked on its own because Wi-Fi can carry the default route
#   meanwhile, which hid a cable left without a lease after boot (2026-10-22).
# - The default route: its gateway silent for a minute -> re-enumerate the dongle and renew DHCP everywhere.
MAC=98:03:8e:79:ea:42
fail=0; nolease=0
while true; do
  IF=$(ifconfig -a | awk -v m="$MAC" '/^[a-z]/ {i=$1; sub(":", "", i)} $1 == "ether" && $2 == m {print i; exit}')
  if [ -n "$IF" ] && ifconfig "$IF" | grep -q "status: active"; then
    A=$(ifconfig "$IF" | awk '$1 == "inet" {print $2; exit}')
    case "$A" in ""|169.254.*) nolease=$((nolease+1)) ;; *) nolease=0 ;; esac
    if [ "$nolease" -eq 4 ]; then
      logger -t ue200watch "$IF: link up without a lease for 60 s: asking DHCP again"
      ipconfig set "$IF" DHCP >/dev/null 2>&1
    elif [ "$nolease" -ge 8 ]; then
      logger -t ue200watch "$IF: still no lease: re-enumerating UE200"
      /usr/local/bin/ue200reset 2>&1 | logger -t ue200watch
      sleep 8; ipconfig set "$IF" DHCP >/dev/null 2>&1
      nolease=0
    fi
  fi
  GW=$(route -n get default 2>/dev/null | awk '/gateway/{print $2}')
  if [ -n "$GW" ] && ping -c1 -t2 "$GW" >/dev/null 2>&1; then
    fail=0
  else
    fail=$((fail+1))
    if [ "$fail" -ge 4 ]; then
      logger -t ue200watch "no connectivity for $((fail*15))s: re-enumerating UE200"
      /usr/local/bin/ue200reset 2>&1 | logger -t ue200watch
      sleep 8
      for i in $(networksetup -listallhardwareports | awk '/Device: en/{print $2}'); do
        ipconfig set "$i" DHCP >/dev/null 2>&1
      done
      fail=0
    fi
  fi
  sleep 15
done
