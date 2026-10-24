import socket,struct,time,random,sys
def q(name,qtype=1):
    tid=random.randrange(65536)
    hdr=struct.pack('>HHHHHH',tid,0x0100,1,0,0,0)
    body=b''.join(bytes([len(l)])+l.encode('latin1') for l in name.split('.'))+b'\0'
    return tid,hdr+body+struct.pack('>HH',qtype,1)
RC={0:'NOERROR',1:'FORMERR',2:'SERVFAIL',3:'NXDOMAIN',4:'NOTIMP',5:'REFUSED'}
def ask(server,name,qtype=1,to=6.0):
    tid,pkt=q(name,qtype)
    s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.settimeout(to)
    t=time.monotonic(); s.sendto(pkt,(server,53))
    try:
        d,_=s.recvfrom(2048); ms=(time.monotonic()-t)*1000
        rid,fl=struct.unpack('>HH',d[:4]); return ms,RC.get(fl&15,fl&15),rid==tid
    except socket.timeout:
        return None,'TIMEOUT',False
    finally: s.close()
n=int(sys.argv[1])
for server in ('114.114.114.114','8.8.8.8'):
    for name in ('bad host!','nonexistent-gai-test.invalid','example.com'):
        res=[ask(server,name) for _ in range(n)]
        ok=[r[0] for r in res if r[0] is not None]
        rcs={}
        for r in res: rcs[r[1]]=rcs.get(r[1],0)+1
        print('%-16s %-30s answered=%d/%d  rtt_ms=%s  %s'%(server,name,len(ok),n,
              ('%d..%d'%(min(ok),max(ok))) if ok else '-', rcs))
