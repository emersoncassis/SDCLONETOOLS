#!/usr/bin/env python3
"""Gera imagens reais de cartão (ext2/3/4, FAT12/16/32, exFAT, MBR, GPT) e confere diag.h/fsread.h."""
import os, subprocess, sys, zlib, random, shutil, re

import tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix='sdclonetest_')
W = f'{TMP}/img'
SRC = f'{W}/src'
SMALL = f'{W}/src_small'
BIN = f'{TMP}/fstest'
os.makedirs(W, exist_ok=True)
_r = subprocess.run(['gcc', '-O1', '-g', '-fsanitize=address,undefined', '-o', BIN, os.path.join(ROOT, 'fstest.c')], capture_output=True, text=True)
if _r.returncode != 0:
    print('Falha ao compilar fstest.c:\n', _r.stderr); sys.exit(1)
fails = 0
env = dict(os.environ, LANG='C.UTF-8', LC_ALL='C.UTF-8')

def sh(cmd, check=True, **kw):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, env=env, **kw)
    if check and r.returncode != 0:
        print('FALHOU:', cmd, '\n', r.stdout, r.stderr); sys.exit(1)
    return r

def ok(name, cond, extra=''):
    global fails
    print(('  OK    ' if cond else '  FALHA ') + name + (('  ' + extra) if (extra and not cond) else ''))
    if not cond: fails += 1

def rnd(n, seed):
    return random.Random(seed).randbytes(n)

def make_tree(root, big):
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(root)
    def w(rel, data):
        p = os.path.join(root, rel); os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'wb').write(data)
    w('leia-me.txt', b'ola mundo\n' * 3)
    w('vazio.bin', b'')
    w('um.bin', b'X')
    for n in (4095, 4096, 4097): w(f'b{n}.bin', rnd(n, n))
    w('medio.bin', rnd(100001, 5))
    w('Acentuação ção.txt', b'acentos\n')
    w('MAIÚSCULAS e espaços.TXT', b'caps\n')
    w('a' * 150 + '.txt', b'nome longo\n')
    w('pasta/sub/deep/arquivo.dat', rnd(50000, 9))
    w('pasta/outro.txt', b'x' * 5000)
    for i in range(400 if big else 60): w(f'muitos/arquivo_numero_{i:04d}_com_nome_longo.txt', f'conteudo {i}\n'.encode() * (i % 7 + 1))
    if big: w('grande.bin', rnd(3_000_001, 77))

def expected(root):
    exp = {}
    for d, ds, fs in os.walk(root):
        for x in ds: exp['D ' + '/' + os.path.relpath(os.path.join(d, x), root)] = None
        for x in fs:
            p = os.path.join(d, x); b = open(p, 'rb').read()
            exp['F ' + '/' + os.path.relpath(p, root)] = (len(b), '%08x' % (zlib.crc32(b) & 0xffffffff))
    return exp

def walk_image(img, base, sec=512):
    r = sh(f'{BIN} {img} walk {base} {sec}', check=False)
    got = {}; fs = None; errs = []
    for line in r.stdout.splitlines():
        if line.startswith('FS '): fs = int(line.split()[1])
        elif line.startswith('D '): got[line] = None
        elif line.startswith('F '):
            parts = line.rsplit(' ', 2); got['F ' + parts[0][2:]] = (int(parts[1]), parts[2])
        elif (line.startswith('ERR') or line.startswith('ERRORS')) and line != 'ERRORS 0': errs.append(line)
    if r.returncode != 0 or 'ABORTING' in r.stderr or 'runtime error' in r.stderr or 'AddressSanitizer' in r.stderr:
        errs.append('stderr: ' + r.stderr[:400])
    return fs, got, errs

def check_fs(name, img, base, exp, want_fs):
    fs, got, errs = walk_image(img, base)
    got.pop('D /lost+found', None) if 'D /lost+found' not in exp else None
    ok(f'{name}: tipo detectado = {want_fs}', fs == want_fs, f'(veio {fs})')
    missing = [k for k in exp if k not in got]
    extra = [k for k in got if k not in exp]
    wrong = [k for k in exp if k in got and exp[k] != got[k]]
    ok(f'{name}: {len(exp)} itens, nenhum faltando/errado', not missing and not wrong and not extra and not [e for e in errs if e.startswith('ERR ') or e.startswith('stderr')],
       f'falta={missing[:3]} errado={[(k, exp[k], got[k]) for k in wrong[:2]]} sobra={extra[:3]} erros={errs[:3]}')

def diag(img, sec=512):
    r = sh(f'{BIN} {img} diag {sec}', check=False)
    d = {'parts': [], 'found': [], 'v': 0, 'T': {}, 'err': r.stderr}
    for line in r.stdout.splitlines():
        if line.startswith('T '): d['T'] = dict(kv.split('=') for kv in line[2:].split())
        elif line.startswith('P '):
            m = dict(re.findall(r'(\w+)=(\[.*?\]|\S+)', line)); d['parts'].append(m)
        elif line.startswith('L '):
            m = dict(re.findall(r'(\w+)=(\[.*?\]|\S+)', line)); d['found'].append(m)
        elif line.startswith('V '): d['v'] = int(line.split()[1], 16)
    return d

V = dict(HEALTHY=1, TINY=2, READERR=4, BEYOND=8, NOTABLE=16, RAW=32, BADGPT=64, SMALLPART=128, LINUX=256, ENCRYPTED=512, SUPERFLOPPY=1024, UNKNOWNPART=2048)
def has(d, *names): return all(d['v'] & V[n] for n in names)
def hasnt(d, *names): return not any(d['v'] & V[n] for n in names)
FS = dict(FAT12=1, FAT16=2, FAT32=3, EXFAT=4, NTFS=5, EXT2=6, EXT3=7, EXT4=8, SWAP=9, EMPTY=15, UNKNOWN=16)

def mkext(img, size_mb, t, bs, src):
    sh(f'rm -f {img}; truncate -s {size_mb}M {img}; mke2fs -q -F -t {t} -b {bs} -d {src} {img}')

def mkfat(img, size_mb, bits, spc, src):
    sh(f'rm -f {img}; truncate -s {size_mb}M {img}; mkfs.vfat -F {bits} -s {spc} -n TESTFAT {img} >/dev/null')
    sh(f'mcopy -s -m -Q -i {img} {src}/* ::/')

def mkexfat(img, size_mb, src, spc_kb=4):
    sh(f'rm -f {img}; truncate -s {size_mb}M {img}; mkfs.exfat -c {spc_kb}K -L TESTEXF {img} >/dev/null')
    loop = sh(f'losetup -f --show {img}').stdout.strip()
    mnt = f'{W}/mnt'; os.makedirs(mnt, exist_ok=True)
    try:
        sh(f'mount.exfat-fuse {loop} {mnt}')
        sh(f'cp -r {src}/. {mnt}/')
        sh('sync')
    finally:
        sh(f'fusermount3 -u {mnt} || umount {mnt}', check=False)
        sh(f'losetup -d {loop}', check=False)

def assemble(disk, size_mb, table, parts):
    """parts: [(start_sector, size_sectors_or_None, type, imgfile)]"""
    sh(f'rm -f {disk}; truncate -s {size_mb}M {disk}')
    script = f'label: {table}\n' + ''.join(f'start={s}, ' + (f'size={z}, ' if z else '') + f'type={t}\n' for s, z, t, _ in parts)
    subprocess.run(f'sfdisk -q {disk}', shell=True, input=script, text=True, check=True, capture_output=True)
    for s, z, t, f in parts:
        sh(f'dd if={f} of={disk} bs=512 seek={s} conv=notrunc status=none')

def zero_sector0(src, dst):
    shutil.copy(src, dst)
    sh(f'dd if=/dev/zero of={dst} bs=512 count=1 conv=notrunc status=none')

print('== preparando árvores de arquivos')
make_tree(SRC, True); make_tree(SMALL, False)
E_BIG = expected(SRC); E_SMALL = expected(SMALL)

print('== sistemas de arquivos isolados (superfloppy: sem tabela)')
imgs = {}
for name, t, bs in (('ext4_4k', 'ext4', 4096), ('ext4_1k', 'ext4', 1024), ('ext2_1k', 'ext2', 1024), ('ext3_2k', 'ext3', 2048)):
    img = f'{W}/{name}.img'; mkext(img, 64, t, bs, SRC); imgs[name] = img
    check_fs(name, img, 0, E_BIG, FS[t.upper()])
img = f'{W}/fat32_spc1.img'; mkfat(img, 40, 32, 1, SRC); check_fs('FAT32 (cluster 512 B)', img, 0, E_BIG, FS['FAT32']); imgs['fat32'] = img
img = f'{W}/fat32_spc8.img'; mkfat(img, 300, 32, 8, SRC); check_fs('FAT32 (cluster 4 KiB)', img, 0, E_BIG, FS['FAT32'])
img = f'{W}/fat16.img'; mkfat(img, 64, 16, 4, SRC); check_fs('FAT16', img, 0, E_BIG, FS['FAT16'])
img = f'{W}/fat12.img'; mkfat(img, 8, 12, 4, SMALL); check_fs('FAT12', img, 0, E_SMALL, FS['FAT12'])
img = f'{W}/exfat.img'; mkexfat(img, 64, SRC); check_fs('exFAT', img, 0, E_BIG, FS['EXFAT']); imgs['exfat'] = img
img = f'{W}/exfat_512.img'; mkexfat(img, 64, SRC, 1); check_fs('exFAT (cluster 1 KiB)', img, 0, E_BIG, FS['EXFAT'])

print('== cartão de console retro: FAT32 pequena + ext4 (MBR)')
H = f'{W}/handheld.img'
assemble(H, 300, 'dos', [(8192, 81920, 'c', imgs['fat32']), (90112, None, '83', imgs['ext4_4k'])])
d = diag(H)
ok('tabela MBR com 2 partições', d['T'].get('table') == '1' and len(d['parts']) == 2, str(d['T']))
ok('p1 = FAT32 e p2 = ext4', [p['fs'] for p in d['parts']] == ['3', '8'], str([p['fs'] for p in d['parts']]))
ok('veredito: Linux (Windows só mostra a FAT)', has(d, 'LINUX') and hasnt(d, 'RAW', 'NOTABLE', 'TINY', 'READERR'), hex(d['v']))
check_fs('arquivos dentro da ext4 do cartão', H, 90112 * 512, E_BIG, FS['EXT4'])
check_fs('arquivos dentro da FAT32 do cartão', H, 8192 * 512, E_BIG, FS['FAT32'])

print('== tabela de partições apagada: encontrar e reconstruir')
HN = f'{W}/handheld_notable.img'; zero_sector0(H, HN)
d = diag(HN)
ok('sem tabela → NOTABLE com 2 partições encontradas', has(d, 'NOTABLE') and len(d['found']) == 2, f"{hex(d['v'])} found={len(d['found'])}")
if len(d['found']) == 2:
    f1, f2 = d['found']
    ok('início das partições encontradas = originais', (int(f1['start']), int(f2['start'])) == (8192 * 512, 90112 * 512), f"{f1['start']} {f2['start']}")
    ok('tamanho declarado FAT32 = 81920 setores', int(f1['size']) == 81920 * 512, f1['size'])
    ok('tamanho declarado ext4 cobre até o fim da partição', int(f2['size']) == 64 * 1024 * 1024, f2['size'])
r = sh(f'cd {W} && {BIN} {HN} rebuild && cp rebuilt_s0.bin new_s0.bin', check=False)
ok('reconstrução gera MBR', os.path.exists(f'{W}/new_s0.bin'), r.stdout + r.stderr)
HR = f'{W}/handheld_rebuilt.img'; shutil.copy(HN, HR)
sh(f'dd if={W}/new_s0.bin of={HR} bs=512 count=1 conv=notrunc status=none')
d = diag(HR)
ok('tabela reconstruída é lida de novo (2 partições, FAT32+ext4)', len(d['parts']) == 2 and [p['fs'] for p in d['parts']] == ['3', '8'], str([p['fs'] for p in d['parts']]))
ok('ext4 reconstruída abre os arquivos', walk_image(HR, 90112 * 512)[2] == [] , '')

print('== partição pequena num cartão grande')
S = f'{W}/smallpart.img'
assemble(S, 256, 'dos', [(2048, 81920, 'c', imgs['fat32'])])
d = diag(S)
ok('SMALLPART e sem outras falhas', has(d, 'SMALLPART') and hasnt(d, 'LINUX', 'RAW', 'TINY', 'NOTABLE', 'HEALTHY'), hex(d['v']))

print('== cartão saudável (partição ocupa tudo)')
G = f'{W}/healthy.img'; mkfat(G, 128, 32, 1, SRC)
assemble(f'{W}/healthy_disk.img', 130, 'dos', [(2048, None, 'c', G)])
d = diag(f'{W}/healthy_disk.img')
ok('HEALTHY', has(d, 'HEALTHY') and hasnt(d, 'SMALLPART', 'TINY', 'RAW', 'NOTABLE', 'LINUX'), hex(d['v']))

print('== cartão que informa capacidade minúscula (32 MB)')
T = f'{W}/tiny.img'; mkfat(T, 32, 16, 4, SMALL)
d = diag(T)
ok('TINY', has(d, 'TINY'), hex(d['v']))
ok('e ainda assim lê o sistema de arquivos (superfloppy)', d['T'].get('table') == '3', str(d['T']))

print('== cartão em branco (zeros) e com lixo')
Z = f'{W}/zero.img'; sh(f'truncate -s 200M {Z}')
d = diag(Z); ok('zeros → RAW', has(d, 'RAW') and hasnt(d, 'HEALTHY'), hex(d['v']))
J = f'{W}/junk.img'; open(J, 'wb').write(rnd(8 * 1024 * 1024, 1) * 25)
d = diag(J); ok('lixo aleatório → RAW', has(d, 'RAW') and hasnt(d, 'NOTABLE'), hex(d['v']) + str(d['found']))

print('== GPT (exFAT + ext4)')
GP = f'{W}/gpt.img'
assemble(GP, 400, 'gpt', [(2048, 131072, 'EBD0A0A2-B9E5-4433-87C0-68B6B72699C7', imgs['exfat']), (133120, None, '0FC63DAF-8483-4772-8E79-3D69D8477DE4', imgs['ext4_4k'])])
d = diag(GP)
ok('GPT reconhecida, cabeçalhos/CRC/backup ok', d['T'].get('table') == '2' and d['T'].get('gpthdr') == '1' and d['T'].get('gptcrc') == '1' and d['T'].get('gptbak') == '1', str(d['T']))
ok('GPT: exFAT + ext4', [p['fs'] for p in d['parts']] == ['4', '8'], str([p['fs'] for p in d['parts']]))
ok('GPT saudável (LINUX, sem BADGPT)', has(d, 'LINUX') and hasnt(d, 'BADGPT', 'RAW'), hex(d['v']))
check_fs('exFAT dentro da GPT', GP, 2048 * 512, E_BIG, FS['EXFAT'])
GB = f'{W}/gpt_bad.img'; shutil.copy(GP, GB)
sh(f'dd if=/dev/zero of={GB} bs=512 seek=1 count=1 conv=notrunc status=none')
d = diag(GB)
ok('GPT com cabeçalho principal apagado: BADGPT e partições lidas do backup', has(d, 'BADGPT') and len(d['parts']) == 2, f"{hex(d['v'])} n={len(d['parts'])} {d['T']}")

print('== MBR com partição estendida e lógicas')
X = f'{W}/ext.img'
sh(f'rm -f {X}; truncate -s 300M {X}')
subprocess.run(f'sfdisk -q {X}', shell=True, input='label: dos\nstart=2048, size=20480, type=c\nstart=22528, size=20480, type=83\nstart=43008, type=5\nstart=45056, size=20480, type=83\nstart=67584, size=20480, type=83\n', text=True, check=True, capture_output=True)
d = diag(X)
ok('estendida + lógicas: 5 entradas (3 primárias/estendida + 2 lógicas)', len(d['parts']) == 5, str(len(d['parts'])) + ' ' + str([(p['src'], p['mbrtype']) for p in d['parts']]))

print('== partição além do fim do cartão')
BE = f'{W}/beyond.img'; shutil.copy(H, BE); sh(f'truncate -s 100M {BE}')
d = diag(BE)
ok('BEYOND', has(d, 'BEYOND'), hex(d['v']))

print('== setor ilegível/fim do arquivo: leitura não passa do fim')
SH = f'{W}/short.img'; sh(f'truncate -s 3M {SH}')
d = diag(SH); ok('3 MB tolerado', d['v'] != 0 and 'AddressSanitizer' not in d['err'], d['err'][:200])

print('\nRESULTADO:', 'TUDO OK' if fails == 0 else f'{fails} FALHA(S)')
sys.exit(1 if fails else 0)
