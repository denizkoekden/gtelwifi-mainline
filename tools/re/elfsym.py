import sys, struct
d = sys.stdin.buffer.read()
assert d[:4] == b'\x7fELF'
e_shoff, = struct.unpack_from('<I', d, 0x20)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 0x2e)
secs = []
for i in range(e_shnum):
    o = e_shoff + i*e_shentsize
    name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from('<10I', d, o)
    secs.append((name, typ, flags, addr, off, size, link, entsize))
shstr = secs[e_shstrndx]
def sname(n): 
    s = d[shstr[4]+n:]; return s[:s.index(b'\0')].decode()
names = [sname(s[0]) for s in secs]
want = sys.argv[1:]
for i,(n,typ,flags,addr,off,size,link,entsize) in enumerate(secs):
    if typ in (2,11):  # SYMTAB/DYNSYM
        strs = secs[link]
        for j in range(size//16):
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from('<IIIBBH', d, off+16*j)
            s = d[strs[4]+st_name:]; s = s[:s.index(b'\0')].decode(errors='replace')
            if any(w in s for w in want):
                # find section containing value
                for k,(n2,t2,f2,a2,o2,sz2,l2,e2) in enumerate(secs):
                    if a2 and a2 <= st_value < a2+sz2:
                        fo = o2 + st_value - a2
                        blob = d[fo:fo+min(st_size,512)] if t2 != 8 else b''
                        print(s, hex(st_value), st_size, names[k])
                        for q in range(0, len(blob), 16):
                            words = struct.unpack_from('<%dI' % (min(16,len(blob)-q)//4), blob, q)
                            print('  +%03x' % q, ' '.join('%08x' % w for w in words))
                        break
