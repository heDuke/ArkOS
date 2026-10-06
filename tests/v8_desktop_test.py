"""Production ArkOS guest: real input, consent, independent apps and persistence."""
from fixtures import fresh_data_disk
from vm import VM,ROOT
from PIL import Image
import os,time,json,sys,re
os.environ['ARKOS_MACHINE']='q35';os.environ['ARKOS_SMP']='4'
fw=sys.argv[1] if len(sys.argv)>1 else 'bios'
out=ROOT/'build'/('test-v8-desktop-'+fw);out.mkdir(exist_ok=True)
disk=out/'data.img';fresh_data_disk(disk)
v=VM('v8-desktop-'+fw,disk,firmware=fw,device='virtio-multitouch-pci,virtio-tablet-pci,virtio-keyboard-pci')
def screen(name):v.screen(name);Image.open(out/(name+'.ppm')).save(out/(name+'.png'))
def app(name):
 v.key('f5');time.sleep(1);v.type(name);time.sleep(.3);v.key('ret');v.wait('[app] '+name+' ring3 ready',timeout=30);time.sleep(.6)
def settle(after=0):
 # A window transition blocks input until it finishes; on slow hosts it can
 # take several seconds instead of the ~230ms target.
 try:v.wait('[motion] end kind=window value=65536',timeout=30,after=after)
 except TimeoutError:pass
 time.sleep(1)
def tap_until(x,y,marker,tries=8,timeout=4):
 # Navigation taps are idempotent; retry until the expected page/state change
 # appears, tolerating a tap that lands before the window publishes its regions.
 for _ in range(tries):
  m=len(v.log.read_text());v.tap(x,y)
  try:
   v.wait(marker,timeout=timeout,after=m);time.sleep(.5);return
  except TimeoutError:time.sleep(.5)
 raise TimeoutError('tap(%d,%d) -> %s'%(x,y,marker))
try:
 v.wait('[session] setup ready');v.type('Native83!');v.key('tab');v.type('Native83!');v.key('ret');v.wait('[session] desktop unlocked',60)
 v.key('f3');v.wait('[permission] Consent requested by notes');screen('01-native-consent');v.key('esc');v.wait('[permission] Denied notes')
 # Denial is remembered for this process; repeated polling must not reprompt.
 start=len(v.log.read_text());v.type('Preserve draft');v.key('ctrl-w');time.sleep(1.4)
 assert '[permission] Consent requested' not in v.log.read_text()[start:];screen('02-unsaved-close-protection')
 m=len(v.log.read_text());v.key('f4');time.sleep(.4);settle(m)
 tap_until(300,335,'[ui] Settings page 应用权限');screen('03-permissions')
 # Notes (文本编辑) is the 4th row of the right-hand catalog; capture its toggles.
 tap_until(760,428,'[ui] Settings page 应用访问权限');screen('04-notes-permissions')
 m=len(v.log.read_text());v.tap(1083,404);v.wait('[permission] Settings notes grants=6',after=m);v.key('f3');v.key('ctrl-i');v.type('zhongguo');v.key('spc');v.wait('[ime] Native Pinyin commit accepted in Notes process');v.key('ctrl-i');v.key('ctrl-s');v.wait('[notes] File saved through private user API');screen('05-native-notes')
 v.terminal();v.command('cat notes.txt');v.wait('Preserve draft中国')
 v.command('echo OpenArgument > argument.txt');v.command('run notes argument.txt');time.sleep(2.5);v.key('end');v.type('-edited');v.key('ctrl-s');time.sleep(1.5);v.terminal();v.command('cat argument.txt');v.wait('OpenArgument\n-edited');v.command('run notes notes.txt');time.sleep(1.5)
 app('Todo');v.wait('[permission] Consent requested by todo');v.key('ret');v.wait('[permission] Allowed todo');time.sleep(1.1);v.type('Verify native permissions');v.key('ret');v.wait('[todo] Native file saved');screen('06-independent-todo')
 app('Browser');app('Calculator');v.type('123+45');v.key('ret');screen('07-independent-calculator')
 app('Timer');v.key('spc');time.sleep(1.3);screen('08-independent-timer')
 for name in ['Clock','Paint','Markdown','WASM']:app(name)
 v.key('ctrl-t');time.sleep(.5);screen('09-nine-independent-apps')
 log=v.log.read_text();spawned=re.findall(r'Spawn pid=(\d+) uid=1000 name=(\w+)',log)
 names={name:pid for pid,name in spawned};assert set(['notes','browser','calculator','todo','timer','clock','paint','markdown'])<=names.keys();assert len(set(names.values()))==9
 m=len(v.log.read_text());v.key('f4');time.sleep(.4);settle(m)
 tap_until(300,335,'[ui] Settings page 应用权限')
 tap_until(760,286,'[ui] Settings page 应用访问权限')
 m=len(v.log.read_text());v.tap(1083,354);v.wait('[process] Exit pid='+names['clock']+' status=0xffffffffffffffff',after=m);screen('10-live-process-revocation')
 # The app catalog only exists on the category page; ESC returns to it.
 m=len(v.log.read_text());v.key('esc');v.wait('[ui] Settings page 应用权限',after=m)
 tap_until(760,428,'[ui] Settings page 应用访问权限')
 m=len(v.log.read_text());v.tap(1083,404);v.wait('[permission] Settings notes grants=4',after=m);v.key('f3');v.type(' Not on disk');start=len(v.log.read_text());v.key('ctrl-s');v.wait('[permission] Consent requested by notes',after=start);v.key('esc');v.wait('[permission] Denied notes',after=start)
 v.terminal();v.command('cat notes.txt');time.sleep(.3);assert 'Not on disk' not in v.log.read_text();v.command('sync')
 assert '[exception]' not in v.log.read_text() and 'User fault pid=1' not in v.log.read_text()
finally:v.close()
# Reboot the written disk: account, text and revoked grants must survive.
v=VM('v8-persist-'+fw,disk,firmware=fw,device='virtio-multitouch-pci,virtio-tablet-pci')
try:
 v.wait('[session] login ready');v.type('Native83!');v.key('ret');v.wait('[session] desktop unlocked',60)
 v.terminal();v.command('cat notes.txt');v.wait('Preserve draft中国');v.command('cat todo.txt');v.wait('Verify native permissions')
 v.command('run clock');time.sleep(.7);assert '[app] Clock ring3 ready' not in v.log.read_text()
 v.key('f3');v.wait('[permission] Consent requested by notes');v.key('esc')
finally:v.close()
(out/'results.json').write_text(json.dumps({'result':'PASS','firmware':fw,'cpus':4,'independent_apps':names,'verified':['deny without prompt spam','unsaved draft retained','grant in settings','Pinyin within Notes process','nine simultaneous distinct PIDs','live UI revocation terminates process','live FILES revocation denies save','reboot preserves text and revoked grants'],'runtime_host_bridge':False},indent=2)+'\n')
print('PASS native consent, nine isolated applications, live revocation and reboot persistence',fw)
