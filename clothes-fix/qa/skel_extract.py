# Writes "<n> ids... / n*16 floats" for the HAnim node list + skin-to-bone matrices of a DFF.
import struct,sys
def chunks(d,o,e):
    out=[]
    while o+12<=e:
        t,s,v=struct.unpack_from('<III',d,o); out.append((t,o+12,s)); o+=12+s
    return out
def find(l,t): return [c for c in l if c[0]==t]
def extract(path):
    d=open(path,'rb').read()
    root=chunks(d,0,len(d)); clump=chunks(d,root[0][1],root[0][1]+root[0][2])
    fl=find(clump,14)[0]; fr=chunks(d,fl[1],fl[1]+fl[2])
    ids=None
    for c in fr:
        if c[0]!=3: continue
        for p in chunks(d,c[1],c[1]+c[2]):
            if p[0]==0x11e:
                ver,nid,n=struct.unpack_from('<III',d,p[1])
                if n: ids=[struct.unpack_from('<III',d,p[1]+20+i*12)[0] for i in range(n)]
    gl=find(clump,26)[0]
    for g in find(chunks(d,gl[1],gl[1]+gl[2]),15):
        gc=chunks(d,g[1],g[1]+g[2]); st=find(gc,1)[0]; verts=struct.unpack_from('<I',d,st[1]+8)[0]
        ext=find(gc,3)[0]
        for sk in find(chunks(d,ext[1],ext[1]+ext[2]),0x116):
            o,size=sk[1],sk[2]; bones,used,maxw=d[o],d[o+1],d[o+2]
            base=o+4+used+verts*4+verts*16; rem=size-(base-o)
            if rem==bones*64+12 or rem==bones*64: stride=64; pre=0
            elif rem>=bones*68: stride=68; pre=4
            else: raise SystemExit(f"{path}: skin layout rem={rem} bones={bones}")
            mats=[struct.unpack_from('<16f',d,base+i*stride+pre) for i in range(bones)]
            return ids,mats,bones
    raise SystemExit(path+': no skin')
ids,mats,bones=extract(sys.argv[1])
assert ids and len(ids)==bones, (sys.argv[1],len(ids or []),bones)
print(len(ids)); print(' '.join(map(str,ids)))
for m in mats: print(' '.join(repr(x) for x in m))
