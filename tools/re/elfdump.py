import sys, struct
d = sys.stdin.buffer.read()
e_shoff, = struct.unpack_from('<I', d, 0x20)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 0x2e)
secs = [struct.unpack_from('<10I', d, e_shoff+i*e_shentsize) for i in range(e_shnum)]
def v2o(v):
    for (n,t,f,a,o,s,l,i,al,es) in secs:
        if a and a <= v < a+s and t != 8: return o + v - a
def cstr(v):
    o = v2o(v); s = d[o:o+80]; return s[:s.index(b'\0')].decode(errors='replace')
mode = sys.argv[1]
if mode == 'str':
    for a in sys.argv[2:]: print(a, cstr(int(a,16)))
elif mode == 'restab':
    base = int(sys.argv[2],16); n = int(sys.argv[3])
    for k in range(n):
        p,c,w,h,x,f = struct.unpack_from('<IIHHII', d, v2o(base+20*k))
        print(k, hex(p), c, w, h, x, f)
elif mode == 'words':
    base = int(sys.argv[2],16); n = int(sys.argv[3])
    o = v2o(base)
    print(' '.join('%08x'%w for w in struct.unpack_from('<%dI'%n, d, o)))
