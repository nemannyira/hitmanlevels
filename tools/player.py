"""Plays Hitman like a player would, through the game's own input (dev
commands): walks and runs with the keys, steers with the mouse, picks up,
shoots. For playing levels through in tests.

    import player
    player.walk_to(x, z)          # True when there (within 1.5 m)
    player.pickup()  player.shoot()
"""
import math
import time

import dev

DIK_W, DIK_E, DIK_LSHIFT, DIK_SPACE = 0x11, 0x12, 0x2A, 0x39
MICKEYS_PER_DEGREE = 25.0  # measured on C1-1 (the default mouse settings)


def lua(code, timeout=15):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def pos():
    return [float(v) for v in lua('local x, y, z = h2.player():pos() return x .. " " .. y .. " " .. z').split()]


def _angle(dx, dz):
    return math.degrees(math.atan2(dx, dz))  # 0 = +Z, 90 = +X (as obj:rot)


def _diff(a, b):
    return (a - b + 180.0) % 360.0 - 180.0


def walk_to(x, z, run=False, near=150.0, timeout=120.0, log=print):
    """Walks Hitman to (x, z) with the keys, steering by where he really went
    (a mouse move turns him after a moment: positive = to larger angles)."""
    start = time.time()
    stuck = 0
    err = 0.0
    while time.time() - start < timeout:
        p = pos()
        dist = math.hypot(x - p[0], z - p[2])
        if dist < near:
            return True
        step = 0.5 if abs(err) > 25 or dist < 400 else 1.2
        keys = 'h2.dev_key(%d, %f)' % (DIK_W, step)
        if run:
            keys += ' h2.dev_key(%d, %f)' % (DIK_LSHIFT, step)
        lua(keys + ' return 1')
        time.sleep(step + 0.2)
        now = pos()
        moved = math.hypot(now[0] - p[0], now[2] - p[2])
        if moved < 15:
            stuck += 1
            if stuck >= 6:
                log("    stuck at %.0f %.0f %.0f (%.0f cm to go)" % (now[0], now[1], now[2], dist))
                return False
            lua('h2.dev_look(%d, 0) return 1' % int(60 * MICKEYS_PER_DEGREE))  # try another way
            time.sleep(0.4)
            continue
        stuck = 0
        err = _diff(_angle(x - now[0], z - now[2]), _angle(now[0] - p[0], now[2] - p[2]))
        if abs(err) > 3:
            lua('h2.dev_look(%d, 0) return 1' % int(err * MICKEYS_PER_DEGREE))
            time.sleep(0.4)
    p = pos()
    log("    timeout walking (%.0f cm to go)" % math.hypot(x - p[0], z - p[2]))
    return False


def pickup():
    lua('h2.dev_key(%d, 0.3) return 1' % DIK_E)
    time.sleep(1.0)


def shoot(times=1):
    for _ in range(times):
        lua('h2.dev_mouse(0, 0.15) return 1')
        time.sleep(0.6)


def look_at(x, y, z):
    """Turns him (the camera) towards a point: a shot then goes there."""
    p = pos()
    lua('local p = h2.player() p:setrot(%f) return 1' % _angle(x - p[0], z - p[2]))
    time.sleep(0.3)
