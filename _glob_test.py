# -*- coding: utf-8 -*-
import subprocess, tempfile, os
d = tempfile.mkdtemp()
os.makedirs(os.path.join(d, 'main', 'dlna', 'codecs', 'libhelix-aac'), exist_ok=True)
open(os.path.join(d, 'main', 'a.c'), 'w').write('')
open(os.path.join(d, 'main', 'dlna', 'b.c'), 'w').write('')
open(os.path.join(d, 'main', 'dlna', 'codecs', 'libhelix-aac', 'c.c'), 'w').write('')
cmake = None
for cand in [r'C:\Users\gaufu\.platformio\packages\tool-cmake\bin\cmake.exe',
             r'C:\Users\gaufu\.platformio\packages\tool-cmake\bin\cmake3.exe']:
    if os.path.exists(cand):
        cmake = cand
        break
if cmake is None:
    cmake = 'cmake'
code = ('file(GLOB_RECURSE s main/*.*)\n'
        'foreach(x IN LISTS s)\n'
        '  message(STATUS "GOT ${x}")\n'
        'endforeach()\n')
open(os.path.join(d, 'CMakeLists.txt'), 'w').write(
    'cmake_minimum_required(VERSION 3.16)\nproject(t)\n' + code)
r = subprocess.run([cmake, '-S', d, '-B', os.path.join(d, 'bld')],
                   capture_output=True, text=True)
print(r.stdout)
if r.stderr:
    print('ERR', r.stderr[-500:])
