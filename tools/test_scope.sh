#!/bin/bash
# Sniper scope and binocular views (widescreen check): screenshots in dumps/shots.
#   bash tools/test_scope.sh [--nobuild]
cd /c/Users/pinte/source/repos/H2-LevelEditor
bash tools/load.sh 'C1-1\C1-1__MAIN' "$@" || exit 1
D=tools/dev.py
python $D 'local h=h2.objects("ZHITMAN2")[1] local x,y,z=h:pos() h2.item("RifleSniperR93", x+40, y+5, z) return "rifle"'
sleep 1
python $D 'h2.dev_key(0x12, 0.2)' >/dev/null      # E: pick up
sleep 3
python $D 'h2.dev_key(0x3B, 0.2)' >/dev/null      # F1: first person
sleep 2
python $D 'h2.dev_mouse(0, 0.3)' >/dev/null       # left button: scope
sleep 2
python $D --shot test_scope
python $D 'h2.dev_mouse(1, 0.2)' >/dev/null       # right button: leave the scope
sleep 1.5
python $D 'h2.dev_key(0x3B, 0.2)' >/dev/null      # F1: back to third person
sleep 1.5
python $D 'h2.dev_key(0x06, 0.2)' >/dev/null      # 5: binoculars
sleep 2.5
python $D --shot test_binoculars
python $D 'h2.dev_key(0x06, 0.2)' >/dev/null
sleep 1.5
python $D --shot test_normal
python $D 'local h=h2.objects("ZHITMAN2")[1] local x,y,z=h:pos() h2.item("NightvisionGoggles", x, y+5, z) return "goggles"'
sleep 1
python $D 'h2.dev_key(0x12, 0.2)' >/dev/null      # E: pick up
sleep 3
python $D 'h2.dev_key(0x07, 0.2)' >/dev/null      # 6: night vision
sleep 2.5
python $D --shot test_nightvision
