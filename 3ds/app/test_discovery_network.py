"""Real UDP sockets: multicast-only and legacy-unicast-only Cast responders."""
from pathlib import Path
import socket,struct,subprocess,threading
root=Path(__file__).resolve().parent
Path('/tmp/discover-main.c').write_text('''
#include "discovery.h"
#include <stdio.h>
int main(void) {NativeTV tv[8];NativeDiscoveryInfo info;atomic_bool cancel=false;
unsigned n=native_discover_ex(tv,8,&cancel,&info);
printf("devices=%u packets=%u multicast=%u\\n",n,info.valid_packets,info.multicast);
for(unsigned i=0;i<n;i++)printf("%s %s\\n",tv[i].name,tv[i].ip);
return n==1?0:1;}
''')
subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root),
 '/tmp/discover-main.c',str(root/'discovery.c'),'-o','/tmp/native-discover-network'],check=True)
def name(value):return b''.join(bytes([len(p)])+p.encode() for p in value.split('.'))+b'\0'
def rr(owner,kind,data):return name(owner)+struct.pack('!HHIH',kind,1,120,len(data))+data
instance='Test TV._googlecast._tcp.local'
payload=rr('_googlecast._tcp.local',12,name(instance))
payload+=rr(instance,33,struct.pack('!HHH',0,0,8009)+name('tv.local'))
payload+=rr('tv.local',1,bytes([192,168,1,42]))
payload+=rr(instance,16,b'\x0afn=Test TV')
for mode in ('multicast','unicast'):
    sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM,socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);sock.bind(('',5353))
    sock.setsockopt(socket.IPPROTO_IP,socket.IP_ADD_MEMBERSHIP,socket.inet_aton('224.0.0.251')+socket.inet_aton('0.0.0.0'))
    sock.settimeout(.2);stopped=threading.Event();errors=[]
    def respond():
        while not stopped.is_set():
            try:packet,sender=sock.recvfrom(9000)
            except socket.timeout:continue
            if len(packet)<12 or packet[2]&0x80:continue
            if name('_googlecast._tcp.local') not in packet:continue
            if mode=='multicast' and sender[1]!=5353:continue
            if mode=='unicast' and sender[1]==5353:continue
            allowed=(b'\0\1',b'\x80\1') if mode=='multicast' else (b'\0\1',)   # the multicast socket sets QU
            if packet[-2:] not in allowed:errors.append('invalid query class')
            reply=packet[:2]+struct.pack('!HHHHH',0x8400,0,4,0,0)+payload
            sock.sendto(reply,('224.0.0.251',5353) if mode=='multicast' else sender)
    thread=threading.Thread(target=respond);thread.start()
    try:
        result=subprocess.run(['/tmp/native-discover-network'],capture_output=True,text=True,timeout=8)
        assert result.returncode==0,(mode,result.stdout,result.stderr)
        assert 'Test TV 192.168.1.42' in result.stdout,(mode,result.stdout)
        assert not errors,errors
    finally:stopped.set();thread.join();sock.close()
print('PASS: real UDP mDNS discovery with multicast-only and legacy-unicast-only responders')
