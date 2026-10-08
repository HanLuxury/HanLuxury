# Facts the libGTASA review asks about, for every TESTLIT DFF.
import struct,sys,os,collections
def chunks(d,o,e):
    out=[]
    while o+12<=e:
        t,s,v=struct.unpack_from('<III',d,o); out.append((t,o+12,s)); o+=12+s
    return out
def find(l,t): return [c for c in l if c[0]==t]
stats=collections.Counter(); longnames=[]; multi=[]; names_all=collections.Counter()
maxlen=0
for root,_,files in os.walk(sys.argv[1]):
    for f in files:
        if not f.endswith('.dff'): continue
        p=os.path.join(root,f); d=open(p,'rb').read()
        r=chunks(d,0,len(d))[0]; clump=chunks(d,r[1],r[1]+r[2])
        fl=find(clump,14)[0]; fr=chunks(d,fl[1],fl[1]+fl[2])
        fst=find(fr,1)[0]; nframes=struct.unpack_from('<I',d,fst[1])[0]
        mats=[struct.unpack_from('<12f',d,fst[1]+4+i*56) for i in range(nframes)]
        parents=[struct.unpack_from('<i',d,fst[1]+4+i*56+48)[0] for i in range(nframes)]
        names=[]
        for c in [c for c in fr if c[0]==3]:
            nm=None
            for pl in chunks(d,c[1],c[1]+c[2]):
                if pl[0]==0x253f2fe:
                    nm=d[pl[1]:pl[1]+pl[2]]; maxlen=max(maxlen,pl[2])
                    if pl[2]>=23: longnames.append((f,pl[2],nm.decode('latin1')))
                if pl[0]==0x11e:
                    ver,nid,cnt=struct.unpack_from('<III',d,pl[1])
                    if cnt:
                        flags,kfs=struct.unpack_from('<II',d,pl[1]+12); stats[f'hanim ver=0x{ver:x} flags={flags} kfsize={kfs}']+=1
            names.append(nm)
        gl=find(clump,26)[0]; geos=[g for g in chunks(d,gl[1],gl[1]+gl[2]) if g[0]==15]
        skins=[]
        for g in geos:
            gc=chunks(d,g[1],g[1]+g[2]); st=find(gc,1)[0]; verts=struct.unpack_from('<I',d,st[1]+8)[0]
            ml=find(gc,8)[0]
            for m in find(chunks(d,ml[1],ml[1]+ml[2]),7):
                for t in find(chunks(d,m[1],m[1]+m[2]),6):
                    for s in chunks(d,t[1],t[1]+t[2]): stats[f'texture string chunk type 0x{s[0]:x}']+=1
            ext=find(gc,3)[0]; sk=find(chunks(d,ext[1],ext[1]+ext[2]),0x116)[0]
            o=sk[1]; bones,used,maxw=d[o],d[o+1],d[o+2]; stats[f'skin maxWeights={maxw}']+=1
            base=o+4+used+verts*20; split=struct.unpack_from('<III',d,base+bones*64)
            stats[f'split data {split}']+=1
            skins.append(d[base:base+bones*64])
        atoms=[struct.unpack_from('<II',d,a[1]+12)  for a in []]
        atomics=[]
        for a in find(clump,20):
            ac=chunks(d,a[1],a[1]+a[2]); stt=find(ac,1)[0]; fi,gi=struct.unpack_from('<II',d,stt[1]); atomics.append((fi,gi))
        stats[f'atomics per dff={len(atomics)}']+=1
        for fi,gi in atomics:
            m=mats[fi]; ident=all(abs(a-b)<1e-4 for a,b in zip(m,(1,0,0,0,1,0,0,0,1,0,0,0)))
            par=parents[fi]; stats[f'atomic frame parent={par} identity={ident}']+=1
        if len(atomics)>1:
            same=all(skins[gi]==skins[atomics[0][1]] for _,gi in atomics)
            multi.append((f,len(atomics),'same invbind' if same else 'DIFFERENT invbind'))
print('max frame-name chunk length:',maxlen)
print('names >= 23 bytes:',len(longnames)); [print('  ',x) for x in sorted(set(longnames))[:20]]
for k,v in sorted(stats.items()): print(f'{v:5}  {k}')
print('multi-atomic DFFs:',len(multi)); [print('  ',x) for x in multi[:10]]
