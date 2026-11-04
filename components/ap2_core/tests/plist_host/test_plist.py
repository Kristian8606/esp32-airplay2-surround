#!/usr/bin/env python3
"""Tests of the plist module through driver.c, against Python's plistlib.

    SODIUM=<libsodium prefix> ./make_driver.sh ../../main/plist /tmp/plist_driver
    python3 test_plist.py /tmp/plist_driver [other_driver_to_compare]

With a second driver every read is also compared with that build; the known,
intended differences are excluded and listed in main below.
"""
import json, math, os, plistlib, random, struct, subprocess, sys

checks = 0
def ok(cond, what):
    global checks; checks += 1
    if not cond: raise SystemExit(f'FAILED: {what}')

class Dut:
    def __init__(self, path):
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    def cmd(self, line):
        self.p.stdin.write(line + '\n'); self.p.stdin.flush()
        out = self.p.stdout.readline()
        if not out: raise SystemExit(f'FAILED: driver stopped on {line[:200]}')
        return json.loads(out)
    def read(self, op, key, number, body): return self.cmd(f'{op} {key} {number} {body.hex() or "-"}')
    def close(self):
        self.p.stdin.close(); self.p.wait(); ok(self.p.returncode == 0, 'sanitizers clean')

def B(o): return plistlib.dumps(o, fmt=plistlib.FMT_BINARY, sort_keys=False)
def items(hexbody):
    return list(plistlib.loads(bytes.fromhex(hexbody)).items())

def enc(objs, top=0, oref=1, osz=1, ver=b'00'):
    body = b'bplist' + ver; offs = []
    for o in objs: offs.append(len(body)); body += o
    st = len(body); t = b''.join(x.to_bytes(osz, 'big') for x in offs)
    return body + t + b'\0' * 6 + bytes([osz, oref]) + len(objs).to_bytes(8, 'big') + top.to_bytes(8, 'big') + st.to_bytes(8, 'big')
A = lambda s: bytes([0x50 | len(s)]) + s
D = lambda ks, vs: bytes([0xD0 | len(ks)]) + bytes(ks + vs)

# ---- replies ----------------------------------------------------------------
def test_builders(d):
    stream = lambda *a: items(d.cmd('bstream %d,%d,%d,%d 4096 -' % a)['v'])
    ok(items(d.cmd('binit 4096 7001 -')['v']) == [('eventPort', 7001), ('timingPort', 0)], 'initial setup')
    ok(stream(96, 6001, 6002, 5) == [('streams', [{'type': 96, 'dataPort': 6001, 'controlPort': 6002}])], 'stream 96')
    r = stream(103, 7001, 0, 6291456)[0][1][0]
    ok(list(r.items()) == [('type', 103), ('dataPort', 7001), ('audioBufferSize', 6291456), ('controlPort', 0)], 'stream 103')
    ok(stream(103, 1, 2, 4294967295)[0][1][0]['audioBufferSize'] == 4294967295, 'buffer size')
    ok(items(d.cmd('bfeed 103,44100 128 -')['v']) == [('streams', [{'type': 103, 'sr': 44100.0}])], 'feedback')
    for c in range(256):
        h = d.cmd(f'bcmd 2048 {c} -')['v']
        if c <= 5: ok(items(h) == [('type', 'sendMediaRemoteCommand'), ('modernMediaRemoteCommand', c)], 'command')
        else: ok(h == '', 'bad command')
    for v in (0, 0.25, 1):
        ok(items(d.cmd(f'bvol {v} 2048 -')['v']) == [('type', 'sendMediaRemoteCommand'), ('value', 'dvlc'), ('volume', float(v)), ('params', {'volume': float(v)})], 'volume')
    for v in ('-1', '1.5', 'nan', 'inf', '-inf'): ok(d.cmd(f'bvol {v} 2048 -')['v'] == '', 'bad volume')
    for f in (0, 1, 2**32, 2**63 - 1):
        r = dict(items(d.cmd(f'binfo AA:BB,L,{f},-20 1024 -')['v'])); ok(r['features'] == f, 'features')
    r = items(d.cmd('binfo AA:BB:CC:01:02:03,Living,5,-20.5 1024 -')['v'])
    ok([k for k, _ in r] == ['deviceid', 'features', 'model', 'protovers', 'srcvers', 'vv', 'statusFlags', 'pk', 'pi', 'name', 'audioFormats', 'audioLatencies', 'initialVolume', 'supportedFormats'], 'info keys')
    r = dict(r); ok(r['deviceid'] == 'AA:BB:CC:01:02:03' and r['pk'] == bytes(range(1, 33)) and r['initialVolume'] == -20.5 and len(r['pi']) == 36, 'info values')
    ok(r['audioFormats'] == [{'type': 96, 'audioInputFormats': 0x01000000, 'audioOutputFormats': 0x01000000}], 'audioFormats')
    ok([x['type'] for x in r['audioLatencies']] == [96, 103] and r['supportedFormats']['bufferStream'] & (1 << 40), 'latencies, 7.1')
    for name in ('Хол', 'a&b', '😀x', 'é', 'x' * 64, 'колона' * 5):
        h = d.cmd(f'binfo AA:BB,{name},5,-20 1024 -')['v']; ok(dict(items(h))['name'] == name, f'name {name}')
    ok(d.cmd('binfo AA:BB,L,5,nan 4096 -')['v'] == '', 'bad volume in info')
    # no reply may run past the buffer, and a short buffer gives 0
    for op, full in (('binit %d 7001 -', 'binit 4096 7001 -'), ('bcmd %d 1 -', 'bcmd 4096 1 -')):
        need = len(d.cmd(full)['v']) // 2
        for cap in range(0, need + 3):
            line = op % cap
            ok((d.cmd(line)['v'] != '') == (cap >= need), f'capacity {line}')
    need = len(d.cmd('binfo AA:BB,L,5,-20 4096 -')['v']) // 2
    for cap in range(0, need + 2): ok((d.cmd(f'binfo AA:BB,L,5,-20 {cap} -')['v'] != '') == (cap >= need), 'info capacity')

def test_xml_b64(d):
    r = d.cmd('xml x 65536 -'); ok(r['term'] == 1 and r['over'] == 0, 'xml terminated')
    x = plistlib.loads(bytes.fromhex(r['v']))
    ok(x == {'name': 'A & B <c> "q" \'r\'', 'neg': -5, 'big': 2**64 - 1, 'pk': bytes([1, 2, 3, 250, 251]), 'none': b'', 'arr': [{'x': 1}], 'sub': {'y': 'z'}}, 'xml content')
    for cap in list(range(0, 700)):
        r = d.cmd(f'xml x {cap} -'); ok(r['over'] == 0 and (cap == 0 or r['term'] == 1), f'xml capacity {cap}')
    import base64
    for n in range(0, 40):
        raw = os.urandom(n); r = d.cmd(f'b64 {raw.hex() or "-"} 200 -'); want = base64.b64encode(raw).decode()
        ok(r['v'] == want and r['r'] == len(want) and r['need'] == len(want), 'base64')
        if n: ok(d.cmd(f'b64 {raw.hex()} {len(want) - 1} -')['r'] < 0, 'base64 too small')
        ok(d.cmd(f'b64 {raw.hex() or "-"} {len(want)} -')['r'] == len(want), 'base64 exact')

# ---- reads, against a model of the rules --------------------------------------
KEYS = ['x', 'type', 'ekey', 'eiv', 'shk', 'streams', 'ct', 'sr', 'rate', 'name', 'k' * 20]
def rnd_value(depth=0):
    c = random.random()
    if depth < 4 and c < .18: return {random.choice(KEYS): rnd_value(depth + 1) for _ in range(random.randrange(0, 5))}
    if depth < 4 and c < .28: return [rnd_value(depth + 1) for _ in range(random.randrange(0, 4))]
    return random.choice([
        lambda: random.randrange(0, 256), lambda: random.randrange(0, 70000), lambda: random.randrange(0, 2**33), lambda: -random.randrange(1, 2**40),
        lambda: random.random() * 1000, lambda: float(random.randrange(0, 100000)), lambda: 0.0, lambda: -2.0, lambda: 44100.0, lambda: 5e9, lambda: os.urandom(random.choice((0, 1, 16, 32, 64, 72, 300))),
        lambda: 'a' * random.choice((0, 1, 14, 15, 16, 300)), lambda: 'Хол', lambda: True, lambda: plistlib.UID(random.randrange(0, 300)),
    ])()
def rnd_doc():
    d = {k: rnd_value() for k in random.sample(KEYS, random.randrange(0, 6))}
    if random.random() < .7:
        d['streams'] = [rnd_stream() for _ in range(random.randrange(0, 4))]
    return d
def rnd_stream():
    if random.random() < .1: return rnd_value(3)
    s = {k: rnd_value(2) for k in random.sample(['ct', 'sr', 'spf', 'ss', 'ch', 'audioFormat', 'controlPort', 'name'], random.randrange(0, 6))}
    if random.random() < .2: s['sr'] = random.choice((44100.0, 48000.0, 1.0, 1.5, 0.0, -1.0, 44100, 2147483647.0, 2147483648.0))
    if random.random() < .9: s['type'] = random.choice((96, 103, 5, 'x'))
    for k in ('ekey', 'eiv', 'shk'):
        if random.random() < .5: s[k] = os.urandom(random.choice((16, 32, 72))) if random.random() < .9 else 'str'
    return s

def is_int(v): return isinstance(v, int) and not isinstance(v, (bool, plistlib.UID))
def deep(o, key, cap, root=True):
    """returns bytes or None"""
    if isinstance(o, dict):
        if root and key in o:
            v = o[key]; return v if isinstance(v, bytes) and len(v) <= cap else None
        v = o.get(key)
        if not root and isinstance(v, bytes) and len(v) <= cap: return v
        for v in o.values():
            r = deep(v, key, cap, False)
            if r is not None: return r
    elif isinstance(o, list):
        for v in o:
            r = deep(v, key, cap, False)
            if r is not None: return r
    return None

def kv_expect(stream):
    out = []
    for k, v in stream.items():
        if isinstance(v, plistlib.UID): out.append([k, 4, 0, 0])
        elif isinstance(v, bool): out.append([k, 0, 0, 0])
        elif isinstance(v, int): out.append([k, 1, 0, v])
        elif isinstance(v, float):
            if k != 'sr': out.append([k, 0, 0, 0])
            elif 1 <= v <= 2147483647 and v == int(v): out.append([k, 1, 0, int(v)])
            else: return None
        elif isinstance(v, bytes): out.append([k, 2, len(v), 0])
        elif isinstance(v, str): out.append([k, 3, len(v.encode('utf-16-be')) // 2, 0])
        elif isinstance(v, list): out.append([k, 5, 0, 0])
        elif isinstance(v, dict): out.append([k, 6, 0, 0])
    return out

def check_doc(d, doc):
    b = B(doc); key = random.choice(KEYS)
    r = d.read('int', key, 0, b); v = doc.get(key); ok(r == ({'r': 1, 'v': v} if is_int(v) else {'r': 0, 'v': 0}), f'int {key} {doc}')
    r = d.read('real', key, 0, b)
    if isinstance(v, float) or is_int(v): ok(r['r'] == 1 and float(r['v']) == float(v), 'real')
    else: ok(r['r'] == 0, 'real absent')
    cap = random.choice((1, 2, 15, 16, 17, 400))
    r = d.read('str', key, cap, b)
    if isinstance(v, str) and v.isascii() and len(v) < cap: ok(r == {'r': 1, 'v': v}, 'str')
    else: ok(r['r'] == 0, 'str absent')
    cap = random.choice((0, 1, 16, 32, 72, 400)); want = deep(doc, key, cap)
    r = d.read('data', key, cap, b); ok(r == {'r': int(want is not None), 'v': (want or b'').hex()}, f'deep {key} {cap} {doc}')
    streams = doc.get('streams')
    r = d.read('count', '-', 0, b)
    ok(r == ({'r': 1, 'v': len(streams)} if isinstance(streams, list) else {'r': 0, 'v': 0}), 'count')
    for i in range(len(streams) + 1 if isinstance(streams, list) else 1):
        s = streams[i] if isinstance(streams, list) and i < len(streams) else None
        r = d.read('info', '-', i, b); good = isinstance(s, dict) and is_int(s.get('type'))
        if good:
            ln = lambda k: len(s[k]) if isinstance(s.get(k), bytes) else 0
            ok(r == {'r': 1, 't': s['type'], 'e': ln('ekey'), 'i': ln('eiv'), 's': ln('shk')}, 'info')
        else: ok(r['r'] == 0 and r['t'] == -1 and r['e'] == r['i'] == r['s'] == 0, 'info absent')
        r = d.read('kv', '32', i, b)
        want = kv_expect(s) if isinstance(s, dict) and s else None
        if want is not None: ok(r['r'] == 1 and r['kv'] == want, f'kv {s} {r}')
        else: ok(r['r'] == 0, 'kv refused')
        if want:
            n = len(want)
            ok(d.read('kv', str(n), i, b)['r'] == 1, 'kv exact')
            if n: ok(d.read('kv', str(n - 1), i, b)['r'] == 0, 'kv short')
    want_type = random.choice((96, 103, 5)); caps = random.choice(('64,16,32', '256,64,64', '0,16,32', '72,0,0'))
    ce, ci, cs = map(int, caps.split(','))
    r = d.read('crypto', caps, want_type, b); hit = None
    for s in (streams if isinstance(streams, list) else []):
        if isinstance(s, dict) and is_int(s.get('type')) and s['type'] == want_type: hit = s; break
    def field(k, c):
        v = hit.get(k) if hit else None
        return v if isinstance(v, bytes) and 0 < len(v) <= c and c else b''
    e, i, s = field('ekey', ce), field('eiv', ci), field('shk', cs)
    ok(r == {'r': int(bool(e or i or s)), 'el': len(e), 'il': len(i), 'sl': len(s), 'e': e.hex(), 'i': i.hex(), 's': s.hex()}, f'crypto {r} {hit}')

def test_reads(d, runs):
    for _ in range(runs): check_doc(d, rnd_doc())
    # encodings plistlib does not produce
    one = lambda v: enc([D([1], [2]), A(b'x'), v])
    for v, want in ((b'\x10\xff', 255), (b'\x11\xff\xff', 65535), (b'\x12\xff\xff\xff\xff', 4294967295), (b'\x13' + b'\xff' * 8, -1)):
        ok(d.read('int', 'x', 0, one(v)) == {'r': 1, 'v': want}, 'int widths')
    ok(d.read('int', 'x', 0, one(b'\x14' + b'\0' * 16))['r'] == 0, '16-byte int')
    ok(d.read('real', 'x', 0, one(b'\x22' + struct.pack('>f', 1.5))) == {'r': 1, 'v': 1.5}, 'real 4')
    ok(d.read('int', 'x', 0, enc([D([1], [2]), b'\x61\x00x', b'\x10\x05'])) == {'r': 1, 'v': 5}, 'UTF-16 key')
    ok(d.read('str', 'x', 100, one(b'\x5f\x10\x03abc')) == {'r': 1, 'v': 'abc'}, 'long form count')
    ok(d.read('int', 'x', 0, enc([b'\xd1\x00\x01\x00\x02', A(b'x'), b'\x10\x07'], oref=2, osz=2)) == {'r': 1, 'v': 7}, 'two-byte refs')
    ok(d.read('int', 'x', 0, enc([b'\x10\x01', D([2], [3]), A(b'x'), b'\x10\x09'], top=1)) == {'r': 1, 'v': 9}, 'root not first')

BAD = [b'', b'bplist00', b'xplist00' + B({'x': 1})[8:], B({'x': 1})[:6] + b'01' + B({'x': 1})[8:], B({'x': 1})[:-1], B({'x': 1}) + b'junk', B({'x': 1})[:20],
       B([{'x': 1}]), B(5),
       enc([D([1], [0]), A(b'x')]), enc([D([1], [9]), A(b'x')]), enc([D([9], [1]), A(b'x')]), enc([D([1], [2]), A(b'x'), b'\x10\x09'], top=9),
       enc([D([1], [2]), A(b'x'), b'\x15' + b'\0' * 32]), enc([D([1], [2]), A(b'x'), b'\x5f\x13' + b'\xff' * 8]), enc([D([1], [2]), A(b'x'), b'\x5f\x20\x03abc']),
       enc([D([1], [2]), A(b'x'), b'\x5f\x10\x30abc']), B({'x': 1})[:-8] + (3).to_bytes(8, 'big')]
OPS = (('int', 'x', 0), ('real', 'x', 0), ('str', 'x', 100), ('data', 'x', 100), ('count', '-', 0), ('info', '-', 0), ('kv', '32', 0), ('crypto', '-', 96))
def test_malformed(d, runs):
    for b in BAD:
        for op in OPS: ok(d.read(op[0], op[1], op[2], b)['r'] == 0, f'malformed {op} {b.hex()[:40]}')
    # a chain of references that grows exponentially must end quickly
    n = 60; objs = []
    for i in range(n - 2): objs.append(b'\xa2' + bytes([i + 1, i + 1]))
    objs.append(D([n - 1], [n - 1])); objs.append(A(b'y'))
    ok(d.read('data', 'x', 100, enc([D([1], [1]) ] + [A(b'y')]))['r'] == 0, 'self reference')
    doc = enc([D([1], [2]), A(b'a')] + [b'\xa2' + bytes([i + 3, i + 3]) for i in range(40)] + [b'\x10\x01'])
    ok(d.read('data', 'x', 100, doc)['r'] == 0, 'reference bomb')
    # random damage: nothing may crash (the sanitizers would stop the driver)
    seeds = [B(rnd_doc()) for _ in range(40)]
    for _ in range(runs):
        b = bytearray(random.choice(seeds))
        for _ in range(random.randrange(1, 6)):
            where = random.randrange(len(b)) if random.random() < .5 else len(b) - random.randrange(1, min(33, len(b)))
            b[where] = random.choice((0, 1, 0x0f, 0x7f, 0x80, 0xff, random.randrange(256)))
        if random.random() < .1: b = b[:random.randrange(len(b))]
        op = random.choice(OPS); d.read(op[0], op[1], op[2], bytes(b)); checks_add()
def checks_add():
    global checks; checks += 1

def compare(a, b, runs):
    """Same requests to two builds. Intended differences are skipped: the stream
    crypto lookup, deep nesting, reals of 4 bytes, odd keys."""
    def same(line):
        ra, rb = a.cmd(line), b.cmd(line)
        # what a failed call leaves in its outputs is not used by anyone
        ok(ra == rb or (ra['r'] == 0 and rb['r'] == 0), f'{line[:120]}\n new {ra}\n old {rb}')
    for _ in range(runs):
        doc = rnd_doc(); h = B(doc).hex(); key = random.choice(KEYS)
        same(f'int {key} 0 {h}'); same(f'real {key} 0 {h}'); same(f'str {key} {random.choice((1, 16, 400))} {h}')
        same(f'data {key} {random.choice((0, 16, 32, 72, 400))} {h}'); same(f'count - 0 {h}')
        for i in range(3): same(f'info - {i} {h}'); same(f'kv 32 {i} {h}'); same(f'kv 2 {i} {h}')
        # how SETUP reads the keys: stream lookup, then the deep search
        ca = a.cmd(f'crypto 64,16,32 96 {h}')
        for k, cap, ln, val in (('ekey', 64, 'el', 'e'), ('eiv', 16, 'il', 'i'), ('shk', 32, 'sl', 's')):
            old = b.cmd(f'data {k} {cap} {h}')
            if ca['r'] and (ca['el'] or ca['sl']):
                # with one stream of that type both routes must give the same key
                st = doc.get('streams'); first = [s for s in st if isinstance(s, dict) and s.get('type') == 96]
                top_clean = k not in doc and not any(isinstance(v, (dict, list)) and k in str(v) for kk, v in doc.items() if kk != 'streams')
                if len(st) == 1 and len(first) == 1 and top_clean and old['r']:
                    ok(old['v'] == ca[val], f'setup key {k}')
    for line in ('binit 4096 7001 -', 'binit 4096 0 -', 'bstream 96,6001,6002,5 4096 -', 'bstream 103,7001,0,6291456 4096 -', 'bfeed 103,44100 4096 -', 'bfeed 103,48000 4096 -',
                 'bcmd 2048 0 -', 'bcmd 2048 5 -', 'bcmd 2048 6 -', 'bvol 0.5 2048 -', 'bvol 2 2048 -', 'binfo AA:BB:CC:01:02:03,Living,1234567890123,-20 8192 -'):
        ra, rb = a.cmd(line).get('v', ''), b.cmd(line).get('v', '')
        ok(bool(ra) == bool(rb) and (not ra or items(ra) == items(rb)), f'reply {line}')

if __name__ == '__main__':
    random.seed(os.environ.get('SEED'))
    runs = int(os.environ.get('RUNS', 3000))
    d = Dut(sys.argv[1])
    test_builders(d); test_xml_b64(d); test_reads(d, runs); test_malformed(d, runs * 5)
    d.close()
    if len(sys.argv) > 2:
        a, b = Dut(sys.argv[1]), Dut(sys.argv[2]); compare(a, b, runs); a.close(); b.close()
        print('other build: same results')
    print(f'OK, {checks} checks')
