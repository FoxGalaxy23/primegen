#!/usr/bin/env python3
"""Генерирует mods/nal/common/psl_data.inc из Public Suffix List (https://publicsuffix.org/list/, MPL-2.0).
Запуск:  python3 tools/gen_psl.py /путь/к/public_suffix_list.dat
Правила (ICANN и PRIVATE секции) приводятся к punycode и записываются одной строкой с разделителем '\\n'."""
import sys, os
rules = []
for line in open(sys.argv[1], encoding='utf-8'):
    line = line.strip()
    if not line or line.startswith('//'): continue
    line = line.split()[0]
    neg = line.startswith('!'); wild = line.startswith('*.')
    core = line.lstrip('!')
    labels = core.split('.')
    enc = []
    for l in labels:
        if l == '*': enc.append('*'); continue
        try: enc.append(l.encode('idna').decode('ascii'))
        except Exception: enc.append(l)
    rules.append(('!' if neg else '') + '.'.join(enc).lower())
rules = sorted(set(rules))
body = '\\n'.join(rules) + '\\n'
out = '/* Автогенерация tools/gen_psl.py. Public Suffix List (Mozilla Public License 2.0, https://publicsuffix.org/). Правил: %d */\nstatic const char kPslRules[] =\n' % len(rules)
chunk = []; cur = ''
for r in rules:
    if len(cur) + len(r) > 100: chunk.append(cur); cur = ''
    cur += r + '\\n'
chunk.append(cur)
out += '\n'.join('"%s"' % c for c in chunk) + ';\n'
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'mods', 'nal', 'common', 'psl_data.inc'), 'w').write(out)
print('правил:', len(rules))
