#!/usr/bin/env python3
"""Tests of socket_utils, rtsp_events, mdns_airplay and audio_crypto through driver.c."""
import json, os, random, subprocess, sys
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

checks = 0
def ok(cond, what):
    global checks; checks += 1
    if not cond: raise SystemExit(f'FAILED: {what}')

class Dut:
    def __init__(self, path):
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    def __call__(self, line):
        self.p.stdin.write(line + '\n'); self.p.stdin.flush()
        out = self.p.stdout.readline()
        if not out: raise SystemExit(f'FAILED: driver stopped on {line[:120]}')
        return json.loads(out)
    def close(self):
        self.p.stdin.close(); self.p.wait(); ok(self.p.returncode == 0, 'sanitizers clean')

KEY_HEX = ''.join('%02x' % ((i * 7 + 1) & 255) for i in range(32))
def test_mdns(d):
    calls = d('mdns')['calls']
    ok(calls[0] == ['init'] and calls[1] == ['host', 'h-Living-Room'], 'mdns start')
    ok(calls[2][:5] == ['svc', 'Living Room', '_airplay', '_tcp', 7000], '_airplay service')
    ok(calls[2][5] == [['deviceid', 'AA:BB:CC:01:02:03'], ['features', '0x401C4A00,0x1C340'], ['flags', '0x4'], ['model', 'AudioAccessory6,1'], ['pk', KEY_HEX],
                       ['pi', '11111111-2222-4333-8444-555555555555'], ['srcvers', '377.40.00'], ['vv', '2'], ['acl', '0']], '_airplay records')
    ok(calls[3][:5] == ['svc', 'AABBCC010203@Living Room', '_raop', '_tcp', 7000], '_raop service')
    ok(calls[3][5] == [['am', 'AudioAccessory6,1'], ['cn', '0,1,2,3'], ['da', 'true'], ['et', '0,3,5'], ['ft', '0x401C4A00,0x1C340'], ['md', '0,2'], ['pk', KEY_HEX],
                       ['sf', '0x4'], ['tp', 'UDP'], ['vn', '65537'], ['vs', '377.40.00'], ['vv', '2']], '_raop records')
    long = d('mdns ' + 'n' * 80)['calls']; ok(long[2][1] == 'n' * 63 and long[3][1] == 'AABBCC010203@' + 'n' * 50, 'long name')

def test_sockets(d):
    r = d('udp 0 0 0'); ok(r['fd'] == 1 and r['port_ok'] and r['any'] and r['type'] == 2 and not r['reuse'] and r['timeo'] == 0, 'udp')
    ok(d('udp 0 5 0')['timeo'] == 5, 'udp timeout'); a, b = d('udp 0 0 0')['rcvbuf'], d('udp 0 0 65536')['rcvbuf']; ok(a != b, 'udp buffer')
    r = d('tcp 0 1 0'); ok(r['fd'] == 1 and r['port_ok'] and r['type'] == 1 and r['reuse'] and r['listening'] and not r['nonblock'], 'tcp')
    ok(d('tcp 0 5 1')['nonblock'] == 1, 'tcp non-blocking'); ok(d('udpnull')['fd'] == 1 and d('tcpnull')['fd'] == 1, 'no port wanted')
    held = d('hold')['port']; r = d(f'tcp {held} 1 0'); ok(r['fd'] == -1 and r['bound'] == 77, 'busy port')

def test_events(d):
    ok(d('evnull')['r'] == -1, 'no callback'); ok(d('evdiff 10')['r'] == [0, 0, 0, 0, -1, -1, -1, -1, -1, -1], 'four listeners')
    ok(d('evdiff 2')['r'] == [0, 0], 'same callback again'); ok(d('emit 1')['log'] == [100, 111, 122, 133], 'order and user data')

def crypto_case(k):
    n = random.choice((0, 1, 5, 15, 16, 17, 100, 1400)); pt = os.urandom(n); hdr = os.urandom(12); non = os.urandom(8)
    return pt, hdr, ChaCha20Poly1305(k).encrypt(b'\0' * 4 + non, pt, hdr[4:12]) + non

def test_crypto(d, runs):
    k = os.urandom(32); K = k.hex()
    for _ in range(runs):
        pt, hdr, pay = crypto_case(k); full = hdr + pay; cap = 4096; want = {'r': len(pt), 'v': pt.hex()}
        ok(d(f'rtp 1 {K} {cap} {pay.hex()} {full.hex()}') == want, 'rtp'); ok(d(f'buf 1 {K} {cap} {full.hex()} -') == want, 'buffered')
        ok(d(f'buf 1 {K} {len(pt)} {full.hex()} -') == want, 'exact capacity')
        if pt: ok(d(f'buf 1 {K} {len(pt) - 1} {full.hex()} -')['r'] == -1, 'small capacity')
        bad = bytearray(full); bad[random.randrange(4, len(bad))] ^= 1  # bytes 0-3 are not authenticated
        ok(d(f'buf 1 {K} {cap} {bytes(bad).hex()} -')['r'] == -1, 'damaged packet')
        ok(d(f'buf 1 {K} {cap} {full[:random.randrange(len(full))].hex() or "-"} -')['r'] == -1, 'cut packet')
        ok(d(f'rtp 1 {K} {cap} {pay.hex()} {hdr.hex()}')['r'] == -1, 'header too short for the payload')
    ok(d(f'rtp 0 {K} 100 0102 800000000000000000000000')['v'] == '0102', 'plain rtp'); ok(d(f'buf 0 {K} 100 8000000000000000000000000102 -')['v'] == '0102', 'plain buffered')

def compare(a, b, runs):
    k = os.urandom(32); K = k.hex()
    for line in ('mdns', 'mdns Хол x', 'mdns a=b', 'mdns ' + 'n' * 80): ok(a(line) == b(line), line)
    for _ in range(runs):
        pt, hdr, pay = crypto_case(k); full = hdr + pay
        m = random.random()
        if m < .3: f = bytearray(full); f[random.randrange(len(f))] ^= 1; full = bytes(f); pay = full[12:]
        elif m < .4: full = full[:random.randrange(len(full))]; pay = full[12:]
        cap = random.choice((0, 1, max(len(pt) - 1, 0), len(pt), len(pt) + 1, 4096)); ty = random.choice((1, 1, 1, 0))
        for line in (f'rtp {ty} {K} {cap} {pay.hex() or "-"} {full.hex() or "-"}', f'buf {ty} {K} {cap} {full.hex() or "-"} -'):
            ok(a(line) == b(line), f'{line[:30]} {a(line)} {b(line)}')

if __name__ == '__main__':
    d = Dut(sys.argv[1]); test_mdns(d); test_sockets(d); test_events(d); test_crypto(d, int(os.environ.get('RUNS', 2000))); d.close()
    if len(sys.argv) > 2:
        a, b = Dut(sys.argv[1]), Dut(sys.argv[2]); compare(a, b, 3000); a.close(); b.close(); print('other build: same results')
    print(f'OK, {checks} checks')
