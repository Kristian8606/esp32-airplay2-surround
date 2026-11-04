#!/usr/bin/env python3
"""End-to-end tests of the HAP module through driver.c.

    ./make_driver.sh ../../main/hap /tmp/hap_driver <libs prefix>
    python3 test_hap.py /tmp/hap_driver [other_driver_to_compare]

The client side is independent of the firmware code. With a second driver the
same client also has to pass against that build, and an identity stored by one
must load in the other.
"""
import os, random, sys
from test_hap_client import *

CID = b'11111111-2222-3333-4444-555555555555'
READ, WRITE, SALT = 'Control-Read-Encryption-Key', 'Control-Write-Encryption-Key', 'Control-Salt'
checks = 0
def ok(cond, what):
    global checks; checks += 1
    if not cond: raise SystemExit(f'FAILED: {what}')

def setup_m1(d, transient=True):
    items = [(0, b'\0'), (6, b'\1')] + ([(0x13, b'\x10')] if transient else [])
    r = d.step('setup1', tlv(*items)); ok(r['err'] == 0, 'M1 accepted')
    m = untlv(r['out']); ok(m[6] == b'\2' and len(m[2]) == 16 and len(m[3]) == 384, 'M2 shape')
    return m[2], int.from_bytes(m[3], 'big'), r

def transient(d, srp=None, **enc):
    d.cmd('new'); salt, B, _ = setup_m1(d); c = srp or Srp()
    K, M1, M2 = c.respond(salt, B, **enc)
    r = d.step('setup3', tlv((6, b'\3'), (3, strip(c.A)), (4, M1)))
    return K, M2, r, c, B

def test_transient(d):
    K, M2, r, _, _ = transient(d); m = untlv(r['out'])
    ok(r['err'] == 0 and m[6] == b'\4' and m[4] == M2, 'server proof')
    ok(r['ps'] == 4 and r['established'] == 1 and r['transient'] == 1, 'transient state')
    ok(r['k'] == K.hex() and r['shared'] == K[:32].hex(), 'session key')
    ok(r['enc'] == hkdf(SALT, READ, K).hex() and r['dec'] == hkdf(SALT, WRITE, K).hex(), 'control keys')
    ok(r['audio'] == hkdf(SALT, READ, K[:32]).hex(), 'audio key')

def test_refusals(d):
    d.cmd('new'); salt, B, _ = setup_m1(d); c = Srp(pw=b'0000'); _, M1, _ = c.respond(salt, B)
    r = d.step('setup3', tlv((6, b'\3'), (3, strip(c.A)), (4, M1)))
    ok(r['out'] == bytes.fromhex('060104070102') and r['established'] == 0 and r['k'] == '', 'wrong password')
    for bad_a in (b'\0' * 384, pad(N)):
        d.cmd('new'); setup_m1(d)
        r = d.step('setup3', tlv((6, b'\3'), (3, bad_a), (4, b'\0' * 64)))
        ok(r['out'] == bytes.fromhex('060104070102') and r['established'] == 0, 'A = 0 mod N refused')
    d.cmd('new'); r = d.step('setup3', tlv((6, b'\3'), (3, b'\5' * 384), (4, b'\0' * 64))); ok(r['err'] != 0, 'M3 before M1')
    d.cmd('new'); r = d.step('setup5', tlv((6, b'\5'), (5, b'\0' * 40))); ok(r['err'] != 0, 'M5 before M1')
    d.cmd('new'); r = d.step('verify3', tlv((6, b'\3'), (5, b'\0' * 40))); ok(r['err'] != 0 and r['established'] == 0, 'V3 before V1')
    d.cmd('new'); r = d.step('raw3', b'\0' * 68); ok(r['err'] != 0 and r['established'] == 0, 'raw3 before raw1')
    for key in (b'\0' * 32, (1).to_bytes(32, 'little')):
        d.cmd('new'); r = d.step('verify1', tlv((6, b'\1'), (3, key))); ok(r['err'] != 0 and r['pv'] == 0, 'low-order key')
        d.cmd('new'); r = d.step('raw1', b'\1\0\0\0' + key + b'\7' * 32); ok(r['err'] != 0 and r['pv'] == 0, 'raw low-order key')
    d.cmd('new'); r = d.step('verify1', tlv((6, b'\1'), (3, b'\5' * 31))); ok(r['err'] != 0, 'short key')
    d.cmd('new'); r = d.step('raw1', b'\1\0\0'); ok(r['err'] != 0, 'short raw M1')

def test_persistent(d, pk):
    d.cmd('new'); salt, B, r = setup_m1(d, transient=False); ok(r['transient'] == 0, 'not transient')
    c = Srp(); K, M1, M2 = c.respond(salt, B)
    r = d.step('setup3', tlv((6, b'\3'), (3, strip(c.A)), (4, M1))); ok(untlv(r['out']).get(4) == M2 and r['established'] == 0, 'M4')
    ed = ed25519.Ed25519PrivateKey.generate(); ek = hkdf('Pair-Setup-Encrypt-Salt', 'Pair-Setup-Encrypt-Info', K)
    X = hkdf('Pair-Setup-Controller-Sign-Salt', 'Pair-Setup-Controller-Sign-Info', K)
    # a wrong signature, then a wrong key, may not finish the pairing
    bad = seal(ek, b'PS-Msg05', tlv((1, CID), (3, raw_pub(ed)), (10, b'\1' * 64)))
    r = d.step('setup5', tlv((6, b'\5'), (5, bad))); ok(r['out'] == bytes.fromhex('060106070102') and r['ps'] == 4, 'bad M5 signature')
    r = d.step('setup5', tlv((6, b'\5'), (5, b'\0' * 200))); ok(r['out'] == bytes.fromhex('060106070102') and r['ps'] == 4, 'bad M5 tag')
    good = seal(ek, b'PS-Msg05', tlv((1, CID), (3, raw_pub(ed)), (10, ed.sign(X + CID + raw_pub(ed)))))
    r = d.step('setup5', tlv((6, b'\5'), (5, good))); m = untlv(r['out']); ok(m[6] == b'\6' and r['ps'] == 6, 'M6')
    inner = untlv(unseal(ek, b'PS-Msg06', m[5])); ok(inner[3] == pk, 'M6 key')
    AX = hkdf('Pair-Setup-Accessory-Sign-Salt', 'Pair-Setup-Accessory-Sign-Info', K)
    ed25519.Ed25519PublicKey.from_public_bytes(pk).verify(inner[10], AX + inner[1] + pk); ok(True, 'M6 signature')

def verify_m1(d):
    ck = x25519.X25519PrivateKey.generate(); r = d.step('verify1', tlv((6, b'\1'), (3, raw_pub(ck))))
    m = untlv(r['out']); ok(r['err'] == 0 and m[6] == b'\2' and r['pv'] == 2, 'V2')
    sh = ck.exchange(x25519.X25519PublicKey.from_public_bytes(m[3]))
    return ck, m, sh, hkdf('Pair-Verify-Encrypt-Salt', 'Pair-Verify-Encrypt-Info', sh), r

def test_verify(d, pk):
    d.cmd('new'); ck, m, sh, vk, r = verify_m1(d); ok(r['shared'] == sh.hex() and r['established'] == 0, 'V2 secret')
    inner = untlv(unseal(vk, b'PV-Msg02', m[5])); ok(inner[1] == b'AA:BB:CC:01:02:03', 'V2 identifier')
    ed25519.Ed25519PublicKey.from_public_bytes(pk).verify(inner[10], m[3] + inner[1] + raw_pub(ck)); ok(True, 'V2 signature')
    r = d.step('verify3', tlv((6, b'\3'), (5, b'\0' * 80)))
    ok(r['err'] != 0 and r['out'] == bytes.fromhex('060104070102') and r['established'] == 0 and r['pv'] == 2, 'V3 wrong key')
    ed = ed25519.Ed25519PrivateKey.generate()
    r = d.step('verify3', tlv((6, b'\3'), (5, seal(vk, b'PV-Msg03', tlv((1, CID), (10, ed.sign(raw_pub(ck) + CID + m[3])))))))
    ok(r['out'] == b'\6\1\4' and r['pv'] == 4 and r['established'] == 1, 'V4')
    ok(r['enc'] == hkdf(SALT, READ, sh).hex() and r['dec'] == hkdf(SALT, WRITE, sh).hex(), 'verify control keys')
    ok(r['audio'] == hkdf(SALT, READ, sh).hex(), 'verify audio key')

def test_raw(d, pk):
    d.cmd('new'); ck = x25519.X25519PrivateKey.generate(); ed = ed25519.Ed25519PrivateKey.generate()
    r = d.step('raw1', b'\1\0\0\0' + raw_pub(ck) + raw_pub(ed)); ok(r['err'] == 0 and len(r['out']) == 96 and r['pv'] == 2, 'raw M2')
    sp = r['out'][:32]; sh = ck.exchange(x25519.X25519PublicKey.from_public_bytes(sp)); ok(r['shared'] == sh.hex(), 'raw secret')
    ctr = Cipher(algorithms.AES(H(b'Pair-Verify-AES-Key', sh)[:16]), modes.CTR(H(b'Pair-Verify-AES-IV', sh)[:16])).encryptor()
    ed25519.Ed25519PublicKey.from_public_bytes(pk).verify(ctr.update(r['out'][32:]), sp + raw_pub(ck)); ok(True, 'raw signature')
    r = d.step('raw3', b'\0\0\0\0' + b'\5' * 10); ok(r['err'] != 0 and r['established'] == 0, 'short raw M3')
    r = d.step('raw3', b'\0\0\0\0' + ctr.update(ed.sign(raw_pub(ck) + sp)))
    ok(r['err'] == 0 and r['out'] == b'' and r['pv'] == 4 and r['established'] == 1, 'raw M4')
    ok(r['audio'] == hkdf(SALT, READ, sh).hex(), 'raw audio key')
    ok(r['enc'] == hkdf(SALT, WRITE, sh).hex() and r['dec'] == hkdf(SALT, READ, sh).hex(), 'raw keys')

def test_transient_then_verify(d):
    K, _, r, _, _ = transient(d); ck, m, sh, vk, r = verify_m1(d)
    ok(r['established'] == 1 and r['enc'] == hkdf(SALT, READ, K).hex(), 'control keys kept until V3')
    r = d.step('verify3', tlv((6, b'\3'), (5, seal(vk, b'PV-Msg03', b''))))
    ok(r['enc'] == hkdf(SALT, READ, sh).hex() and r['k'] == K.hex() and r['transient'] == 1, 'keys after V3')

def test_leading_zeros(d, runs):
    """All the hash forms that differ only by leading zero bytes must pair."""
    seen = {'A': 0, 'S_pad': 0, 'S_strip': 0, 'B': 0}
    # A client key whose top byte is zero
    a = None
    while a is None:
        cand = int.from_bytes(os.urandom(32), 'big')
        if pow(G, cand, N) >> (8 * 383) == 0: a = cand
    for pad_ab in (False, True):
        K, M2, r, c, _ = transient(d, Srp(a=a), pad_AB=pad_ab); seen['A'] += 1
        ok(untlv(r['out']).get(4) == M2 and r['k'] == K.hex(), f'A with leading zero, padded={pad_ab}')
    i = -1
    while i + 1 < runs or (i < 12 * runs and not (seen['S_pad'] and seen['S_strip'] and seen['B'])):
        i += 1
        pad_s, pad_ab = bool(i & 1), bool(i & 2)
        K, M2, r, c, B = transient(d, pad_S=pad_s, pad_AB=pad_ab)
        ok(untlv(r['out']).get(4) == M2 and r['k'] == K.hex(), f'run {i}, pad_S={pad_s}, pad_AB={pad_ab}')
        if c.S >> (8 * 383) == 0: seen['S_pad' if pad_s else 'S_strip'] += 1
        if B >> (8 * 383) == 0: seen['B'] += 1
    ok(seen['S_pad'] and seen['S_strip'] and seen['B'], f'edge cases reached: {seen}')
    return seen

def test_tlv_and_fuzz(d, runs):
    # broken messages in every state
    d.cmd('new')
    for i in range(runs):
        for step in ('setup1', 'setup3', 'setup5', 'verify1', 'verify3', 'raw1', 'raw3'):
            if random.random() < .5: d.cmd('new')
            if random.random() < .3 and step in ('setup3', 'setup5'): setup_m1(d)
            n = random.choice((0, 1, 2, 3, 4, 31, 32, 35, 36, 67, 68, 255, 256, 257, 511, 600, 1500))
            data = os.urandom(n)
            if random.random() < .7 and n >= 3:
                data = bytes([random.choice((1, 3, 4, 5, 6, 0x13)), random.randrange(256)]) + data[2:]
            d.step(step, data)  # must not crash or trip the sanitizers
    d.cmd('new'); ok(d.cmd('state').get('established') == 0, 'alive after fuzzing')

def run(path, leading_runs, fuzz_runs):
    d = Dut(path); ok(d.cmd('init')['err'] == 0, 'init'); pk = bytes.fromhex(d.cmd('init')['pk']); ok(d.cmd('new')['ok'] == 1, 'new')
    test_transient(d); test_refusals(d); test_persistent(d, pk); test_verify(d, pk); test_raw(d, pk); test_transient_then_verify(d)
    seen = test_leading_zeros(d, leading_runs)
    test_tlv_and_fuzz(d, fuzz_runs)
    nvs = d.cmd('nvs'); ok(nvs['ns'] == 'airplay' and nvs['ed25519_pub'] == pk.hex() and len(nvs['ed25519_sec']) == 128, 'stored identity')
    d.close(); ok(d.p.returncode == 0, 'sanitizers clean')
    return nvs, seen

def identity_carries_over(a_path, b_path):
    a = Dut(a_path); a.cmd('init'); nvs = a.cmd('nvs'); a.close()
    b = Dut(b_path); b.cmd(f"nvsset ed25519_pub {nvs['ed25519_pub']}"); b.cmd(f"nvsset ed25519_sec {nvs['ed25519_sec']}")
    pk = b.cmd('init')['pk']; ok(pk == nvs['ed25519_pub'], 'identity loads'); b.cmd('new'); pkb = bytes.fromhex(pk)
    test_verify(b, pkb); b.close()

if __name__ == '__main__':
    leading = int(os.environ.get('LEADING_RUNS', 1500)); fuzz = int(os.environ.get('FUZZ_RUNS', 1500))
    nvs, seen = run(sys.argv[1], leading, fuzz); print('leading zeros met:', seen)
    if len(sys.argv) > 2:
        # the second build gets the shared flows only: it may not do persistent pairing
        d = Dut(sys.argv[2]); d.cmd('init'); pk = bytes.fromhex(d.cmd('init')['pk']); d.cmd('new')
        test_transient(d); test_verify(d, pk); test_raw(d, pk); d.close()
        identity_carries_over(sys.argv[2], sys.argv[1]); identity_carries_over(sys.argv[1], sys.argv[2])
        print('other build: same client passes, identities carry over both ways')
    print(f'OK, {checks} checks')
