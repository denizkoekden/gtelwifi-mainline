#!/usr/bin/env python3
"""condcheck.py <android-base-conditional.xml> <.config>: list the AOSP
conditional kernel requirements whose condition holds but which the config
does not meet. mergecheck.sh only compares against the fragments it merged."""
import re
import sys
import xml.etree.ElementTree as ET

xmlf, cfgf = sys.argv[1], sys.argv[2]
cfg = {}
for line in open(cfgf):
    line = line.strip()
    m = re.match(r'^(CONFIG_\w+)=(.*)$', line)
    if m:
        cfg[m.group(1)] = m.group(2).strip('"')
    m = re.match(r'^# (CONFIG_\w+) is not set', line)
    if m:
        cfg[m.group(1)] = 'n'
val = lambda k: cfg.get(k, 'n')
# the file is a list of fragments without a root element
root = ET.fromstring('<root>' + open(xmlf).read() + '</root>')
bad = 0
for grp in root.iter('group'):
    conds = [(c.find('key').text, c.find('value').text)
             for c in grp.findall('conditions/config')]
    if not all(val(k) == v for k, v in conds):
        continue
    for c in grp.findall('config'):
        k, v = c.find('key').text, c.find('value').text
        if val(k) != v:
            bad += 1
            print(f"conditional: {k} wants {v}, have {val(k)} "
                  f"(when {' & '.join(f'{a}={b}' for a, b in conds)})")
print(f"conditional requirements: {bad} unmet")
sys.exit(1 if bad else 0)
