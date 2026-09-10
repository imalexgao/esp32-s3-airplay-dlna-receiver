# -*- coding: utf-8 -*-
"""Set URI + Play via raw SOAP, print response, then dump counters."""
import socket
import time
import urllib.request
import json

HOST = '192.168.66.71'
AVT = 'urn:schemas-upnp-org:service:AVTransport:1'
URI = 'http://192.168.66.30:8000/test_dlna.aac'


def soap(action, body, timeout=8):
    env = ('<?xml version="1.0"?>'
           '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
           's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
           '<s:Body>' + body + '</s:Body></s:Envelope>')
    req = ('POST /upnp/control/avt HTTP/1.1\r\nHost: ' + HOST + '\r\n'
           'Content-Type: text/xml; charset="utf-8"\r\n'
           'SOAPAction: "' + AVT + '#' + action + '"\r\n'
           'Content-Length: ' + str(len(env)) + '\r\n'
           'Connection: close\r\n\r\n' + env).encode()
    s = socket.create_connection((HOST, 80), timeout=timeout)
    s.sendall(req)
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
    return d[:300].decode(errors='replace')


def diag():
    j = json.load(urllib.request.urlopen('http://%s/api/audio/usb' % HOST, timeout=10))
    ks = ('dlna_soap_posts', 'dlna_soap_seturi', 'dlna_soap_play',
          'dlna_seturi_calls', 'dlna_play_calls', 'dlna_stream_play_calls',
          'dlna_fmt_diag', 'dlna_http_status', 'dlna_crash_stage',
          'dlna_aac_open_rc', 'dlna_aac_frames', 'dlna_aac_feed_calls',
          'dlna_stream_end', 'dlna_task_alive')
    return {k: j.get(k) for k in ks}


print('stop   ->', soap('Stop', '<u:Stop xmlns:u="urn:schemas-upnp-org:service:AVTransport:1"><InstanceID>0</InstanceID></u:Stop>'))
print('seturi ->', soap('SetAVTransportURI',
                        '<u:SetAVTransportURI xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
                        '<InstanceID>0</InstanceID><CurrentURI>%s</CurrentURI>'
                        '<CurrentURIMetaData></CurrentURIMetaData></u:SetAVTransportURI>' % URI))
time.sleep(1)
print('play   ->', soap('Play', '<u:Play xmlns:u="urn:schemas-upnp-org:service:AVTransport:1"><InstanceID>0</InstanceID><Speed>1</Speed></u:Play>'))
time.sleep(10)
print('diag   ->', diag())
