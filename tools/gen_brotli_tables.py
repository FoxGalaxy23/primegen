#!/usr/bin/env python3
"""Генерирует mods/render/ttf_render/brotli_tables.inc из исходников эталонной реализации Brotli (MIT, Google):
словарь (RFC 7932, приложение A), таблицы преобразований слов (приложение B) и таблицу контекстов literal'ов.
Эталон нужен только для генерации и сверки (pip download brotli --no-binary :all:), в проект он не входит.
Запуск:  python3 tools/gen_brotli_tables.py /путь/к/brotli-X.Y.Z/c"""
import re, sys, os
c = sys.argv[1]
def read(p): return open(os.path.join(c, p), encoding='utf-8', errors='replace').read()

def arr(src, name):
    m = re.search(name + r'[^=]*=\s*\{(.*?)\};', src, re.S); return m.group(1)
def nums(body, names=None):
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    out = []
    for t in re.split(r'[,\s]+', body.strip()):
        if not t: continue
        if names and t in names: out.append(names[t])
        else: out.append(int(t, 0))
    return out

dic = nums(arr(read('common/dictionary_inc.h'), r'kBrotliDictionaryData\[\]'))
assert len(dic) == 122784, len(dic)
ts = read('common/transform.c')
enum = {n: v for n, v in re.findall(r'(BROTLI_TRANSFORM_\w+)\s*=\s*(\d+)', read('common/transform.h'))}
enum = {k: int(v) for k, v in enum.items()}
trans = nums(arr(ts, r'kTransformsData\[\]'), enum); assert len(trans) == 121 * 3, len(trans)
pmap = nums(arr(ts, r'kPrefixSuffixMap\[50\]')); assert len(pmap) == 50
# строка kPrefixSuffix: склейка C-литералов с экранированием
ps_src = re.search(r'kPrefixSuffix\[217\]\s*=(.*?);', ts, re.S).group(1)
ps_src = re.sub(r'/\*.*?\*/', '', ps_src, flags=re.S)
lits = re.findall(r'"((?:[^"\\]|\\.)*)"', ps_src)
raw = bytearray()
for lit in lits:
    i = 0
    while i < len(lit):
        ch = lit[i]
        if ch != '\\': raw.append(ord(ch)); i += 1; continue
        i += 1; e = lit[i]
        if e in 'ntr': raw.append({'n': 10, 't': 9, 'r': 13}[e]); i += 1
        elif e in '"\'\\': raw.append(ord(e)); i += 1
        elif e == 'x':
            j = i + 1; h = ''
            while j < len(lit) and lit[j] in '0123456789abcdefABCDEF': h += lit[j]; j += 1
            raw.append(int(h, 16) & 255); i = j
        elif e in '01234567':
            j = i; o = ''
            while j < len(lit) and len(o) < 3 and lit[j] in '01234567': o += lit[j]; j += 1
            raw.append(int(o, 8)); i = j
        else: raise SystemExit('escape ' + e)
assert len(raw) == 216, len(raw)
raw.append(0)
ctx = nums(arr(read('common/context.c'), r'_kBrotliContextLookupTable\[2048\]')); assert len(ctx) == 2048

def fmt(name, vals, per=20):
    lines = [', '.join(str(v) for v in vals[i:i + per]) for i in range(0, len(vals), per)]
    return 'static const unsigned char %s[%d] = {\n%s\n};\n' % (name, len(vals), ',\n'.join(lines))
out = '/* Автогенерация tools/gen_brotli_tables.py. Данные — из RFC 7932 (словарь и преобразования); источник: эталонная реализация Brotli,\n   Copyright 2013 Google Inc., лицензия MIT (https://opensource.org/licenses/MIT). */\n'
out += fmt('kBrDict', dic, 24) + fmt('kBrPrefixSuffix', list(raw), 24)
out += 'static const unsigned short kBrPrefixSuffixMap[50] = {' + ', '.join(map(str, pmap)) + '};\n'
out += fmt('kBrTransforms', trans, 30) + fmt('kBrCtxLut', ctx, 32)
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'mods', 'render', 'ttf_render', 'brotli_tables.inc'), 'w').write(out)
print('ok', len(out))
