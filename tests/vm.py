from pathlib import Path
import json,os,subprocess,time,selectors
ROOT=Path(__file__).resolve().parents[1]
class VM:
 def __init__(self,name='manual',disk=None,firmware='bios',device='virtio-multitouch-pci',gpu='vmware',external=None,ready='[arkos] desktop ready',extra_args=None):
  self.out=ROOT/'build'/('test-'+name);self.out.mkdir(parents=True,exist_ok=True)
  self.log=self.out/'serial.log';self.log.write_text('');self.err=open(self.out/'qemu.log','w')
  args=[os.environ.get('QEMU_BIN','qemu-system-x86_64'),'-machine',os.environ.get('ARKOS_MACHINE','pc'),'-smp',os.environ.get('ARKOS_SMP','1'),'-cpu',os.environ.get('ARKOS_CPU','max'),'-accel','tcg','-m',os.environ.get('ARKOS_MEMORY','512M'),'-vga',gpu,'-cdrom',os.environ.get('ARKOS_ISO',str(ROOT/'build/arkos-0.13.0.iso')),'-boot','d','-netdev','user,id=net0','-device','e1000,netdev=net0,romfile=','-display',os.environ.get('ARKOS_DISPLAY','none'),'-serial','file:'+str(self.log),'-qmp','stdio']
  if gpu=='vmware':args+=['-global','vmware-svga.vgamem_mb='+os.environ.get('ARKOS_VRAM_MB','64')]
  if os.environ.get('ARKOS_TRACE_DISPLAY'):args+=['-trace','events='+str(ROOT/'tests/qemu-display-trace.events')+',file='+str(self.out/'display-trace.log')]
  if os.environ.get('ARKOS_HDD_ONLY'):
   i=args.index('-cdrom');del args[i:i+2];args[args.index('-boot')+1]='c'
  if disk:args+=['-drive','file='+str(disk)+',format=raw,if=ide,index=0']
  if external:args+=['-drive','file='+str(external)+',format=raw,if=ide,index=1']
  if device:
   for item in device.split(','):args+=['-device',item]
  if os.environ.get('QEMU_DATA'):args+=['-L',os.environ['QEMU_DATA']]
  if os.environ.get('ARKOS_GPU_DEVICE'):args+=['-device',os.environ['ARKOS_GPU_DEVICE']]
  if os.environ.get('ARKOS_SPICE'):args+=['-spice',os.environ['ARKOS_SPICE']]
  if extra_args:args+=extra_args
  if firmware=='uefi':args+=['-drive','if=pflash,format=raw,readonly=on,file='+os.environ.get('OVMF_CODE','/usr/share/OVMF/OVMF_CODE_4M.fd')]
  self.p=subprocess.Popen(args,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.err,bufsize=0)
  self.sel=selectors.DefaultSelector();self.sel.register(self.p.stdout,selectors.EVENT_READ)
  try:self.read();self.q('qmp_capabilities');self.wait(ready,60)
  except Exception:
   self.p.terminate();self.p.wait(timeout=5);self.err.close();self.sel.close();raise
 def read(self):
  if not self.sel.select(timeout=30):raise TimeoutError('QMP timeout: '+self.log.read_text())
  line=self.p.stdout.readline()
  if not line:raise RuntimeError((self.out/'qemu.log').read_text()+'\n'+self.log.read_text())
  try:return json.loads(line)
  except json.JSONDecodeError:
   with open(self.out/"qmp-unexpected.log","ab") as log:log.write(line)
   return self.read()
 def q(self,cmd,args=None):
  self.p.stdin.write((json.dumps({'execute':cmd,'arguments':args or {}})+'\n').encode());self.p.stdin.flush()
  while True:
   reply=self.read()
   if 'error' in reply:raise RuntimeError(reply)
   if 'return' in reply:return reply['return']
 def wait(self,s,timeout=20,after=0):
  end=time.time()+timeout
  while s not in self.log.read_text()[after:]:
   if time.time()>end:raise TimeoutError(s+'\n'+self.log.read_text())
   if self.p.poll() is not None:raise RuntimeError('VM exited')
   time.sleep(.1)
 def key(self,key):
  # Explicit QMP edges avoid HMP's guest-clock release timer overlapping the
  # next chord when a long disk operation advances guest/wall time differently.
  keys=key.split('-')
  self.q('input-send-event',{'events':[{'type':'key','data':{'down':True,'key':{'type':'qcode','data':part}}} for part in keys]})
  time.sleep(.045)
  self.q('input-send-event',{'events':[{'type':'key','data':{'down':False,'key':{'type':'qcode','data':part}}} for part in reversed(keys)]})
  time.sleep(.045)
 def type(self,text):
  lookup={' ':'spc','.':'dot','-':'minus','_':'shift-minus','/':'slash','>':'shift-dot','<':'shift-comma','|':'shift-backslash','"':'shift-apostrophe',"'":'apostrophe',':':'shift-semicolon',';':'semicolon','=':'equal','+':'shift-equal','(':'shift-9',')':'shift-0','!':'shift-1','\\':'backslash'}
  for ch in text:
   key=lookup.get(ch,ch.lower())
   if ch.isupper():key='shift-'+key
   self.key(key)
 def command(self,s):
  # Wait for the next shell prompt so consecutive commands cannot interleave
  # when the guest is slow to process input.
  mark=len(self.log.read_text())
  self.type(s);self.key('ret')
  try:self.wait('$ ',timeout=20,after=mark)
  except TimeoutError:pass
 def terminal(self):self.key('f1');time.sleep(.5)
 def enroll_test_user(self):
  # Only call on an isolated blank fixture/RAM session, never a user's disk.
  self.wait('[session] setup ready')
  self.type('Refresh-Test42!');self.key('tab');self.type('Refresh-Test42!');self.key('ret')
  self.wait('[session] desktop unlocked',timeout=30)
 def screen(self,name):time.sleep(.5);self.q('screendump',{'filename':str(self.out/(name+'.ppm'))})
 def touch(self,x,y,kind='begin',slot=0,tracking=1):
  width,height=map(int,os.environ.get('ARKOS_GEOMETRY','1280x800').split('x'))
  ax=round(x*32767/(width-1));ay=round(y*32767/(height-1))
  if kind in ('end','cancel'):events=[{'type':'mtt','data':{'type':'end','slot':slot,'tracking-id':-1,'axis':'x','value':ax}}]
  else:events=[{'type':'mtt','data':{'type':kind,'slot':slot,'tracking-id':tracking,'axis':'x','value':ax}},{'type':'mtt','data':{'type':'data','slot':slot,'tracking-id':tracking,'axis':'x','value':ax}},{'type':'mtt','data':{'type':'data','slot':slot,'tracking-id':tracking,'axis':'y','value':ay}}]
  self.q('input-send-event',{'events':events});time.sleep(.18)
 def tap(self,x,y):self.touch(x,y);self.touch(x,y,'end')
 def close(self):
  if self.p.poll() is None:self.q('quit');self.p.wait(timeout=5)
  self.err.close();self.sel.close()
