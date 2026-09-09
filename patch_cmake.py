import io

path = r"C:\Users\gaufu\Desktop\airplay2\esp32-s3-airplay-dlna-receiver\main\CMakeLists.txt"
with io.open(path, "r", encoding="utf-8") as f:
    src = f.read()

# 1) source files
old1 = '    "dacp_client.c"\n'
new1 = old1 + '    "dlna/source_arbiter.c"\n    "dlna/dlna_renderer.c"\n    "dlna/dlna_upnp.c"\n'
assert src.count(old1) == 1, "anchor src"
src = src.replace(old1, new1)

# 2) include dirs
old2 = '    "usb"\n'
new2 = old2 + '    "dlna"\n'
assert src.count(old2) == 1, "anchor inc"
src = src.replace(old2, new2)

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(src)
print("CMakeLists.txt patched OK")
