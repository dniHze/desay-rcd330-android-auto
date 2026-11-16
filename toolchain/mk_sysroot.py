import os,sys
root=sys.argv[1]; n=0
for dp,dn,fn in os.walk(root):
    for name in fn:
        p=os.path.join(dp,name)
        try:
            if os.path.islink(p) or os.path.getsize(p)>512: continue
            with open(p,'rb') as f: head=f.read(6)
            if head!=b'Link: ': continue
            with open(p,'rb') as f: tgt=f.read().decode('latin1')[6:].strip()
            if not tgt: continue
            os.remove(p); os.symlink(tgt,p); n+=1
        except Exception: pass
print("reconstructed symlinks:",n)
