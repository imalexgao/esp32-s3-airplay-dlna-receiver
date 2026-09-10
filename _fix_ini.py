# -*- coding: utf-8 -*-
"""Add build_src_filter to platformio.ini [env:esp32s3-usbhost]."""
p = 'platformio.ini'
b = open(p, 'rb').read()
dollar = chr(36).encode()
needle = (b'build_flags =\r\n    ' + dollar + b'{env.build_flags}\r\n'
          b'    -DUSE_DEFAULT_STDLIB\r\n')
assert needle in b, 'build_flags needle not found'
repl = (needle +
        b'build_src_filter =\r\n'
        b'    +<*> -<.git/> -<.svn/> +<dlna/codecs/libhelix-aac/*.c>\r\n')
b2 = b.replace(needle, repl, 1)
open(p, 'wb').write(b2)
i = b2.find(b'build_src_filter')
print(repr(b2[i:i+100]))
