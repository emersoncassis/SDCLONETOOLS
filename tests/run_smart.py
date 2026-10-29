#!/usr/bin/env python3
"""Cópia inteligente (smart.h/vhd.h): monta cartões reais (MBR/GPT com FAT32, ext4 e exFAT), copia só os dados usados
para destinos menores, maiores e para .vhd, e confere com fsck.vfat, e2fsck, fsck.exfat, sfdisk e qemu-img.
Precisa de root e: dosfstools mtools exfatprogs exfat-fuse e2fsprogs fdisk qemu-utils."""
import os, subprocess, random, sys, shutil, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = tempfile.mkdtemp(prefix='sdsmart_')
os.environ['ASAN_OPTIONS'] = 'detect_leaks=0'
for src, out in (('tests/smart_test.c', 'smart_test'), ('fstest.c', 'fstest')):
    r = subprocess.run(['gcc', '-O1', '-g', '-fsanitize=address,undefined', '-I', ROOT, '-o', f'{T}/{out}', os.path.join(ROOT, src)], capture_output=True, text=True)
    if r.returncode: print(r.stderr); sys.exit(1)
fails = 0
def sh(c, check=True):
    r = subprocess.run(c, shell=True, capture_output=True, text=True)
    if check and r.returncode != 0: print('FALHOU:', c, r.stdout, r.stderr); sys.exit(1)
    return r
def ok(n, c, extra=''):
    global fails
    print(('  OK    ' if c else '  FALHA ') + n + ('' if c else '  ' + extra))
    if not c: fails += 1
MB = 1 << 20
def tree(root, seed, n, big):
    shutil.rmtree(root, ignore_errors=True); os.makedirs(root)
    rnd = random.Random(seed)
    for i in range(n):
        d = os.path.join(root, f'pasta{i % 7}', f'sub{i % 3}'); os.makedirs(d, exist_ok=True)
        open(os.path.join(d, f'arq_{i:04d}.bin'), 'wb').write(rnd.randbytes(rnd.randint(0, big)))
def mkfat(img, mb, src):
    sh(f'rm -f {img}; truncate -s {mb}M {img}; mkfs.vfat -F 32 -n BOOTFAT {img} >/dev/null; mcopy -s -i {img} {src}/* ::/')
def mkext(img, mb, src):
    sh(f'rm -f {img}; truncate -s {mb}M {img}; mke2fs -q -t ext4 -L ROOTFS -d {src} {img}')
def mkexfat(img, mb, src):
    sh(f'rm -f {img}; truncate -s {mb}M {img}; mkfs.exfat -L EASYROMS {img} >/dev/null')
    loop = sh(f'losetup -f --show {img}').stdout.strip(); m = f'{T}/mnt'; os.makedirs(m, exist_ok=True)
    sh(f'mount.exfat-fuse {loop} {m}'); sh(f'cp -r {src}/. {m}/; sync'); sh(f'umount {m}'); sh(f'losetup -d {loop}')
def assemble(disk, total_mb, label, parts):
    sh(f'rm -f {disk}; truncate -s {total_mb}M {disk}')
    lines = [f'label: {label}']
    for start, img, typ in parts:
        sz = os.path.getsize(img) // 512
        lines.append(f'start={start}, size={sz}, type={typ}')
        sh(f'dd if={img} of={disk} bs=1M seek={start*512//MB} conv=notrunc status=none')
    sh(f"printf '%s\\n' | sfdisk -q {disk}" % '\\n'.join(lines))
    # "carregador de boot" no espaço antes da 1a partição (como no R36S)
    sh(f'dd if=/dev/urandom of={disk} bs=512 seek=64 count=1900 conv=notrunc status=none')
def parts_of(disk):
    out = sh(f'sfdisk -d {disk}').stdout
    res = []
    for l in out.splitlines():
        if 'start=' in l:
            f = dict(x.strip().split('=', 1) for x in l.split(':', 1)[1].split(',') if '=' in x)
            res.append((int(f['start']), int(f['size'])))
    return res
def extract(disk, start, size, out):
    sh(f'dd if={disk} of={out} bs=512 skip={start} count={size} status=none')
def walk(img):
    return sh(f'{T}/fstest {img} walk 0').stdout
def check_case(name, src, dst_mb, kinds, ref=None):
    print(f'== {name}')
    ref = ref or src
    dst = f'{T}/dst.img'
    r = sh(f'{T}/smart_test {src} {dst} {dst_mb*MB}', check=False); print(r.stdout.strip())
    if r.returncode != 0: ok('cópia executada', False, r.stdout + r.stderr); return
    ok('tamanho do destino', os.path.getsize(dst) == dst_mb * MB)
    a = open(ref, 'rb').read(1048576); b = open(dst, 'rb').read(1048576)
    ok('área de boot antes da 1a partição igual', a[512:] == b[512:] if 'GPT' not in name else a[34*512:] == b[34*512:])
    sp = parts_of(ref); dp = parts_of(dst)
    ok('mesmo número de partições', len(sp) == len(dp), f'{sp} {dp}')
    if 'gpt' in name: ok('GPT válida (sfdisk --verify)', 'error' not in sh(f'sfdisk --verify {dst} 2>&1', check=False).stdout.lower())
    for i, (k, (s0, z0), (s1, z1)) in enumerate(zip(kinds, sp, dp)):
        extract(ref, s0, z0, f'{T}/sp.img'); extract(dst, s1, z1, f'{T}/dp.img')
        ok(f'p{i+1} ({k}) termina dentro do destino', (s1 + z1) * 512 <= dst_mb * MB)
        if k == 'fat': r = sh(f'fsck.vfat -n {T}/dp.img', check=False)
        elif k == 'ext': r = sh(f'e2fsck -fn {T}/dp.img', check=False)
        else: r = sh(f'fsck.exfat -n {T}/dp.img', check=False)
        ok(f'p{i+1} ({k}) fsck sem erros', r.returncode == 0, r.stdout[-600:] + r.stderr[-300:])
        ok(f'p{i+1} ({k}) arquivos idênticos (CRC de todos)', walk(f'{T}/sp.img') == walk(f'{T}/dp.img'))
    return dst

tree(f'{T}/t1', 1, 40, 200000); tree(f'{T}/t2', 2, 120, 400000); tree(f'{T}/t3', 3, 300, 1500000)
mkfat(f'{T}/p1.img', 128, f'{T}/t1'); mkext(f'{T}/p2.img', 300, f'{T}/t2'); mkexfat(f'{T}/p3.img', 700, f'{T}/t3')
mkfat(f'{T}/p3f.img', 700, f'{T}/t3')
D1 = f'{T}/mbr_exfat.img'
assemble(D1, 1200, 'dos', [(2048, f'{T}/p1.img', 'c'), (2048 + 128*2048, f'{T}/p2.img', '83'), (2048 + 428*2048, f'{T}/p3.img', '7')])
# exFAT termina no fim do disco? estende a última partição até o fim do cartão (como cartões reais)
check_case('MBR, exFAT por último, 1200 MB -> 900 MB', D1, 900, ['fat', 'ext', 'exfat'])
check_case('MBR, exFAT, mesmo tamanho', D1, 1200, ['fat', 'ext', 'exfat'])
D2 = f'{T}/mbr_fat.img'
assemble(D2, 1200, 'dos', [(2048, f'{T}/p1.img', 'c'), (2048 + 128*2048, f'{T}/p2.img', '83'), (2048 + 428*2048, f'{T}/p3f.img', 'c')])
check_case('MBR, FAT32 por último, 1200 MB -> 900 MB', D2, 900, ['fat', 'ext', 'fat'])
D3 = f'{T}/gpt.img'
assemble(D3, 1200, 'gpt', [(2048, f'{T}/p1.img', 'EBD0A0A2-B9E5-4433-87C0-68B6B72699C7'), (2048 + 128*2048, f'{T}/p2.img', '0FC63DAF-8483-4772-8E79-3D69D8477DE4'), (2048 + 428*2048, f'{T}/p3.img', 'EBD0A0A2-B9E5-4433-87C0-68B6B72699C7')])
check_case('GPT, exFAT por último, 1200 MB -> 900 MB', D3, 900, ['fat', 'ext', 'exfat'])
check_case('GPT, destino maior (1200 -> 1500 MB)', D3, 1500, ['fat', 'ext', 'exfat'])
check_case('GPT, mesmo tamanho', D3, 1200, ['fat', 'ext', 'exfat'])
# muito pequeno: precisa recusar
print('== destino pequeno demais')
r = sh(f'{T}/smart_test {D1} {T}/x.img {500*MB}', check=False); print(r.stdout.strip())
ok('recusa destino menor que os dados', r.returncode == 3)
# ext4 por último: recusa
D4 = f'{T}/mbr_ext_last.img'
assemble(D4, 1200, 'dos', [(2048, f'{T}/p1.img', 'c'), (2048 + 128*2048, f'{T}/p3.img', '7'), (2048 + 828*2048, f'{T}/p2.img', '83')])
r = sh(f'{T}/smart_test {D4} {T}/x.img {1000*MB}', check=False); print(r.stdout.strip())
ok('recusa reduzir ext4', r.returncode == 3)
# VHD: backup só dos dados, depois grava em cartão menor
print('== VHD')
r = sh(f'{T}/smart_test {D1} {T}/b.vhd same'); print(r.stdout.strip())
ok('VHD bem menor que o cartão', os.path.getsize(f'{T}/b.vhd') < 400 * MB, str(os.path.getsize(f'{T}/b.vhd')))
r = sh(f'qemu-img info -f vpc {T}/b.vhd', check=False); ok('qemu-img reconhece o VHD', r.returncode == 0 and 'vpc' in r.stdout, r.stdout + r.stderr)
sh(f'qemu-img convert -f vpc -O raw {T}/b.vhd {T}/b_raw.img')
ok('VHD tem o tamanho virtual do cartão', os.path.getsize(f'{T}/b_raw.img') == os.path.getsize(D1), str(os.path.getsize(f'{T}/b_raw.img')))
for i, (s, z) in enumerate(parts_of(D1)):
    extract(D1, s, z, f'{T}/sp.img'); extract(f'{T}/b_raw.img', s, z, f'{T}/dp.img')
    ok(f'VHD p{i+1} arquivos idênticos', walk(f'{T}/sp.img') == walk(f'{T}/dp.img'))
check_case('VHD -> cartão de 900 MB (restauração com redução)', f'{T}/b.vhd', 900, ['fat', 'ext', 'exfat'], ref=D1)
print('FALHAS:', fails)
shutil.rmtree(T, ignore_errors=True)
sys.exit(1 if fails else 0)
