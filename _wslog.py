# -*- coding: utf-8 -*-
import sys, time
sys.path.insert(0, r'C:\Users\gaufu\.platformio\penv\Lib\site-packages')
import websocket

URL = 'ws://192.168.66.71/ws/logs'
KWS = ['dlna', 'stream', 'mp3', 'http', 'error', 'usb', 'audio', 'decode', 'resp', 'recv', 'resample', 'source', 'feed']

def on_message(ws, message):
    msg = str(message)
    low = msg.lower()
    if any(k in low for k in KWS):
        print(msg[:500])

def run():
    ws = websocket.WebSocketApp(URL, on_message=on_message,
                                on_error=lambda w, e: print('WS ERR', e),
                                on_close=lambda w, a, b: print('WS closed'))
    print('connecting to', URL)
    ws.run_forever(ping_interval=10, ping_timeout=5)

while True:
    try:
        run()
    except Exception as e:
        print('ws run exception:', e)
    print('reconnecting in 2s...')
    time.sleep(2)
