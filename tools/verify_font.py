# -*- coding: utf-8 -*-
"""Verify a generated LVGL font's CMAP can resolve all required chars."""
import re, sys

def check_font(fname, texts):
    content = open(fname, encoding='utf-8').read()
    rs = int(re.search(r'range_start = 0x([0-9A-F]+)', content).group(1), 16)
    rl = int(re.search(r'range_length = 0x([0-9A-F]+)', content).group(1), 16)
    ul = content.split('unicode_list[] = {', 1)[1].split('};', 1)[0]
    offs = set(int(x, 16) for x in re.findall(r'0x([0-9A-F]{4})', ul))
    def has(c):
        u = ord(c)
        if u < rs or u >= rs + rl:
            return False
        return (u - rs) in offs
    print('--- %s (range=0x%04X len=0x%X chars=%d) ---' % (fname.split('\\')[-1], rs, rl, len(offs)))
    ok = True
    for t in texts:
        bad = [c for c in t if not has(c)]
        if bad:
            ok = False
            print('  %s: MISSING %s' % (t, ' '.join('U+%04X' % ord(c) for c in bad)))
        else:
            print('  %s: OK' % t)
    return ok

texts = [
    '请在服务区休息', '驾驶时长:', '休息前疲劳:', '停车位置:',
    '0 分钟', '命令: rest stop 提前退出', '/ 15:00', '00:00',
    '服务区', '应急车道', '普通路段', '正常', '轻度', '严重',
    '疲劳驾驶监测', '速度', 'GPS', '司机', '人脸', '有', '无',
]
r = check_font(sys.argv[1], texts)
print('RESULT:', 'ALL OK' if r else 'HAS MISSING CHARS')
