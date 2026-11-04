"""Independent iOS-style client for the HAP module, written from the public
descriptions of the protocol (RFC 5054, RFC 5869, HomeKit pair-setup/verify)."""
import hashlib, json, os, subprocess
from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes, serialization

def _pi_floor(bits):
    """floor(pi * 2^bits), Machin's formula on integers."""
    guard = bits + 64; one = 1 << guard
    def atan_inv(x):
        total = term = one // x; x2 = x * x; n = 3; sign = -1
        while term:
            term //= x2; total += sign * (term // n); n += 2; sign = -sign
        return total
    return (4 * (4 * atan_inv(5) - atan_inv(239))) >> (guard - bits)

# RFC 3526 / RFC 5054 3072-bit group
N = 2**3072 - 2**3008 - 1 + 2**64 * (_pi_floor(2942) + 1690314)
G = 5
NLEN = 384
def H(*parts):
    h = hashlib.sha512()
    for p in parts: h.update(p)
    return h.digest()
def pad(i): return i.to_bytes(NLEN, 'big')
def strip(i): return i.to_bytes((i.bit_length() + 7) // 8, 'big')

def tlv(*items):
    out = b''
    for t, v in items:
        if not v: out += bytes([t, 0])
        while v:
            out += bytes([t, min(len(v), 255)]) + v[:255]; v = v[255:]
    return out
def untlv(b):
    d = {}; i = 0; last = None
    while i + 2 <= len(b):
        t, l = b[i], b[i + 1]; v = b[i + 2:i + 2 + l]; i += 2 + l
        d[t] = d.get(t, b'') + v if last == t else v
        last = t
    return d
def hkdf(salt, info, ikm, n=32):
    return HKDF(hashes.SHA512(), n, salt.encode(), info.encode()).derive(ikm)
def seal(key, nonce, data): return ChaCha20Poly1305(key).encrypt(b'\0' * 4 + nonce, data, None)
def unseal(key, nonce, data): return ChaCha20Poly1305(key).decrypt(b'\0' * 4 + nonce, data, None)
def raw_pub(k): return k.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)

class Dut:
    def __init__(self, path):
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    def cmd(self, line):
        self.p.stdin.write(line + '\n'); self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())
    def step(self, name, data=b''):
        r = self.cmd(f'{name} {data.hex()}'); r['out'] = bytes.fromhex(r.get('out', '')); return r
    def close(self):
        self.p.stdin.close(); self.p.wait()

class Srp:
    """Client side. pad_kS/pad_AB pick the encoding used for the hashes that
    the public descriptions leave ambiguous when a leading byte is zero."""
    def __init__(self, a=None, user=b'Pair-Setup', pw=b'3939'):
        self.user, self.pw = user, pw
        self.a = a or int.from_bytes(os.urandom(32), 'big')
        self.A = pow(G, self.a, N)
    def respond(self, salt, B, pad_S=True, pad_AB=False):
        k = int.from_bytes(H(pad(N), pad(G)), 'big')
        x = int.from_bytes(H(salt, H(self.user + b':' + self.pw)), 'big')
        u = int.from_bytes(H(pad(self.A), pad(B)), 'big')
        S = pow((B - k * pow(G, x, N)) % N, self.a + u * x, N)
        self.S = S
        K = H(pad(S)) if pad_S else H(strip(S))
        Af = pad(self.A) if pad_AB else strip(self.A); Bf = pad(B) if pad_AB else strip(B)
        hxor = bytes(p ^ q for p, q in zip(H(pad(N)), H(strip(G))))
        M1 = H(hxor, H(self.user), salt, Af, Bf, K)
        M2 = H(Af, M1, K)
        return K, M1, M2
