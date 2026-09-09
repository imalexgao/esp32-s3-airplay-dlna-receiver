import io

path = r"C:\Users\gaufu\Desktop\airplay2\esp32-s3-airplay-dlna-receiver\main\main.c"
with io.open(path, "r", encoding="utf-8") as f:
    src = f.read()

# 1) init calls after led_init()
old1 = "  led_init();\n"
new1 = old1 + "#ifdef CONFIG_DLNA_ENABLE\n  dlna_renderer_init();\n  source_arbiter_init();\n#endif\n"
assert src.count(old1) == 1, "anchor1"
src = src.replace(old1, new1)

# 2) includes — add after last main-local include line
old2 = '#include "spiffs_storage.h"\n'
new2 = old2 + '#ifdef CONFIG_DLNA_ENABLE\n#include "dlna/dlna_renderer.h"\n#include "dlna/source_arbiter.h"\n#endif\n'
if old2 in src:
    src = src.replace(old2, new2, 1)
else:
    print("WARN: spiffs_storage.h include not found, skipping includes")

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(src)
print("main.c patched OK")
