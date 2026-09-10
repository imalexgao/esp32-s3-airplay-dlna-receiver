# -*- coding: utf-8 -*-
import socket, time, urllib.request, json

HOST = '192.168.66.71'
URI = 'http://192.168.66.30:8000/test_dlna.mp3'

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
    s = socket.create_connection((HOST, 80), timeout=5)
    s.sendall(req)
    d = b''
    s.settimeout(5)
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

AVT = 'urn:schemas-upnp-org:service:AVTransport:1'
def act(name, inner):
    return soap('/upnp/control/avt', AVT + '#' + name,
                '<u:' + name + ' xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
                + inner + '</u:' + name + '>')

r = act('SetAVTransportURI',
        '<InstanceID>0</InstanceID><CurrentURI>' + URI +
        '</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>')
print('=== SetURI response ===')
print(r.decode('utf-8', 'replace')[:500])
time.sleep(0.5)
r = act('Play', '<InstanceID>0</InstanceID><Speed>1</Speed>')
print('=== Play response ===')
print(repr(r[:200]))
time.sleep(4)
r = json.load(urllib.request.urlopen('http://192.168.66.71/api/audio/usb', timeout=10))
for k in ('dlna_soap_posts', 'dlna_soap_avt_calls', 'dlna_soap_seturi',
          'dlna_soap_play'):
    print(k, '=', r.get(k))
for ep in ('/api/info', '/api/status', '/api/system/info'):
    try:
        r2 = json.load(urllib.request.urlopen('http://192.168.66.71' + ep, timeout=5))
        print(ep, str(r2)[:150])
    except Exception as e:
        print(ep, 'n/a')
