# -*- coding: utf-8 -*-
import socket, time

HOST = '192.168.66.71'
URI = 'http://192.168.66.30:8000/test_dlna.wav'

def soap(path, action, body):
    env = ('<?xml version="1.0"?>\n'
           '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
           's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
           '<s:Body>' + body + '</s:Body></s:Envelope>')
    req = ('POST ' + path + ' HTTP/1.1\r\n'
           'Host: ' + HOST + '\r\n'
           'Content-Type: text/xml; charset="utf-8"\r\n'
           'SOAPAction: "' + action + '"\r\n'
           'Content-Length: ' + str(len(env)) + '\r\n'
           'Connection: close\r\n\r\n' + env).encode()
    s = socket.create_connection((HOST, 80), timeout=10)
    s.sendall(req)
    d = b''
    s.settimeout(10)
    try:
        while True:
            x = s.recv(8192)
            if not x:
                break
            d += x
            if len(d) > 6000:
                break
    except socket.timeout:
        pass
    s.close()
    return d

def get(path):
    s = socket.create_connection((HOST, 80), timeout=8)
    s.sendall(('GET ' + path + ' HTTP/1.1\r\nHost: ' + HOST +
               '\r\nConnection: close\r\n\r\n').encode())
    d = b''
    s.settimeout(8)
    try:
        while True:
            x = s.recv(8192)
            if not x:
                break
            d += x
    except socket.timeout:
        pass
    s.close()
    return d

# 1) SetAVTransportURI
r = soap('/upnp/control/avt',
         'urn:schemas-upnp-org:service:AVTransport:1#SetAVTransportURI',
         '<u:SetAVTransportURI xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
         '<InstanceID>0</InstanceID>'
         '<CurrentURI>' + URI + '</CurrentURI>'
         '<CurrentURIMetaData></CurrentURIMetaData>'
         '</u:SetAVTransportURI>')
print('SetAVTransportURI:', r[:80].decode('utf-8', 'replace'))
print('  body:', r.split(b'\r\n\r\n')[-1][:200].decode('utf-8', 'replace') if b'\r\n\r\n' in r else '')
time.sleep(1)

# 2) Play
r = soap('/upnp/control/avt',
         'urn:schemas-upnp-org:service:AVTransport:1#Play',
         '<u:Play xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
         '<InstanceID>0</InstanceID><Speed>1</Speed>'
         '</u:Play>')
print('Play:', r[:80].decode('utf-8', 'replace'))
print('  body:', r.split(b'\r\n\r\n')[-1][:300].decode('utf-8', 'replace') if b'\r\n\r\n' in r else '')
time.sleep(2)

# 3) GetTransportInfo + GetPositionInfo
r = soap('/upnp/control/avt',
         'urn:schemas-upnp-org:service:AVTransport:1#GetTransportInfo',
         '<u:GetTransportInfo xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
         '<InstanceID>0</InstanceID></u:GetTransportInfo>')
print('GetTransportInfo:', r.split(b'\r\n\r\n')[-1][:400].decode('utf-8', 'replace') if b'\r\n\r\n' in r else '')
