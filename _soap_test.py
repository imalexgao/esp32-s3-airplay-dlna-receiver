# -*- coding: utf-8 -*-
"""SOAP test helper: set URI and play, then dump DLNA diag counters."""
import sys, time, urllib.request, json, socket

HOST = '192.168.66.71'
AVT = 'urn:schemas-upnp-org:service:AVTransport:1'

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

def post(action, body):
    return soap('/upnp/control/avt', AVT + '#' + action,
                '<u:' + action + ' xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
                + body + '</u:' + action + '>')

def diag():
    r = json.load(urllib.request.urlopen('http://%s/api/audio/usb' % HOST, timeout=10))
    keys = ('dlna_fmt_diag','dlna_flac_open_rc','dlna_flac_frames','dlna_flac_read_calls',
            'dlna_mp3_frames','dlna_mp3_feed_calls','dlna_feed_calls','dlna_crash_stage',
            'dlna_http_status','dlna_stream_end','dlna_task_alive')
    for k in keys:
        print('  %s = %s' % (k, r.get(k)))

def play(uri, wait=8):
    r = post('Stop', '<InstanceID>0</InstanceID>')
    print('Stop len', len(r))
    time.sleep(0.3)
    body = '<InstanceID>0</InstanceID><CurrentURI>%s</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>' % uri
    print('SetAVTransportURI len', len(post('SetAVTransportURI', body)))
    time.sleep(0.3)
    t0 = time.time()
    r = post('Play', '<InstanceID>0</InstanceID><Speed>1</Speed>')
    print('Play len %d took=%.1fs' % (len(r), time.time() - t0))
    time.sleep(wait)
    print('diag:')
    diag()

if __name__ == '__main__':
    uri = sys.argv[1] if len(sys.argv) > 1 else 'http://192.168.66.30:8000/test_dlna.flac'
    wait = float(sys.argv[2]) if len(sys.argv) > 2 else 8
    play(uri, wait)

