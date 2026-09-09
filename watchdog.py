"""Watchdog: poll device until online, OTA the latest firmware, then run SSDP verification.
Writes a state file so results survive across sessions.
"""
import subprocess, sys, time, os, json, socket, io

HOST = "192.168.66.71"
FW = r"C:\Users\gaufu\Desktop\airplay2\esp32-s3-airplay-dlna-receiver\.pio\build\esp32s3-usbhost\firmware.bin"
STATE = r"C:\Users\gaufu\Desktop\airplay2\esp32-s3-airplay-dlna-receiver\watchdog_state.json"
PS1 = r"C:\Users\gaufu\Desktop\airplay2\esp32-s3-airplay-dlna-receiver\ssdp_test.ps1"

def log(msg):
    line = "[%s] %s" % (time.strftime("%H:%M:%S"), msg)
    print(line)
    with io.open(STATE, "a", encoding="utf-8") as f:
        f.write(line + "\n")

def ping():
    r = subprocess.run(["ping", "-n", "1", "-w", "1500", HOST],
                       capture_output=True, text=True)
    return "Received = 1" in r.stdout or "Received = 2" in r.stdout

def http_ok():
    try:
        s = socket.create_connection((HOST, 80), timeout=5)
        s.sendall(b"GET /description.xml HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n" % HOST.encode())
        data = b""
        while True:
            d = s.recv(4096)
            if not d:
                break
            data += d
        s.close()
        return data.startswith(b"HTTP/1.1 200")
    except Exception:
        return False

def ota():
    r = subprocess.run(["curl.exe", "-s", "--max-time", "150", "-X", "POST",
                        "--data-binary", "@" + FW,
                        "http://%s/api/ota/update" % HOST],
                       capture_output=True, text=True)
    return r.returncode == 0 and "complete" in r.stdout

def ssdp_test():
    r = subprocess.run(["powershell", "-ExecutionPolicy", "Bypass", "-File", PS1],
                       capture_output=True, text=True, timeout=40)
    return r.stdout

with io.open(STATE, "w", encoding="utf-8") as f:
    f.write("watchdog started %s\n" % time.strftime("%Y-%m-%d %H:%M:%S"))

# Phase 1: wait for device
deadline = time.time() + 50 * 60
while time.time() < deadline:
    if ping():
        log("device reachable (ICMP)")
        break
    time.sleep(30)
else:
    log("TIMEOUT: device never came online in 50min")
    sys.exit(1)

# Phase 2: wait for HTTP
t = time.time() + 120
while time.time() < t:
    if http_ok():
        log("HTTP up (description.xml 200)")
        break
    time.sleep(10)
else:
    log("WARN: HTTP not up yet, will try OTA anyway")

# Phase 3: OTA
time.sleep(5)
for attempt in range(3):
    log("OTA attempt %d..." % (attempt + 1))
    if ota():
        log("OTA OK, rebooting")
        break
    log("OTA failed rc, waiting 20s")
    time.sleep(20)
else:
    log("OTA FAILED after 3 attempts")
    sys.exit(1)

# Phase 4: wait for reboot + verify
time.sleep(20)
t = time.time() + 150
while time.time() < t:
    if http_ok():
        log("device back online after OTA")
        break
    time.sleep(10)

# Phase 5: SSDP verification
time.sleep(5)
out = ssdp_test()
log("=== SSDP TEST OUTPUT ===")
for line in out.splitlines():
    log(line)
log("=== END ===")
