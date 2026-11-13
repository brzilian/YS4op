import sys
src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
voices = [None]*100
bank = 0
i = 0
while i < len(d):
    if d[i] != 0xF0: i += 1; continue
    j = d.index(0xF7, i)
    m = d[i:j+1]
    if len(m) == 7 and m[1] == 0x43 and (m[2] & 0xF0) == 0x10 and m[3] == 0x24 and m[4] == 0x07:
        bank = m[5] - 1
    elif len(m) == 4104 and m[1] == 0x43 and m[3] == 0x04:
        data = m[6:6+4096]
        assert ((-sum(data)) & 0x7F) == m[6+4096]
        for k in range(32):
            idx = bank*25 + k
            if idx < 100 and (voices[idx] is None or k < 25):
                voices[idx] = data[k*128:(k+1)*128]
    i = j+1
assert all(v is not None for v in voices)
with open(dst, 'w') as f:
    f.write('// Generated from the YS200 ROM sysex bank supplied by the user (4 overlapping VMEM dumps -> 100 voices).\n')
    f.write('// Each voice is 128 bytes in the 4-op VMEM packed format.\n#pragma once\n#include <cstdint>\n')
    f.write('static const uint8_t kRomBank[100][128] = {\n')
    for n, v in enumerate(voices):
        f.write('  { // %02d %s\n    ' % (n, bytes(v[57:67]).decode('ascii').rstrip()))
        f.write(',\n    '.join(','.join('%d' % b for b in v[r:r+32]) for r in range(0, 128, 32)))
        f.write(' },\n')
    f.write('};\n')
print('ok', [bytes(v[57:67]).decode().strip() for v in voices[::10]])
