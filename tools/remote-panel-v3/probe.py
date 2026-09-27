#!/usr/bin/env python3
"""V3 reference receiver and acceptance probe. No firmware access.
Requires Pillow for PNG decoding. --self-test needs no network or GUI.
All logs/images are written beneath the explicit --output directory.
"""
import argparse, base64, hashlib, io, json, os, socket, struct, time
from pathlib import Path
from PIL import Image, ImageChops

class Ws:
    def __init__(self, host, port):
        self.s=socket.create_connection((host,port),5)
        self.s.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
        key=base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f'GET / HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
        header=b''
        while not header.endswith(b'\r\n\r\n'): header+=self.exact(1)
        if b' 101 ' not in header.split(b'\r\n')[0]: raise RuntimeError(header)
    def exact(self,n):
        b=bytearray()
        while len(b)<n:
            part=self.s.recv(n-len(b))
            if not part: raise EOFError('WebSocket closed')
            b.extend(part)
        return bytes(b)
    def send(self,b):
        n=len(b); mask=os.urandom(4)
        head=bytes([0x82,0x80|n]) if n<126 else bytes([0x82,0xfe])+struct.pack('>H',n)
        self.s.sendall(head+mask+bytes(v^mask[i%4] for i,v in enumerate(b)))
    def recv(self,timeout):
        self.s.settimeout(timeout)
        try: first=self.s.recv(1)
        except socket.timeout: return None,None
        if not first: raise EOFError('WebSocket closed')
        self.s.settimeout(5)
        second=self.exact(1)[0]; n=second&127
        if n==126: n=struct.unpack('>H',self.exact(2))[0]
        if n==127: n=struct.unpack('>Q',self.exact(8))[0]
        if n>3*1024*1024: raise ValueError('Oversize WebSocket payload')
        if second&128: raise ValueError('Masked server payload')
        return first[0]&15,self.exact(n)

class Decoder:
    def __init__(self):
        self.generation=0; self.available=False; self.geometry=None
        self.capture=0; self.pending=None; self.image=None
    def source(self,b):
        if len(b)!=43: raise ValueError('PanelSource length')
        _,state,reason,gen,ox,oy,w,h,scale,cw,ch,rw,rh=struct.unpack('<BBBI5f4I',b)
        if gen!=self.generation or not state:
            self.pending=None; self.image=None; self.capture=0
        self.generation=gen; self.available=bool(state)
        self.geometry=dict(origin=[ox,oy],panel=[w,h],scale=scale,context=[cw,ch],raster=[rw,rh],reason=reason)
        return self.geometry
    def chunk(self,b):
        if len(b)<41: raise ValueError('short PanelChunk')
        _,gen,seq,led,host,total,offset,count=struct.unpack_from('<BIQQQIII',b)
        if count>16384 or len(b)!=41+count or total>2*1024*1024 or offset+count>total:
            raise ValueError('invalid PanelChunk bounds')
        if not self.available or gen!=self.generation: return None
        if offset==0: self.pending=(seq,total,bytearray())
        if not self.pending or self.pending[0]!=seq or self.pending[1]!=total or len(self.pending[2])!=offset:
            raise ValueError('noncontiguous keyframe')
        self.pending[2].extend(b[41:])
        if offset+count<total: return None
        png=bytes(self.pending[2]); image=Image.open(io.BytesIO(png)).convert('RGBA'); image.load()
        if list(image.size)!=self.geometry['raster']: raise ValueError('PNG geometry mismatch')
        previous=self.image
        # Compare RGB: RGBA alpha diff is identically zero for opaque panels.
        bbox=ImageChops.difference(image.convert('RGB'),previous.convert('RGB')).getbbox() if previous else None
        self.image=image; self.capture=seq; self.pending=None
        return dict(capture=seq,firmware_led_sequence_sampled_at_request=led,host_completed_us=host,
                    png_bytes=len(png),pixel_sha256=hashlib.sha256(image.tobytes()).hexdigest(),changed_bbox=bbox),png
    def panel_to_raster(self,x,y):
        g=self.geometry; ox,oy=g['origin']; cw,ch=g['context']; rw,rh=g['raster']
        return ((ox+x*g['scale'])*rw/cw,(oy+y*g['scale'])*rh/ch)

class Probe:
    def __init__(self,args):
        self.ws=Ws(args.host,args.port); self.decoder=Decoder(); self.seq=0; self.args=args
        self.out=Path(args.output); self.out.mkdir(parents=True,exist_ok=True)
        self.log=(self.out/'frames.jsonl').open('w'); self.controls={}; self.acks={}; self.vram=bytearray(1024)
        self.leds=bytes(14); self.firmware_seq=0; self.frames=0; self.panel_bytes=0; self.v2bytes=0
        self.last_record=None; self.first=True; self.timings=[]; self.visual_probe=None; self.visual_results=[]
        self.ws.send(b'\x01\x03')
    def pump(self,seconds):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            op,b=self.ws.recv(max(.001,end-time.monotonic()))
            if b is None: continue
            if op==1:
                obj=json.loads(b)
                if obj.get('t')=='panelControls': self.controls={x['id']:x for x in obj['controls']}
                continue
            if op!=2 or not b: continue
            t=b[0]
            if t==0x81: print('INFO',b.hex(),flush=True)
            elif t==0x84: print('SOURCE',self.decoder.source(b), 'generation',self.decoder.generation,flush=True)
            elif t==0x85:
                self.panel_bytes+=len(b); completed=self.decoder.chunk(b)
                if completed:
                    record,png=completed; self.frames+=1
                    record.update(received_monotonic=time.monotonic(),v2_snapshot_sequence=self.firmware_seq,
                                  lcd_sha256=hashlib.sha256(self.vram).hexdigest(),led_banks=self.leds.hex())
                    self.last_record=record
                    if self.visual_probe:
                        seq,box,baseline=self.visual_probe
                        ack=self.acks.get(seq)
                        current=hashlib.sha256(self.decoder.image.crop(box).tobytes()).hexdigest()
                        if ack and ack['status']==1 and current!=baseline and record['host_completed_us']>=ack['enqueued_us']:
                            result=dict(seq=seq,control='btFunction',region=box,baseline_sha256=baseline,pressed_sha256=current,
                                        enqueue_to_capture_complete_us=record['host_completed_us']-ack['enqueued_us'],capture=record['capture'])
                            print('VISIBLE_PRESS',json.dumps(result),flush=True); self.visual_results.append(result); self.visual_probe=None
                    self.log.write(json.dumps(record)+'\n'); self.log.flush()
                    self.ws.send(struct.pack('<BIQ',0x15,self.decoder.generation,self.decoder.capture))
                    if self.first:
                        (self.out/'first.png').write_bytes(png); self.first=False
                        image=self.decoder.image
                        print('FIRST',json.dumps(dict(dimensions=image.size,distinct_colours=len(set(image.getdata())),**record)),flush=True)
            elif t==0x82:
                self.v2bytes+=len(b); self.firmware_seq,epoch,written,tiles=struct.unpack_from('<IIHH',b,1)
                self.leds=b[13:27]; offset=27
                for tile in range(16):
                    if tiles&(1<<tile): self.vram[tile*64:(tile+1)*64]=b[offset:offset+64]; offset+=64
                if offset!=len(b): raise ValueError('v2 frame size')
            elif t==0x86:
                _,seq,status,slot,gen,recv,resolved,enqueued,owners,detents=struct.unpack('<BIBBIQQQIi',b)
                ack=dict(seq=seq,status=status,slot=slot,generation=gen,received_us=recv,resolved_us=resolved,
                         enqueued_us=enqueued,receive_to_resolve_us=resolved-recv,resolve_to_enqueue_us=enqueued-resolved,
                         owners=owners,detents=detents)
                self.acks[seq]=ack; self.timings.append(ack); print('TOUCH_ACK',json.dumps(ack),flush=True)
            elif t==0x83:
                seq,status,epoch,host=struct.unpack_from('<IBII',b,1)
                self.acks[seq]=dict(seq=seq,status=status,host_us=host)
    def centre(self,name):
        c=self.controls[name]; return c['x']+c['w']/2,c['y']+c['h']/2
    def touch(self,name,phase,contact=1,delta=(0,0),generation=None):
        self.seq+=1; x,y=self.centre(name); x+=delta[0]; y+=delta[1]
        self.ws.send(struct.pack('<BBHIIff',0x14,phase,contact,self.seq,
                                 self.decoder.generation if generation is None else generation,x,y))
        return self.seq
    def button(self,slot,down,contact=500):
        self.seq+=1; self.ws.send(struct.pack('<BBBBHId',0x10,slot,down,0,contact,self.seq,time.monotonic()*1000)); return self.seq
    def pulse(self,name,touch=True):
        if touch: self.touch(name,0)
        else: self.button(self.controls[name]['slot'],1)
        self.pump(.55)
        if touch: self.touch(name,2)
        else: self.button(self.controls[name]['slot'],0)
        self.pump(.65)
    def state(self): return bytes(self.vram),self.leds
    def measure(self,label,seconds,action=None):
        before=(self.panel_bytes,self.frames,self.v2bytes); start=time.monotonic()
        if action: action()
        self.pump(max(0,seconds-(time.monotonic()-start))); elapsed=time.monotonic()-start
        print('RATE',json.dumps(dict(case=label,seconds=elapsed,panel_bytes_per_s=(self.panel_bytes-before[0])/elapsed,
              panel_frames_per_s=(self.frames-before[1])/elapsed,v2_bytes_per_s=(self.v2bytes-before[2])/elapsed)),flush=True)
    def run(self):
        self.pump(5)
        if not self.decoder.image or not self.controls: raise RuntimeError('No fresh keyframe/control map after 5 seconds')
        print('CONTROLS',json.dumps(self.controls,sort_keys=True),flush=True)
        # Coordinates are panel dp; regions are sampled from the actual received raster.
        for label,x,y in [('label',100,10),('knob',155,170),('LCD',470,185),('LED',65,450)]:
            px,py=self.decoder.panel_to_raster(x,y); px=int(px); py=int(py)
            crop=self.decoder.image.crop((max(0,px-4),max(0,py-4),min(self.decoder.image.width,px+5),min(self.decoder.image.height,py+5)))
            print('SAMPLE',label,'panel_dp',(x,y),'raster',(px,py),'rgba',self.decoder.image.getpixel((px,py)),
                  'region_colours',len(set(crop.getdata())),'region_sha256',hashlib.sha256(crop.tobytes()).hexdigest(),flush=True)
        self.pulse('btStop'); self.pulse('btExit')
        c=self.controls['btFunction']; left,top=self.decoder.panel_to_raster(c['x']+2,c['y']+2)
        right,bottom=self.decoder.panel_to_raster(c['x']+c['w']-2,c['y']+c['h']-2)
        box=tuple(map(int,(left,top,right,bottom)))
        baseline=hashlib.sha256(self.decoder.image.crop(box).tobytes()).hexdigest()
        seq=self.touch('btFunction',0,91); self.visual_probe=(seq,box,baseline); self.pump(1)
        self.touch('btFunction',2,91); self.pump(.5)
        print('VISIBLE_PRESS samples',len(self.visual_results),'release_restored',hashlib.sha256(self.decoder.image.crop(box).tobytes()).hexdigest()==baseline,flush=True)
        self.visual_probe=None
        self.measure('stopped idle (may blink)',30)
        self.measure('tempo button press',5,lambda:self.pulse('btTempo'))
        self.pulse('btExit'); self.measure('pattern playing / inspect LCD change records',30,lambda:self.pulse('btPlay'))
        self.pulse('btStop')
        for name in ('btTempo','btKit','btScale'):
            self.pulse('btExit'); before_button=self.state(); self.pulse(name,False); after_button=self.state()
            self.pulse('btExit'); before_touch=self.state(); self.pulse(name,True); after_touch=self.state()
            baseline=before_button==before_touch
            print('PARITY',name,'equivalent_baseline',baseline,'LCD_equal',after_button[0]==after_touch[0],
                  'LED_equal',after_button[1]==after_touch[1], 'verdict', 'PASS' if baseline and after_button==after_touch else 'INCONCLUSIVE/FAIL',flush=True)
            self.pulse('btExit')
        for names in [('trigKey0','trigKey1'),('trigKey0','trigKey0')]:
            seq=[]
            seq.append(self.touch(names[0],0,11)); self.pump(.3)
            seq.append(self.touch(names[1],0,12)); self.pump(.3)
            seq.append(self.touch(names[0],2,11)); self.pump(.3)
            remaining=self.touch(names[1],1,12); self.pump(.1)
            print('REMAINING_CONTACT',names[1],self.acks.get(remaining),flush=True)
            seq.append(self.touch(names[1],2,12)); self.pump(.3)
            owners=[self.acks.get(s,{}).get('owners') for s in seq]
            expected=[1,2,1,0] if names[0]==names[1] else [1,1,0,0]
            print('OWNERSHIP',names,'owners',owners,'expected',expected,'PASS' if owners==expected else 'FAIL',flush=True)
        # Accumulated fractional moves plus reversal; use the emitted detents for exact v2 replay.
        self.touch('encA',0,21); self.pump(.05); emitted=[]
        for dx in (0.1,0.3,1.2,3.0,1.0,0.0):
            seq=self.touch('encA',1,21,(dx,0)); self.pump(.05); emitted.append(self.acks.get(seq,{}).get('detents'))
        self.touch('encA',2,21); self.pump(.1)
        print('ENCODER Touch detents',emitted,'quick tap/drag press owners',[t['owners'] for t in self.timings if t['slot']==64],flush=True)
        # Replay only nonzero detents. This proves the requested sequence, not firmware equivalence.
        for detent in emitted:
            if detent:
                self.seq+=1; self.ws.send(struct.pack('<BBBBHId',0x11,0,detent&255,0,21,self.seq,time.monotonic()*1000)); self.pump(.05)
        self.touch('encA',0,22); self.pump(.5); seq=self.touch('encA',1,22); self.pump(.1)
        print('ENCODER stationary hold owners',self.acks.get(seq,{}).get('owners'),flush=True)
        self.touch('encA',3,22); self.pump(.2)
        seq=self.touch('trigKey0',0,31,generation=self.decoder.generation-1); self.pump(.2)
        print('STALE geometry status',self.acks.get(seq,{}).get('status'),'expected 7',flush=True)
        self.ws.send(b'\x13'); self.pump(.2)
        print('LATENCY ingress samples',len(self.timings),'host receive->resolved/enqueued in TOUCH_ACK records.',flush=True)
        print('UNVERIFIED: physical browser display timing; firmware parity for encoder replay; iPad; lifecycle; CPU/audio cost. VISIBLE_PRESS measures targeted decoded press pixels only.',flush=True)
        print('FRAME_RECORDS',str(self.out/'frames.jsonl'),flush=True)

def self_test():
    d=Decoder(); png=io.BytesIO(); Image.new('RGBA',(4,3),(7,8,9,255)).save(png,format='PNG'); png=png.getvalue()
    src=struct.pack('<BBBI5f4I',0x84,1,0,8,10,20,1100,570,2,2200,1140,4,3)
    d.source(src)
    def chunk(offset,n,gen=8): return struct.pack('<BIQQQIII',0x85,gen,4,123,9000,len(png),offset,n)+png[offset:offset+n]
    assert d.chunk(chunk(0,12)) is None
    record,decoded=d.chunk(chunk(12,len(png)-12)); assert decoded==png and d.image.getpixel((0,0))==(7,8,9,255)
    assert record['firmware_led_sequence_sampled_at_request']==123
    assert d.panel_to_raster(0,0)==(10*4/2200,20*3/1140)
    d.source(struct.pack('<BBBI5f4I',0x84,0,3,9,*([0]*9)))
    assert d.image is None and d.chunk(chunk(0,len(png))) is None
    d.source(src)
    try: d.chunk(chunk(12,len(png)-12)); raise AssertionError('accepted missing prefix')
    except ValueError: pass
    print('PASS receiver: synthetic PNG reassembly/decoding, geometry transform, stale-generation discard, source-loss blanking, missing-chunk rejection')
    print('Synthetic fixture only; no live panel pixels or runtime acceptance evidence.')

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--host',default='127.0.0.1'); ap.add_argument('--port',type=int,default=7789)
    ap.add_argument('--output',default='tools/remote-panel-v3/evidence'); ap.add_argument('--self-test',action='store_true')
    args=ap.parse_args()
    if args.self_test: self_test(); return
    probe=Probe(args)
    try: probe.run()
    finally:
        try: probe.ws.send(b'\x13')
        except OSError: pass
        probe.ws.s.close(); probe.log.close()
if __name__=='__main__': main()
