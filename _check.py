# -*- coding: utf-8 -*-
import socket, re, urllib.request

HOST = '192.168.66.71'

def soap(path, action, body):
    env = ('<?xml version="1.0"?>'
           '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
           's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
           '<s:Body>' + body + '</s:Body></s:Envelope>')
    req = ('POST ' + path + ' HTTP/1.1\r\nHost: ' + HOST + '\r\n'
           'Content-Type: text/xml; charset="utf-8"\r\n'
           'SOAPAction: "' + action + '"\r\n'
           'Content-Length: ' + str(len(env)) + '\r\n'
           'Connection: close\r\n\r\n' + env).encode()
    s = socket.create_connection((HOST, 80), timeout=6)
    s.sendall(req)
    d = b''
    s.settimeout(6)
    try:
        while True:
            x = s.recv(8192)
            if not x:
                break
            d += x
    except Exception:
        pass
    s.close()
    return d.split(b'\r\n\r\n')[-1].decode('utf-8', 'replace')

r = soap('/upnp/control/avt',
         'urn:schemas-upnp-org:service:AVTransport:1#GetPositionInfo',
         '<u:GetPositionInfo xmlns:u="urn:schemas-upnp-org:service:AVTransport:1"><InstanceID>0</InstanceID></u:GetPositionInfo>')
m = re.search(r'<RelTime>([^<]+)</RelTime>', r)
print('RelTime now:', m.group(1) if m else '?')
m = re.search(r'<TrackDuration>([^<]+)</TrackDuration>', r)
print('TrackDuration:', m.group(1) if m else '?')
u = urllib.request.urlopen('http://192.168.66.71/api/audio/usb', timeout=5)
print('USB:', u.read(500).decode('utf-8', 'replace'))
