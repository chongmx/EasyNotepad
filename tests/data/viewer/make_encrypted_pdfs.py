#!/usr/bin/env python3
"""Writes the encrypted PDFs the viewer's tests open: one page each, "Locked text" over a box filled
through an Indexed colour space (its lookup is a string in the page's dictionary, so a string
decrypted wrongly shows as a wrong colour). Each file uses another of the standard security
handler's methods:

    rc4-40.pdf        RC4, 40-bit key (V1, R2); no user password (it only restricts), owner "owner"
    rc4-128.pdf       RC4, 128-bit key (V2, R3); user "café" (Latin-1 in the file), owner "owner"
    aes-128.pdf       AES-128 (V4, R4, crypt filters); user "user", owner "owner"; the page in an
                      object stream, a cross-reference stream, metadata left unencrypted
    aes-256.pdf       AES-256 (V5, R6); no user password, owner "owner"; a second content stream
                      that names the Identity crypt filter (left as it is)
    aes-256-user.pdf  AES-256 (V5, R6); user "pässwörd" (UTF-8), owner "owner"
    aes-256-r5.pdf    AES-256 (V5, R5, the deprecated revision); user "user", owner "owner"

Written from ISO 32000-2 (7.6) with Python's hashlib and the `cryptography` package for RC4 and
AES: a writer apart from the reader under test. Random bytes come from a seeded generator, so the
files rebuild byte for byte:

    python3 tests/data/viewer/make_encrypted_pdfs.py

tests/ViewerTests.inc (viewer_opens_encrypted_pdfs) checks what each must give.
"""
import hashlib
import os
import random
import struct
import zlib

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

try:
    from cryptography.hazmat.decrepit.ciphers.algorithms import ARC4
except ImportError:  # cryptography before 43
    ARC4 = algorithms.ARC4

HERE = os.path.dirname(os.path.abspath(__file__))
PAD = bytes.fromhex("28BF4E5E4E758A4164004E56FFFA01082E2E00B6D0683E802F0CA9FE6453697A")
PERMISSIONS = -3904  # printing and copying not allowed (bits 3 and 5 clear), the rest allowed


def rc4(key, data):
    return Cipher(ARC4(key), None).encryptor().update(data)


def aes_cbc(key, iv, data):
    e = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor()
    return e.update(data) + e.finalize()


def md5(b):
    return hashlib.md5(b).digest()


class Name(str):
    pass


class Ref:
    def __init__(self, n):
        self.n = n


class Stream:
    def __init__(self, d, data, encrypt=True):
        self.d, self.data, self.encrypt = d, data, encrypt


class Handler:
    """The standard security handler, writing."""

    def __init__(self, rng, v, r, user, owner, method, key_bytes=16, encrypt_metadata=True):
        self.rng, self.v, self.r, self.method, self.n = rng, v, r, method, key_bytes
        self.encrypt_metadata = encrypt_metadata
        self.id0 = bytes(rng.getrandbits(8) for _ in range(16))
        self.p = PERMISSIONS
        if r <= 4:
            self.o = self.owner_entry(user, owner)
            self.key = self.file_key(user)
            self.u = self.user_entry()
        else:
            self.key = self.random(32)
            uvs, uks, ovs, oks = self.random(8), self.random(8), self.random(8), self.random(8)
            self.u = self.hash(user, uvs, b"") + uvs + uks
            self.ue = aes_cbc(self.hash(user, uks, b""), bytes(16), self.key)
            self.o = self.hash(owner, ovs, self.u) + ovs + oks
            self.oe = aes_cbc(self.hash(owner, oks, self.u), bytes(16), self.key)
            perms = struct.pack("<i", self.p) + b"\xff\xff\xff\xff" + (b"T" if encrypt_metadata else b"F") + b"adb" + self.random(4)
            e = Cipher(algorithms.AES(self.key), modes.ECB()).encryptor()
            self.perms = e.update(perms) + e.finalize()

    def random(self, n):
        return bytes(self.rng.getrandbits(8) for _ in range(n))

    # Revisions 2 to 4: algorithms 2, 3, 4 and 5.
    def owner_entry(self, user, owner):
        h = md5((owner + PAD)[:32])
        if self.r >= 3:
            for _ in range(50):
                h = md5(h)
        key = h[:self.n]
        x = rc4(key, (user + PAD)[:32])
        if self.r >= 3:
            for i in range(1, 20):
                x = rc4(bytes(b ^ i for b in key), x)
        return x

    def file_key(self, user):
        data = (user + PAD)[:32] + self.o + struct.pack("<i", self.p) + self.id0
        if self.r >= 4 and not self.encrypt_metadata:
            data += b"\xff\xff\xff\xff"
        h = md5(data)
        if self.r >= 3:
            for _ in range(50):
                h = md5(h[:self.n])
        return h[:self.n]

    def user_entry(self):
        if self.r == 2:
            return rc4(self.key, PAD)
        x = rc4(self.key, md5(PAD + self.id0))
        for i in range(1, 20):
            x = rc4(bytes(b ^ i for b in self.key), x)
        return x + self.random(16)

    # Revisions 5 and 6: algorithm 2.B (revision 5: SHA-256 alone).
    def hash(self, password, salt, udata):
        k = hashlib.sha256(password + salt + udata).digest()
        if self.r == 5:
            return k
        i, e = 0, b""
        while i < 64 or e[-1] > i - 32:
            k1 = (password + k + udata) * 64
            e = aes_cbc(k[:16], k[16:32], k1)
            k = [hashlib.sha256, hashlib.sha384, hashlib.sha512][sum(e[:16]) % 3](e).digest()
            i += 1
        return k[:32]

    def encrypt(self, num, gen, data):
        if self.method == "AESV3":
            key = self.key
        else:
            salt = b"sAlT" if self.method == "AESV2" else b""
            key = md5(self.key + struct.pack("<i", num)[:3] + struct.pack("<i", gen)[:2] + salt)[:min(self.n + 5, 16)]
        if self.method == "RC4":
            return rc4(key, data)
        iv = self.random(16)
        pad = 16 - len(data) % 16
        return iv + aes_cbc(key, iv, data + bytes([pad]) * pad)

    def dictionary(self):
        d = {"Filter": Name("Standard"), "V": self.v, "R": self.r, "O": self.o, "U": self.u, "P": self.p}
        if self.v == 2:
            d["Length"] = self.n * 8
        if self.v >= 4:
            cfm = {"AESV2": "AESV2", "AESV3": "AESV3", "RC4": "V2"}[self.method]
            d["CF"] = {"StdCF": {"CFM": Name(cfm), "AuthEvent": Name("DocOpen"), "Length": self.n}}
            d["StmF"] = Name("StdCF")
            d["StrF"] = Name("StdCF")
            d["Length"] = self.n * 8
        if self.r >= 5:
            d["OE"], d["UE"], d["Perms"] = self.oe, self.ue, self.perms
        if not self.encrypt_metadata:
            d["EncryptMetadata"] = False
        return d


def serialize(o, enc):
    """`enc(bytes) -> bytes` encrypts this object's strings (None: left as they are)."""
    if o is None:
        return b"null"
    if isinstance(o, bool):
        return b"true" if o else b"false"
    if isinstance(o, Name):
        return b"/" + o.encode()
    if isinstance(o, (int, float)):
        return str(o).encode()
    if isinstance(o, bytes):
        return b"<" + (enc(o) if enc else o).hex().encode() + b">"
    if isinstance(o, Ref):
        return b"%d 0 R" % o.n
    if isinstance(o, list):
        return b"[" + b" ".join(serialize(x, enc) for x in o) + b"]"
    if isinstance(o, dict):
        return b"<<" + b"".join(b"/" + k.encode() + b" " + serialize(v, enc) for k, v in o.items()) + b">>"
    raise TypeError(o)


def page_objects(extra_contents=False):
    content = b"/CS0 cs 1 sc 10 10 60 30 re f BT /F1 12 Tf 10 70 Td (Locked text) Tj ET"
    objects = {
        1: {"Type": Name("Catalog"), "Pages": Ref(2)},
        2: {"Type": Name("Pages"), "Kids": [Ref(3)], "Count": 1, "MediaBox": [0, 0, 200, 100]},
        3: {"Type": Name("Page"), "Parent": Ref(2),
            "Resources": {"Font": {"F1": Ref(4)},
                          "ColorSpace": {"CS0": [Name("Indexed"), Name("DeviceRGB"), 1, bytes.fromhex("000000FF8000")]}},
            "Contents": [Ref(5), Ref(8)] if extra_contents else Ref(5)},
        4: {"Type": Name("Font"), "Subtype": Name("Type1"), "BaseFont": Name("Helvetica")},
        5: Stream({"Filter": Name("FlateDecode")}, zlib.compress(content)),
    }
    if extra_contents:  # the Identity crypt filter: stored as it is
        objects[8] = Stream({"Filter": [Name("Crypt"), Name("FlateDecode")], "DecodeParms": [{"Name": Name("Identity")}, None]},
                            zlib.compress(b" BT /F1 10 Tf 10 50 Td (Plain part) Tj ET"), encrypt=False)
    return objects


def write(name, h, objects, object_stream=False, metadata=False):
    objects = dict(objects)
    encrypt_num = 6
    objects[encrypt_num] = h.dictionary()
    if metadata:
        xmp = b'<?xpacket begin=""?><x:xmpmeta xmlns:x="adobe:ns:meta/"><dc:title>Plain metadata</dc:title></x:xmpmeta>'
        objects[1]["Metadata"] = Ref(7)
        objects[7] = Stream({"Type": Name("Metadata"), "Subtype": Name("XML")}, xmp, encrypt=h.encrypt_metadata)
    packed = [2, 3, 4] if object_stream else []
    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = {}
    trailer = {"Size": 0, "Root": Ref(1), "Encrypt": Ref(encrypt_num), "ID": [h.id0, h.id0]}

    def body(num, o):
        enc = None if num == encrypt_num else (lambda b, num=num: h.encrypt(num, 0, b))
        if isinstance(o, Stream):
            data = h.encrypt(num, 0, o.data) if o.encrypt and enc else o.data
            d = dict(o.d)
            d["Length"] = len(data)
            return serialize(d, enc) + b"\nstream\n" + data + b"\nendstream"
        return serialize(o, enc)

    if packed:  # the object stream: its objects' strings stay plain; the stream is encrypted
        heads, bodies = [], b""
        for num in packed:
            heads.append(b"%d %d" % (num, len(bodies)))
            bodies += serialize(objects.pop(num), None) + b"\n"
        head = b" ".join(heads) + b"\n"
        objects[9] = Stream({"Type": Name("ObjStm"), "N": len(packed), "First": len(head), "Filter": Name("FlateDecode")},
                            zlib.compress(head + bodies))
    for num in sorted(objects):
        offsets[num] = len(out)
        out += b"%d 0 obj\n" % num + body(num, objects[num]) + b"\nendobj\n"
    size = max(list(objects) + packed) + 1
    if object_stream:  # a cross-reference stream (never encrypted)
        xref_num = size
        size += 1
        offsets[xref_num] = len(out)
        rows = b""
        for num in range(size):
            if num in offsets:
                rows += struct.pack(">BIH", 1, offsets[num], 0)
            elif num in packed:
                rows += struct.pack(">BIH", 2, 9, packed.index(num))
            else:
                rows += struct.pack(">BIH", 0, 0, 65535)
        trailer.update({"Type": Name("XRef"), "Size": size, "W": [1, 4, 2], "Length": len(rows)})
        out += b"%d 0 obj\n" % xref_num + serialize(trailer, None) + b"\nstream\n" + rows + b"\nendstream\nendobj\n"
        out += b"startxref\n%d\n%%%%EOF\n" % offsets[xref_num]
    else:
        start = len(out)
        out += b"xref\n0 %d\n0000000000 65535 f \n" % size
        for num in range(1, size):
            out += (b"%010d 00000 n \n" % offsets[num]) if num in offsets else b"0000000000 65535 f \n"
        trailer["Size"] = size
        out += b"trailer\n" + serialize(trailer, None) + b"\nstartxref\n%d\n%%%%EOF\n" % start
    with open(os.path.join(HERE, name), "wb") as f:
        f.write(bytes(out))


def main():
    rng = random.Random(2026)
    write("rc4-40.pdf", Handler(rng, 1, 2, b"", b"owner", "RC4", key_bytes=5), page_objects())
    write("rc4-128.pdf", Handler(rng, 2, 3, "café".encode("latin-1"), b"owner", "RC4"), page_objects())
    write("aes-128.pdf", Handler(rng, 4, 4, b"user", b"owner", "AESV2", encrypt_metadata=False), page_objects(),
          object_stream=True, metadata=True)
    write("aes-256.pdf", Handler(rng, 5, 6, b"", b"owner", "AESV3", key_bytes=32), page_objects(extra_contents=True))
    write("aes-256-user.pdf", Handler(rng, 5, 6, "pässwörd".encode(), b"owner", "AESV3", key_bytes=32), page_objects())
    write("aes-256-r5.pdf", Handler(rng, 5, 5, b"user", b"owner", "AESV3", key_bytes=32), page_objects())


if __name__ == "__main__":
    main()
