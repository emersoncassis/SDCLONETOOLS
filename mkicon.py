import struct
def draw(n):
    px=[[(0,0,0,0)]*n for _ in range(n)]
    def inside(x,y):
        # contorno de cartão SD: retângulo com canto superior direito cortado
        u=x/n; v=y/n
        if not (0.14<=u<=0.86 and 0.04<=v<=0.96): return False
        if v<0.04+ (u-0.62)*0.9 and u>0.62: return False
        return True
    for y in range(n):
        for x in range(n):
            u=x/n; v=y/n
            if inside(x,y):
                edge = not (inside(x-1,y) and inside(x+1,y) and inside(x,y-1) and inside(x,y+1))
                c=(0x1F,0x4E,0xB4,255) if not edge else (0x10,0x2A,0x6B,255)
                if 0.34<=v<=0.62 and 0.26<=u<=0.74 and not edge: c=(0xEE,0xF3,0xFF,255)   # etiqueta
                if 0.78<=v<=0.93 and 0.22<=u<=0.78 and int((u-0.22)/0.56*6)%2==0 and not edge: c=(0xE8,0xB9,0x2F,255) # contatos
                px[y][x]=c
    return px
def bmp(n):
    px=draw(n)
    h=struct.pack('<IiiHHIIiiII',40,n,n*2,1,32,0,0,0,0,0,0)
    rows=b''.join(b''.join(struct.pack('<BBBB',c[2],c[1],c[0],c[3]) for c in px[y]) for y in range(n-1,-1,-1))
    mask=b'\x00'*(((n+31)//32*4)*n)
    return h+rows+mask
sizes=[48,32,16]
imgs=[bmp(s) for s in sizes]
out=struct.pack('<HHH',0,1,len(sizes)); off=6+16*len(sizes)
for s,i in zip(sizes,imgs):
    out+=struct.pack('<BBBBHHII',s,s,0,0,1,32,len(i),off); off+=len(i)
out+=b''.join(imgs)
open('sdclone.ico','wb').write(out)
