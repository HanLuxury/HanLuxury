import json,re,os,sys
R=sys.argv[1]; T=sys.argv[2]
cc=json.load(open(T+'/character.json')); cl=json.load(open(T+'/clothes.json'))
inc=open(R+'/gamemodes/SERVER/player/character/character_catalog.inc').read()
srv={int(a):(int(b),int(c),int(d)) for a,b,c,d in re.findall(r'\{(\d+),(\d+),(\d+),(\d+)\}',inc)}
defs={k:int(v) for k,v in re.findall(r'#define (EC_\w+) \((\d+)\)',inc)}
slots=["hair","top","jacket","pants","shoes","hat","glasses","mask","watch","necklace","bag","accessory"]
bodybit={f"{g}_{b}":gi*3+bi for gi,g in enumerate(["male","female"]) for bi,b in enumerate(["slim","normal","large"])}
prob=[]
cli={}
for it in cl['items']:
    field=8+slots.index(it['slot'])
    mask=sum(1<<bodybit[k] for k in it['variants'])
    cli[it['id']]=(field,it['price'],mask)
for i in sorted(set(cli)|set(srv)):
    if cli.get(i)!=srv.get(i): prob.append(f"item {i}: client {cli.get(i)} server {srv.get(i)}")
# SQL
for f in ['001_eagle_character.sql','003_eagle_ped_158_298_wardrobe.sql']:
    sq={int(a):(int(b),int(d),int(e)) for a,b,c,d,e,h,r in re.findall(r"VALUES \((\d+),(\d+),'([^']*)',(\d+),(\d+),(\d+),(\d+)\)",open(R+'/sql/'+f).read())}
    for i in sorted(set(sq)|set(srv)):
        if sq.get(i)!=srv.get(i): prob.append(f"{f} item {i}: sql {sq.get(i)} server {srv.get(i)}")
    print(f,'rows',len(sq))
# counts
faces={};hair={}
for r in cc['faces']: faces.setdefault(r['body'],set()).add(r['id'])
for r in cc['hair']: hair.setdefault(r['body'],set()).add(r['id'])
for b in bodybit:
    if faces.get(b)!=set(range(1,defs['EC_FACE_COUNT']+1)): prob.append(f"faces {b}: {sorted(faces.get(b,[]))}")
    if hair.get(b)!=set(range(1,defs['EC_HAIR_COUNT']+1)): prob.append(f"hair {b}: {sorted(hair.get(b,[]))}")
if len(cc['skinTones'])!=defs['EC_SKIN_TONES']: prob.append('skin tones')
if len(cc['hairColors'])!=defs['EC_HAIR_COLORS']: prob.append('hair colors')
if cc['revision']!=defs['EC_CATALOG_REVISION'] or cl['revision']!=defs['EC_CATALOG_REVISION']: prob.append('revision')
if cc['carrierModels']!=[defs['EC_MALE_PED'],defs['EC_FEMALE_PED']]: prob.append('carriers')
# defaults
dflt=[int(x) for x in re.search(r'gECDefault\[22\] = \{([^}]*)\}',inc).group(1).split(',')]
print('server default',dflt)
# thumbnails
A=R+'/cefui/assets/character'
miss=[]
for i,(field,price,mask) in srv.items():
    for gi,g in enumerate(['male','female']):
        if mask & (7<<(gi*3)) and not os.path.exists(f"{A}/items/{g}_{i}.png"): miss.append(f"items/{g}_{i}.png")
for g in ['male','female']:
    for n in range(1,13):
        if not os.path.exists(f"{A}/{g}_face{n}.png"): miss.append(f"{g}_face{n}.png")
    for n in range(1,9):
        if not os.path.exists(f"{A}/{g}_hair{n}.png"): miss.append(f"{g}_hair{n}.png")
print('missing thumbnails',len(miss),miss[:10])
# field orders
js=open(R+'/cefui/js/character.js').read()
jsf=re.search(r"fields=\[([^\]]*)\]",js).group(1).replace("'","").split(',')
srvf=re.findall(r'"(\w+)"',re.search(r'gECJsonFields\[\]\[24\] = \{([^}]*)\}',inc).group(1))
dbf=re.findall(r'"(\w+)"',re.search(r'gECFields\[\]\[\d+\] = \{([^}]*)\}',inc).group(1))
nat=re.findall(r'"(\w+)"',re.search(r'kFields\[kFieldCount\] = \{([^}]*)\}',open(R+'/jni/game/character/CharacterTypes.h').read(),re.S).group(1))
proc=re.findall(r'IN p_(\w+) INT',open(R+'/sql/001_eagle_character.sql').read())[6:]
print('js==native',jsf==nat,'srvjson==native',srvf==nat,'db',dbf,'proc',proc==dbf)
print('PROBLEMS',len(prob)); print('\n'.join(prob[:40]))
