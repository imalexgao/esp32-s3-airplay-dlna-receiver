# -*- coding: utf-8 -*-
import socket, time, json

HOST = '192.168.66.71'

def get(path, timeout=6):
    try:
        s = socket.create_connection((HOST, 80), timeout=timeout)
        s.sendall(('GET ' + path + ' HTTP/1.1\r\nHost: ' + HOST +
                   '\r\nConnection: close\r\n\r\n').encode())
        d = b''
        s.settimeout(timeout)
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
    except Exception as e:
        return ('ERR:' + str(e)).encode()

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
    try:
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
        except socket.timeout:
            pass
        s.close()
        return d.split(b'\r\n\r\n')[-1].decode('utf-8', 'replace')
    except Exception as e:
        return 'ERR:' + str(e)

print('=== 1) system info ===')
d = get('/api/system/info')
body = d.split(b'\r\n\r\n')[-1].decode('utf-8', 'replace') if b'\r\n\r\n' in d else d.decode('utf-8', 'replace')
print(body[:800])

print('=== 2) USB audio ===')
d = get('/api/audio/usb')
body = d.split(b'\r\n\r\n')[-1].decode('utf-8', 'replace') if b'\r\n\r\n' in d else d.decode('utf-8', 'replace')
print(body[:800])

print('=== 3) DLNA transport ===')
AVT = 'urn:schemas-upnp-org:service:AVTransport:1'
r = soap('/upnp/control/avt', AVT + '#GetTransportInfo',
         '<u:GetTransportInfo xmlns:u="urn:schemas-upnp-org:service:AVTransport:1"><InstanceID>0</InstanceID></u:GetTransportInfo>')
print(r[:500])
r = soap('/upnp/control/avt', AVT + '#GetPositionInfo',
         '<u:GetPositionInfo xmlns:u="urn:schemas-upnp-org:service:AVTransport:1"><InstanceID>0</InstanceID></u:GetPositionInfo>')
print(r[:700])
