#!/bin/bash
# Characters at the screen sides (Hor+ strips): a C1-1 guard straight ahead and
# 38 degrees to each side. Screenshots dumps/shots/sides_{center,left,right}.png
cd "$(dirname "$0")/../.."  # (the repository)
bash h2/tools/load.sh 'C1-1\C1-1__MAIN' "$@" || exit 1
D=h2/tools/dev.py
python $D 'h2.freecam(true) h2.setcampos(2750, 170, -6220) h2.camlookat(2750, 120, -5620) return "cam"'
sleep 2
python $D --shot sides_center
python $D 'local y,p=h2.camrot() h2.setcamrot(y-38, p) return "left"'
sleep 2
python $D --shot sides_right
python $D 'local y,p=h2.camrot() h2.setcamrot(y+76, p) return "right"'
sleep 2
python $D --shot sides_left
cd dumps/shots && python -c "
from PIL import Image
ims=[Image.open(f) for f in ['sides_left.png','sides_center.png','sides_right.png']]
w,h=ims[0].size
o=Image.new('RGB',(w,h*3))
for i,im in enumerate(ims): o.paste(im,(0,h*i))
o.resize((w*2//3,h*2)).save('sides_sheet.png')"
echo dumps/shots/sides_sheet.png
