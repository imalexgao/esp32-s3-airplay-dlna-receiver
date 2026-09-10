# -*- coding: utf-8 -*-
import socket

HOST = '192.168.66.71'
body = ('<?xml version="1.0"?>'
        '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
        's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
        '<s:Body><u:GetTransportInfo xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
        '<InstanceID>0</InstanceID></u:GetTransportInfo></s:Body></s:Envelope>')
req = ('POST /upnp/control/avt HTTP/1.1\r\nHost: ' + HOST + '\r\n'
       'Content-Type: text/xml; charset="utf-8"\r\n'
       'SOAPAction: "urn:schemas-upnp-org:service:AVTransport:1#GetTransportInfo"\r\n'
       'Content-Length: ' + str(len(body)) + '\r\n'
       'Connection: close\r\n\r\n' + body).encode()
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
print(d[:700].decode(errors='replace'))
