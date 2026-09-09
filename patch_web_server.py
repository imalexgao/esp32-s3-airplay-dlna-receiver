import io, sys

path = r"C:\Users\gaufu\Desktop\airplay2\esp32-s3-airplay-dlna-receiver\main\network\web_server.c"
with io.open(path, "r", encoding="utf-8") as f:
    src = f.read()

# 1) URI handler budget
old1 = "  config.max_uri_handlers += 6; // /api/audio/format + /api/ui/lang + /api/audio/latency (get/post each)\n"
new1 = old1 + "#ifdef CONFIG_DLNA_ENABLE\n  config.max_uri_handlers += 7; // DLNA: description + 2 SCPD + 2 control + 2 event\n#endif\n"
assert src.count(old1) == 1, "anchor1 not unique/found"
src = src.replace(old1, new1)

# 2) register DLNA endpoints after log_stream_register
old2 = "  log_stream_register(s_server);\n"
new2 = old2 + "\n#ifdef CONFIG_DLNA_ENABLE\n  dlna_upnp_register(s_server);\n  dlna_upnp_start_ssdp();\n#endif\n"
assert src.count(old2) == 1, "anchor2 not unique/found"
src = src.replace(old2, new2)

# 3) include dlna_upnp.h at top (after last include line)
old3 = '#include "log_stream.h"\n'
new3 = old3 + '#include "dlna/dlna_upnp.h"\n'
if old3 in src:
    src = src.replace(old3, new3, 1)
else:
    # fallback: insert before first static definition
    print("WARN: log_stream.h include not found, skipping include add")

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(src)
print("web_server.c patched OK")
