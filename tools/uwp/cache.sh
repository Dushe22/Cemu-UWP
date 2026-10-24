#!/bin/bash
# copy a game's shader/pipeline caches between Xboxes, so a new console starts with warm caches
#   cache.sh export <titleId> <dir> [host]   download them into <dir>
#   cache.sh import <titleId> <dir> [host]   upload them from <dir>; close Cemu on that console first
# host defaults to $XBOX_HOST. titleId as in the file names, e.g. 00050000101c9400 (BOTW USA). Device Portal auth must be off.
# The usage file is console-specific history, so it is not copied: the target builds its own.
set -e
MODE=$1; TITLE=$2; DIR=$3; HOST=${4:-$XBOX_HOST}; B=https://$HOST:11443
[ -n "$MODE" ] && [ -n "$TITLE" ] && [ -n "$DIR" ] && [ -n "$HOST" ] || { echo "usage: cache.sh export|import <titleId> <dir> [host]"; exit 1; }
FULL=$(curl -sk -m 30 $B/api/app/packagemanager/packages | grep -o '"PackageFullName" : "Cemu.Libretro[^"]*"' | cut -d'"' -f4)
[ -n "$FULL" ] || { echo "Cemu is not installed on $B"; exit 1; }
Q="knownfolderid=LocalAppData&packagefullname=$FULL"
# precompiled = DXIL for this GPU family, transferable = guest shaders + PSO recipes
FILES="precompiled/${TITLE}_dxil.bin transferable/${TITLE}_shaders.bin transferable/${TITLE}_d3d12recipes.bin"
mkdir -p "$DIR"
if [ "$MODE" = export ]; then
  for f in $FILES; do
    folder=$(dirname $f); name=$(basename $f)
    code=$(curl -sk -m 600 -o "$DIR/$name" -w "%{http_code}" "$B/api/filesystem/apps/file?$Q&path=%5CLocalState%5Ccache%5CshaderCache%5C$folder&filename=$name")
    [ "$code" = 200 ] && echo "exported $name ($(wc -c < "$DIR/$name") bytes)" || { rm -f "$DIR/$name"; echo "skipped $name (HTTP $code)"; }
  done
elif [ "$MODE" = import ]; then
  if curl -sk -m 30 $B/api/resourcemanager/processes | grep -q '"ImageName" : "CemuUwp.exe"'; then
    echo "Cemu is running on $B; close it first (it keeps the cache files open)"; exit 1
  fi
  J=$(mktemp); curl -sk -m 15 -c $J -o /dev/null $B/api/os/info
  T=$(grep -i csrf $J | awk '{print $7}')
  for f in $FILES; do
    folder=$(dirname $f); name=$(basename $f)
    [ -f "$DIR/$name" ] || { echo "missing $DIR/$name, skipped"; continue; }
    curl -sk -m 600 -b $J -H "X-CSRF-Token: $T" -o /dev/null -w "import $name HTTP %{http_code}\n" -F "$name=@$DIR/$name" \
      "$B/api/filesystem/apps/file?$Q&path=%5CLocalState%5Ccache%5CshaderCache%5C$folder"
  done
  rm -f $J
else
  echo "unknown mode $MODE"; exit 1
fi
