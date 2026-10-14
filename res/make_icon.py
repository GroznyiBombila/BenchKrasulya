# Генерация app.ico: чёрный «осциллограф» со ступенчатым графиком L1→L2→L3→ОЗУ
import struct, math
def render(n):
    px = [[(0,0,0,0)]*n for _ in range(n)]
    r = n*0.18
    for y in range(n):
        for x in range(n):
            # скруглённый квадрат
            dx = max(r-x-0.5, x+0.5-(n-r), 0); dy = max(r-y-0.5, y+0.5-(n-r), 0)
            d = math.hypot(dx, dy)
            if d > r: continue
            a = int(255*min(1, r-d+0.5))
            c = (12,16,20)
            if n >= 32 and (x % (n//4) == 0 or y % (n//4) == 0): c = (20,70,70)
            px[y][x] = (*c, a)
    # ступенчатая кривая скорости (сверху вниз)
    lv = [0.20, 0.38, 0.58, 0.80]
    seg = [0.08, 0.32, 0.56, 0.78, 0.94]
    w = max(1, n//16)
    def dot(x, y, col):
        for yy in range(int(y)-w//2, int(y)+w-w//2):
            for xx in range(int(x)-w//2, int(x)+w-w//2):
                if 0<=xx<n and 0<=yy<n and px[yy][xx][3]: px[yy][xx] = (*col,255)
    pts = []
    for i in range(4):
        pts.append((seg[i]*n, lv[i]*n)); pts.append((seg[i+1]*n - n*0.04, lv[i]*n))
    for (x0,y0),(x1,y1) in zip(pts, pts[1:]):
        steps = int(max(abs(x1-x0), abs(y1-y0))*2)+1
        for s in range(steps+1):
            t = s/steps
            dot(x0+(x1-x0)*t, y0+(y1-y0)*t, (0,230,230) if y1==y0 else (255,220,0))
    return px
def bmp(n):
    px = render(n)
    hdr = struct.pack('<IiiHHIIiiII', 40, n, n*2, 1, 32, 0, 0, 0, 0, 0, 0)
    data = b''.join(bytes((b,g,r,a)) for row in reversed(px) for (r,g,b,a) in row)
    mask = b'\0' * (((n+31)//32)*4*n)
    return hdr + data + mask
sizes = [16, 24, 32, 48, 64, 256]
imgs = [bmp(n) for n in sizes]
out = struct.pack('<HHH', 0, 1, len(sizes))
off = 6 + 16*len(sizes)
for n, im in zip(sizes, imgs):
    out += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(im), off); off += len(im)
open('app.ico','wb').write(out + b''.join(imgs))
