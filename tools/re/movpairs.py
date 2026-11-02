import sys,re
lines=open(sys.argv[1]).read().splitlines()
targets={int(x,16)+0x9f600000:x for x in sys.argv[2:]}
movw={}
rx=re.compile(r'^([0-9a-f]+):\s+[0-9a-f]+\s+(movw|movt)\s+(r\d+|ip|lr|sl|fp), #\d+\s+@ (0x[0-9a-f]+)')
for n,l in enumerate(lines):
    m=rx.match(l)
    if not m: continue
    addr,op,reg,imm=m.groups(); imm=int(imm,16)
    if op=='movw': movw[reg]=(imm,n)
    elif reg in movw and n-movw[reg][1]<8:
        v=(imm<<16)|movw[reg][0]
        if v in targets: print(targets[v], addr, 'line', n+1)
