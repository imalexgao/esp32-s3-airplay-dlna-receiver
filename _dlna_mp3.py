# -*- coding: utf-8 -*-
import socket, time

HOST = '192.168.66.71'
URI = 'http://192.168.66.30:8000/test_dlna.mp3'

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
            if len(d) > 3000:
                break
    except socket.timeout:
        pass
    s.close()
    body_txt = d.split(b'\r\n\r\n')[-1].decode('utf-8', 'replace')
    return d[:40].decode('utf-8', 'replace'), body_txt[:200]

AVT = 'urn:schemas-upnp-org:service:AVTransport:1'

# stop first (clear any state)
r1, b1 = soap('/upnp/control/avt', AVT + '#Stop',
              '<u:Stop xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
              '<InstanceID>0</InstanceID></u:Stop>')
print('Stop:', r1)
time.sleep(1)

r1, b1 = soap('/upnp/control/avt', AVT + '#SetAVTransportURI',
              '<u:SetAVTransportURI xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
              '<InstanceID>0</InstanceID>'
              '<CurrentURI>' + URI + '</CurrentURI>'
              '<CurrentURIMetaData></CurrentURIMetaData>'
              '</u:SetAVTransportURI>')
print('SetAVTransportURI(mp3):', r1, '|', b1[:60])
time.sleep(1)

r1, b1 = soap('/upnp/control/avt', AVT + '#Play',
              '<u:Play xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
              '<InstanceID>0</InstanceID><Speed>1</Speed></u:Play>')
print('Play(mp3):', r1)
time.sleep(8)

r1, b1 = soap('/upnp/control/avt', AVT + '#GetTransportInfo',
              '<u:GetTransportInfo xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
              '<InstanceID>0</InstanceID></u:GetTransportInfo>')
print('TransportInfo:', b1[:200])
