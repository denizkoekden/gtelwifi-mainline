import sys, struct
data = open(sys.argv[1],'rb').read()
base = 0x9F600000
for s in sys.argv[2:]:
    off = int(s,16)
    va = base+off
    pat = struct.pack('<I', va)
    i = data.find(pat)
    res=[]
    while i!=-1:
        res.append(hex(i))
        i = data.find(pat, i+1)
    print(s, hex(va), res)
