import io

path = r"C:\Users\gaufu\Desktop\airplay2\esp32-s3-airplay-dlna-receiver\main\Kconfig.projbuild"
with io.open(path, "r", encoding="utf-8") as f:
    src = f.read()

anchor = '    menu "LED Configuration"\n'
add = (
    '    config DLNA_ENABLE\n'
    '        bool "Enable DLNA/UPnP Media Renderer (AirPlay + DLNA dual source)"\n'
    '        default y\n'
    '        help\n'
    '            Adds a UPnP AV MediaRenderer (SSDP discovery + SOAP control)\n'
    '            so Android apps (BubbleUPnP / Hi-Fi Cast) can stream audio to\n'
    '            the device, alongside AirPlay. The source arbiter enforces a\n'
    '            single active source (last-writer-wins).\n'
    '\n'
)
assert src.count(anchor) == 1, "anchor not found"
src = src.replace(anchor, add + anchor, 1)

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(src)
print("Kconfig.projbuild patched OK")
