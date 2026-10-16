# Passive v3 viewer: Hello 3, collect keyframes, never sends PanelReady/Touch.
import socket, os, base64, struct, sys, time
port=int(sys.argv[1]); out=sys.argv[2]; dur=float(sys.argv[3])
s=socket.create_connection(('127.0.0.1',port)); key=base64.b64encode(os.urandom(16)).decode()
s.sendall(f"GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
buf=b''
while b'\r\n\r\n' not in buf: buf+=s.recv(4096)
buf=buf.split(b'\r\n\r\n',1)[1]
def send(p):
    m=os.urandom(4); s.sendall(bytes([0x82,0x80|len(p)])+m+bytes(b^m[i%4] for i,b in enumerate(p)))
def need(n):
    global buf
    while len(buf)<n:
        d=s.recv(65536)
        if not d: raise EOFError
        buf+=d
def msg():
    global buf
    need(2); b1=buf[1]&127; o=2
    if b1==126: need(4); b1=struct.unpack('>H',buf[2:4])[0]; o=4
    elif b1==127: need(10); b1=struct.unpack('>Q',buf[2:10])[0]; o=10
    need(o+b1); p=buf[o:o+b1]; op=buf[0]&15; buf=buf[o+b1:]; return op,p
send(b'\x01\x03'); s.settimeout(1.0)
end=time.time()+dur; pend={}; n=0
while time.time()<end:
    try: op,p=msg()
    except socket.timeout: continue
    if op!=2 or not p: continue
    if p[0]==0x84: print('source',p[1],p[2],struct.unpack('<9I',p[3:39])[-2:] if p[1] else '')
    if p[0]==0x85:
        gen,seq,led,us,tot,off,cnt=struct.unpack('<IQQQIII',p[1:41])
        b=pend.setdefault(seq,bytearray()); b+=p[41:41+cnt]
        if len(b)==tot:
            open(f'{out}/frame{n:02d}_seq{seq}.png','wb').write(b); n+=1; print('frame',seq,tot); del pend[seq]
s.close()
