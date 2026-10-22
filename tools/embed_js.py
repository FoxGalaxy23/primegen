#!/usr/bin/env python3
"""Встраивает mods/render/webpage/js_prelude.js в C-строку js_prelude.c (построчно).
Запуск из корня проекта:  python3 tools/embed_js.py"""
import os, sys
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'mods', 'render', 'webpage')
src, dst = os.path.join(root, 'js_prelude.js'), os.path.join(root, 'js_prelude.c')
lines = open(src, encoding='utf-8').read().split('\n')
if lines and lines[-1] == '': lines.pop()
esc = lambda l: l.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t').replace('\r', '')
out = ['/* Сгенерировано tools/embed_js.py из js_prelude.js — не править вручную. */', 'const char* webpage_js_prelude =']
out += ['"%s\\n"' % esc(l) for l in lines]
out.append('"\\n"'); out.append(';'); out.append('')
open(dst, 'w', encoding='utf-8').write('\n'.join(out))
print('ok:', len(lines), 'строк')
