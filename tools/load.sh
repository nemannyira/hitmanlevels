#!/bin/bash
# Build, install, start the game and load a level (dev commands must be on).
#   bash tools/load.sh [scene] [--nobuild]     scene default: C1-1\C1-1__MAIN
cd /c/Users/pinte/source/repos/H2-LevelEditor
LEVEL='C1-1\C1-1__MAIN'
BUILD=1
for a in "$@"; do
    case "$a" in
        --nobuild) BUILD=0 ;;
        *) LEVEL="$a" ;;
    esac
done
if [ $BUILD = 1 ]; then
    OUT=$(cmd //c "C:\Users\pinte\source\repos\H2-LevelEditor\build.bat" Release 2>&1)
    if echo "$OUT" | grep -qE "FAILED|error C|error LNK"; then
        echo "$OUT" | grep -E "error|FAILED" | head -20
        echo "BUILD FAILED"
        exit 1
    fi
fi
taskkill //F //IM hitman2.exe >/dev/null 2>&1
sleep 1
cp build/Release/dinput8.dll "/c/Program Files (x86)/Steam/steamapps/common/Hitman 2 Silent Assassin/" || exit 1
timeout 90 python tools/dev.py --start >/dev/null
sleep 2
python tools/dev.py "return h2.dev_loadscene([[$LEVEL]])" >/dev/null
SHORT="${LEVEL##*\}"
python tools/dev.py --wait-scene "$SHORT" 60
sleep 6
