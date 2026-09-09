# -*- coding: utf-8 -*-
import io
p = r'C:\Users\gaufu\Desktop\airplay2\esp32-s3-airplay-dlna-receiver\main\dlna\dlna_stream.c'
lines = io.open(p, encoding='utf-8').read().split('\n')
stack = []
for i, l in enumerate(lines, 1):
    in_str = False
    in_ch = False
    j = 0
    while j < len(l):
        c = l[j]
        if in_str:
            if c == '\\' and j + 1 < len(l):
                j += 2
                continue
            if c == '"':
                in_str = False
            j += 1
            continue
        if in_ch:
            if c == '\\' and j + 1 < len(l):
                j += 2
                continue
            if c == "'":
                in_ch = False
            j += 1
            continue
        if c == '"':
            in_str = True
            j += 1
            continue
        if c == "'":
            in_ch = True
            j += 1
            continue
        if c == '/' and j + 1 < len(l) and l[j + 1] == '/':
            break
        if c == '{':
            stack.append(i)
        elif c == '}':
            if stack:
                op = stack.pop()
                print('%-5d {  ->  %-5d }' % (op, i))
            else:
                print('EXTRA } at', i)
        j += 1
for op in stack:
    print('UNCLOSED { at line', op)
