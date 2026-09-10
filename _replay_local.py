# -*- coding: utf-8 -*-
import socket, time, re

HOST = '192.168.66.71'
URI = 'http://192.168.66.30:8000/test_dlna.wav'

def soap(path, action, body):
    env = ('<?xml version="1.0"?>\n'
           '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
           's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
           '<s:Body>' + body + '</s:Body></s:Envelope>')
    req = ('POST ' + path + ' HTTP/1.1\r\nHost: ' + HOST + '\r\n'
           'Content-Type: text/xml; charset="utf-8"\r\n'
           'SOAPAction: "' + action + '"\r\n'
           'Content-Length: ' + str(len(env)) + '\r\n'
           'Connection: close\r\n\r\n' + env).encode()
    s = socket.create_connection((HOST, 80), timeout=8)
    s.sendall(req)
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
    return d.split(b'\r\n\r\n')[-1].decode('utf-8', 'replace')

AVT = 'urn:schemas-upnp-org:service:AVTransport:1'

def act(name, inner):
    return soap('/upnp/control/avt', AVT + '#' + name,
                '<u:' + name + ' xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
                + inner + '</u:' + name + '>')

print('Stop:', act('Stop', '<InstanceID>0</InstanceID>')[:80].replace('\n', ' '))
time.sleep(1)
print('SetURI:', act('SetAVTransportURI',
                     '<InstanceID>0</InstanceID><CurrentURI>' + URI +
                     '</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>')[:80].replace('\n', ' '))
time.sleep(1)
print('Play:', act('Play', '<InstanceID>0</InstanceID><Speed>1</Speed>')[:80].replace('\n', ' '))
time.sleep(8)
r = act('GetTransportInfo', '<InstanceID>0</InstanceID>')
print('TransportInfo:', r[:300])
r = act('GetPositionInfo', '<InstanceID>0</InstanceID>')
m = re.search(r'<RelTime>([^<]+)</RelTime>', r)
print('RelTime:', m.group(1) if m else '?')
