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
    return d

AVT = 'urn:schemas-upnp-org:service:AVTransport:1'
def act(name, inner):
    return soap('/upnp/control/avt', AVT + '#' + name,
                '<u:' + name + ' xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
                + inner + '</u:' + name + '>')

r = act('Stop', '<InstanceID>0</InstanceID>')
print('Stop len', len(r))
time.sleep(0.5)
r = act('SetAVTransportURI',
        '<InstanceID>0</InstanceID><CurrentURI>' + URI +
        '</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>')
print('SetURI len', len(r))
time.sleep(0.5)
t0 = time.time()
r = act('Play', '<InstanceID>0</InstanceID><Speed>1</Speed>')
print('Play len', len(r), 'took %.1fs' % (time.time() - t0))
time.sleep(6)
r = json.load(urllib.request.urlopen('http://192.168.66.71/api/audio/usb', timeout=10))
for k in ('dlna_soap_posts', 'dlna_soap_avt_calls', 'dlna_soap_seturi',
          'dlna_soap_play', 'dlna_seturi_calls', 'dlna_play_calls',
          'dlna_stream_play_calls', 'dlna_task_alive', 'dlna_play_last_err',
          'dlna_http_status', 'dlna_http_err'):
    print(k, '=', r.get(k))
