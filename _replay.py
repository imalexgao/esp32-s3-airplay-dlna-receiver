# -*- coding: utf-8 -*-
import socket, time

HOST = '192.168.66.71'
URI = 'http://m801.music.126.net/20260910111513/83539a25ddd717ea419e3127ef469ca9/jdymusic/obj/wo3DlMOGwrbDjj7DisKw/62224716696/7d33/1164/ae9f/d6edf2548f00bf5ef7989e90ba15c5d8.mp3?vuutv=QlgBPWTQXLPLg8JDcrxt7mVDYZKY2O4tKfhxJ0WwoUssArxY9zFUpNQvwy7VLPJFiHOjOLPH/aQUo2+Woj0q/RnylqaAp3P6NPd+CiKovbg=&dlna=1'

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
time.sleep(6)
r = act('GetTransportInfo', '<InstanceID>0</InstanceID>')
print('TransportInfo:', r[:300])
r = act('GetPositionInfo', '<InstanceID>0</InstanceID>')
import re
m = re.search(r'<RelTime>([^<]+)</RelTime>', r)
print('RelTime:', m.group(1) if m else '?')
