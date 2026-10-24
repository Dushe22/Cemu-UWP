#!/bin/bash
# install out/package msix on the Xbox (upgrade in place, keeps app data) and launch it
# --clean uninstalls first, which wipes LocalState (settings, caches, game list)
# XBOX_HOST is the console address; Device Portal authentication must be off
set -e
CLEAN=0; [ "$1" = "--clean" ] && CLEAN=1
cd "$(dirname "$0")/../../out/package"
: "${XBOX_HOST:?set XBOX_HOST to the console IP}"
B=https://$XBOX_HOST:11443
J=$(mktemp)
curl -sk -m 15 -c $J -o /dev/null $B/api/os/info
T=$(grep -i csrf $J | awk '{print $7}')
H=(-b $J -H "X-CSRF-Token: $T")
state() { for i in $(seq 1 60); do r=$(curl -sk -m 15 -w " %{http_code}" $B/api/app/packagemanager/state); case "$r" in *" 204") sleep 5;; *) echo "$r"; return;; esac; done; }
FULL=$(curl -sk -m 30 $B/api/app/packagemanager/packages | grep -o '"PackageFullName" : "Cemu.Libretro[^"]*"' | cut -d'"' -f4 || true)
if [ -n "$FULL" ] && [ $CLEAN = 1 ]; then
  curl -sk -m 120 "${H[@]}" -X DELETE -o /dev/null -w "uninstall HTTP %{http_code}\n" "$B/api/app/packagemanager/package?package=$FULL"
  sleep 5
fi
# MSIX=<file> picks a specific package (e.g. an older bisect build); default is the newest
MSIX=${MSIX:-$(ls -t Cemu_*_x64.msix | head -1)}
echo "installing $MSIX over ${FULL:-nothing}"
curl -sk -m 900 "${H[@]}" -o /dev/null -w "upload HTTP %{http_code}\n" -F "$MSIX=@$MSIX" -F "Microsoft.VCLibs.x64.14.00.appx=@Microsoft.VCLibs.x64.14.00.appx" -F "CemuUwp_test.cer=@CemuUwp_test.cer" "$B/api/app/packagemanager/package?package=$MSIX"
state
FULL=$(curl -sk -m 30 $B/api/app/packagemanager/packages | grep -o '"PackageFullName" : "Cemu.Libretro[^"]*"' | cut -d'"' -f4)
A=$(printf %s "Cemu.Libretro.Uwp_1qrqqmyv3f0wj!CemuUwp" | base64 -w0)
P=$(printf %s "$FULL" | base64 -w0)
curl -sk -m 30 "${H[@]}" -X POST -H "Content-Length: 0" -o /dev/null -w "launch HTTP %{http_code}\n" "$B/api/taskmanager/app?appid=$A&package=$P"
