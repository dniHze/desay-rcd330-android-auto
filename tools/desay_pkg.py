#!/usr/bin/env python3
"""Desay 0x81488148 update-container pack/unpack. No crypto; md5+rollback+offset/len only."""
import sys,struct,os,hashlib,argparse
MAGIC=0x81488148; HDRLEN=0x200000; STRIDE=0x200
F_OFF=0x04; F_NAME=0x08; F_LEN=0x28; F_VER=0x4c
def read_records(hdr):
    recs=[]; i=1
    while True:
        base=i*STRIDE
        if base+STRIDE>len(hdr): break
        sl=hdr[base:base+STRIDE]
        if struct.unpack('<I',sl[0:4])[0]!=MAGIC: break
        name=sl[F_NAME:F_NAME+0x40].split(b'\0')[0].decode('latin1')
        off=struct.unpack('<I',sl[F_OFF:F_OFF+4])[0]
        ln =struct.unpack('<I',sl[F_LEN:F_LEN+4])[0]
        ver=sl[F_VER:F_VER+0x40].split(b'\0')[0].decode('latin1')
        if not name: break
        # names come from the (untrusted) header and are joined onto --out/--parts paths
        if name!=os.path.basename(name) or name in ('.','..'):
            sys.exit("refusing suspicious component name %r in slot %d"%(name,i))
        recs.append(dict(slot=i,base=base,name=name,off=off,len=ln,ver=ver)); i+=1
    return recs
def cmd_unpack(a):
    data=open(a.bin,'rb').read(); hdr=data[:HDRLEN]
    recs=read_records(hdr); os.makedirs(a.out,exist_ok=True)
    if not recs: sys.exit("no component records in %s"%a.bin)
    for r in recs:
        # a truncated download must not unpack into short components
        if r['off']<HDRLEN or r['off']+r['len']>len(data):
            sys.exit("%s: component %s at 0x%x+%d lies outside the file (%d bytes)"%(a.bin,r['name'],r['off'],r['len'],len(data)))
        open(os.path.join(a.out,r['name']),'wb').write(data[r['off']:r['off']+r['len']])
        print("unpacked %-12s off=0x%08x len=%d ver=%s"%(r['name'],r['off'],r['len'],r['ver']))
def cmd_pack(a):
    hdr=bytearray(open(a.template,'rb').read(HDRLEN))
    recs=read_records(hdr); off=HDRLEN; blob=bytearray()
    for r in recs:
        p=os.path.join(a.parts,r['name']); comp=open(p,'rb').read()
        struct.pack_into('<I',hdr,r['base']+F_OFF,off)
        struct.pack_into('<I',hdr,r['base']+F_LEN,len(comp))
        blob+=comp; print("packed   %-12s off=0x%08x len=%d"%(r['name'],off,len(comp))); off+=len(comp)
    out=bytes(hdr)+bytes(blob); open(a.out,'wb').write(out)
    md5=hashlib.md5(out).hexdigest()
    # regenerate sidecar: md5 line + original VERSION/ROLL_BACK manifest (unchanged). Without
    # those lines the updater would see no rollback numbers, so the manifest is required.
    man="".join(l for l in open(a.manifest) if l.startswith(("VERSION:","ROLL_BACK_NO:","PROJECT:")))
    if man.count("ROLL_BACK_NO:")!=len(recs) or man.count("VERSION:")!=len(recs):
        sys.exit("%s: expected a VERSION and a ROLL_BACK_NO line per component"%a.manifest)
    open(a.out+".md5",'w').write("%s  .//%s\n%s"%(md5,os.path.basename(a.out),man))
    print("WROTE %s  (%d bytes)  md5=%s"%(a.out,len(out),md5))
p=argparse.ArgumentParser(); sp=p.add_subparsers(dest='c',required=True)
u=sp.add_parser('unpack'); u.add_argument('bin'); u.add_argument('out'); u.set_defaults(fn=cmd_unpack)
k=sp.add_parser('pack'); k.add_argument('--template',required=True); k.add_argument('--parts',required=True); k.add_argument('--out',required=True); k.add_argument('--manifest',required=True); k.set_defaults(fn=cmd_pack)
a=p.parse_args(); a.fn(a)
