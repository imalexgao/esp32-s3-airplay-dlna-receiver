# -*- coding: utf-8 -*-
import urllib.request, time, sys

FW = r'C:\Users\gaufu\Desktop\airplay2\esp32-s3-airplay-dlna-receiver\.pio\build\esp32s3-usbhost\firmware.bin'
HOST = '192.168.66.71'
URL = 'http://%s/api/ota/update' % HOST

data = open(FW, 'rb').read()
print('firmware bytes:', len(data))

req = urllib.request.Request(URL, data=data, headers={'Content-Type': 'application/octet-stream'}, method='POST')
try:
    resp = urllib.request.urlopen(req, timeout=60)
    print('OTA resp status:', resp.status)
    print('body:', resp.read(300).decode('utf-8', 'replace'))
except Exception as e:
    print('OTA err:', e)

print('waiting 45s for boot...')
time.sleep(45)

# check device back
for i in range(6):
    try:
        r = urllib.request.urlopen('http://%s/api/system/info' % HOST, timeout=5)
        b = r.read(600).decode('utf-8', 'replace')
        print('device back, resp:', b[:300])
        break
    except Exception as e:
        print('not yet (%d): %s' % (i, e))
        time.sleep(10)
