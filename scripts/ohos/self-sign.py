#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
self-sign.py — OpenHarmony ELF self-signing implementation (Python 3, hashlib only)

Vendored from https://github.com/hqzing/ohos-selfsign (0BSD License).
Signs cross-compiled OHOS aarch64 ELF/.so/.node files on cloud x86 CI runners,
without the device-only binary-sign-tool / ohos-signpost.

Usage:
    python3 self-sign.py <input_elf> [output_elf]
        If output is omitted, input is rewritten in place.
"""
import hashlib
import struct
import sys

DESC_SIZE = 256
PAGE_SIZE = 4096
FLAG_SELF_SIGN = 0x10
FS_VERITY_DESCRIPTOR_TYPE = 1
HASH_OUT = 32  # SHA-256 digest size in bytes


def sha256(b: bytes) -> bytes:
    return hashlib.sha256(b).digest()


# ─────────────────────── ELF64 section injector ─────────────────────
def inject_codesign_section(raw: bytes) -> tuple[bytes, int]:
    """Append a .codesign section to ELF64, aligned to 4 KiB, with a zeroed
    4 KiB placeholder, then update the section header table and shstrtab.
    Section-level output is equivalent to upstream binary-sign-tool.
    Return (ELF containing the section, section file offset)."""
    # ELF64 header field offsets
    E_SHOFF, E_SHNUM, E_SHSTRNDX = 0x28, 0x3c, 0x3e
    if len(raw) < 64 or raw[:4] != b"\x7fELF" or raw[4] != 2:
        raise ValueError("not ELF64")
    e_shoff = struct.unpack_from("<Q", raw, E_SHOFF)[0]
    e_shnum = struct.unpack_from("<H", raw, E_SHNUM)[0]
    e_shstrndx = struct.unpack_from("<H", raw, E_SHSTRNDX)[0]
    if e_shoff == 0 or e_shnum == 0 or e_shstrndx >= e_shnum:
        raise ValueError("no section header table (stripped ELF unsupported)")

    # shstrtab entry
    shstr_e = raw[e_shoff + e_shstrndx * 64 : e_shoff + (e_shstrndx + 1) * 64]
    shstr_off = struct.unpack_from("<Q", shstr_e, 24)[0]
    shstr_sz = struct.unpack_from("<Q", shstr_e, 32)[0]

    # Place .codesign after all sections and the section header table, aligned to 4 KiB.
    cur_end = e_shoff + e_shnum * 64
    for i in range(e_shnum):
        e = raw[e_shoff + i * 64 : e_shoff + (i + 1) * 64]
        off = struct.unpack_from("<Q", e, 24)[0]
        sz = struct.unpack_from("<Q", e, 32)[0]
        if struct.unpack_from("<I", e, 4)[0] == 8:  # SHT_NOBITS (.bss) occupies no file space
            sz = 0
        if off + sz > cur_end:
            cur_end = off + sz
    cs_off = (cur_end + PAGE_SIZE - 1) // PAGE_SIZE * PAGE_SIZE

    # New shstrtab = old shstrtab + ".codesign\0".
    CS_NAME = b".codesign"
    new_shstr = raw[shstr_off : shstr_off + shstr_sz] + CS_NAME + b"\x00"
    cs_shname = shstr_sz  # .codesign name offset in the new shstrtab
    new_shstr_sz = len(new_shstr)

    # Place the new shstrtab after the section; align the new SHT to 8 bytes.
    new_shstr_off = cs_off + PAGE_SIZE
    new_sht_off = (new_shstr_off + new_shstr_sz + 7) // 8 * 8
    new_shnum = e_shnum + 1
    new_total = new_sht_off + new_shnum * 64

    buf = bytearray(new_total)
    # 1) Original contents.
    buf[0:len(raw)] = raw
    # 2) .codesign section contents (4 KiB of zeros, already initialized).
    # 3) New shstrtab.
    buf[new_shstr_off : new_shstr_off + new_shstr_sz] = new_shstr
    # 4) Copy the old SHT to its new location.
    buf[new_sht_off : new_sht_off + e_shnum * 64] = raw[e_shoff : e_shoff + e_shnum * 64]
    # 5) Append the .codesign entry (64 bytes).
    cs_e = struct.pack(
        "<IIQQQQIIQQ",
        cs_shname,  # sh_name
        1,  # sh_type SHT_PROGBITS
        0,  # sh_flags
        0,  # sh_addr
        cs_off,  # sh_offset
        PAGE_SIZE,  # sh_size
        0, 0,  # sh_link, sh_info
        PAGE_SIZE,  # sh_addralign
        0,  # sh_entsize
    )
    buf[new_sht_off + e_shnum * 64 : new_sht_off + new_shnum * 64] = cs_e
    # 6) Update the shstrtab entry with its new offset and size.
    shstr_e_new = bytearray(
        buf[new_sht_off + e_shstrndx * 64 : new_sht_off + (e_shstrndx + 1) * 64]
    )
    shstr_e_new[24:32] = struct.pack("<Q", new_shstr_off)
    shstr_e_new[32:40] = struct.pack("<Q", new_shstr_sz)
    buf[new_sht_off + e_shstrndx * 64 : new_sht_off + (e_shstrndx + 1) * 64] = shstr_e_new
    # 7) Update the ELF header: e_shoff / e_shnum.
    struct.pack_into("<Q", buf, E_SHOFF, new_sht_off)
    struct.pack_into("<H", buf, E_SHNUM, new_shnum)
    # e_shstrndx is unchanged.

    return bytes(buf), cs_off


# ─────────────────────── Merkle tree root hash ──────────────────────
def merkle_root_hash(data: bytes, cs_off: int, cs_len: int) -> bytes:
    """Match upstream merkle_tree_builder.cpp::RunHashTask:
    zero the leaf hash for the section page, hash other pages with SHA-256,
    then calculate parent levels as usual."""
    if len(data) == 0:
        return sha256(bytes(PAGE_SIZE))

    npages = (len(data) + PAGE_SIZE - 1) // PAGE_SIZE
    cs_page_begin = cs_off // PAGE_SIZE
    cs_page_end = (cs_off + cs_len + PAGE_SIZE - 1) // PAGE_SIZE

    hashes = bytearray()
    for i in range(npages):
        if cs_len > 0 and cs_page_begin <= i < cs_page_end:
            hashes += bytes(HASH_OUT)  # Section page: zero leaf hash.
            continue
        page = data[i * PAGE_SIZE : (i + 1) * PAGE_SIZE]
        if len(page) < PAGE_SIZE:
            page = page + bytes(PAGE_SIZE - len(page))  # Zero-pad the final page.
        hashes += sha256(page)

    if npages == 1:
        return hashes[:HASH_OUT]

    cur = bytes(hashes)
    while True:
        if len(cur) <= PAGE_SIZE:
            page = cur + bytes(PAGE_SIZE - len(cur))
            return sha256(page)
        nxt = bytearray()
        for i in range(0, len(cur), PAGE_SIZE):
            page = cur[i : i + PAGE_SIZE]
            if len(page) < PAGE_SIZE:
                page = page + bytes(PAGE_SIZE - len(page))
            nxt += sha256(page)
        cur = bytes(nxt)


# ─────────────────────── Descriptor and ElfSignInfo ──────────────────
def build_descriptor(sign_size: int, file_size: int, root: bytes,
                     flags: int) -> bytes:
    """Build the 256-byte descriptor layout, entirely little-endian (doc §4)."""
    d = bytearray(DESC_SIZE)
    d[0] = 1            # version
    d[1] = 1            # hashAlgorithm = SHA-256
    d[2] = 12           # log2BlockSize = 2^12 = 4096
    d[3] = 0            # saltSize
    d[4:8] = sign_size.to_bytes(4, "little")
    d[8:16] = file_size.to_bytes(8, "little")
    d[16:16 + 32] = root            # rootHash is left-aligned; remaining 32 bytes stay zero.
    # d[80:112] salt is all zeros.
    d[112:116] = flags.to_bytes(4, "little")
    # d[116:120] reserved1=0
    # d[120:128] merkleTreeOffset=0
    # d[128:255] reserved2=0
    d[255] = 3          # csVersion
    return bytes(d)


# ─────────────────────────── Main flow ───────────────────────────────
def self_sign(in_path: str, out_path: str) -> None:
    with open(in_path, "rb") as f:
        raw = f.read()

    # 0. Reject files that already contain .codesign (this tool only adds a
    #    signature; it does not strip old sections). Repeated signing can
    #    accumulate malformed sections and cause verification to fail.
    #    To re-sign, first run llvm-objcopy --remove-section .codesign <elf>.
    e_shoff = struct.unpack_from("<Q", raw, 0x28)[0]
    e_shnum = struct.unpack_from("<H", raw, 0x3c)[0]
    e_shstrndx = struct.unpack_from("<H", raw, 0x3e)[0]
    if e_shoff != 0 and e_shnum != 0 and e_shstrndx < e_shnum:
        shstr_e = raw[e_shoff + e_shstrndx * 64 : e_shoff + (e_shstrndx + 1) * 64]
        shstr_off = struct.unpack_from("<Q", shstr_e, 24)[0]
        shstr_sz = struct.unpack_from("<Q", shstr_e, 32)[0]
        if shstr_off + shstr_sz <= len(raw):
            for i in range(e_shnum):
                e = raw[e_shoff + i * 64 : e_shoff + (i + 1) * 64]
                name_off = struct.unpack_from("<I", e, 0)[0]
                if name_off < shstr_sz:
                    name = raw[shstr_off + name_off : shstr_off + shstr_sz].split(b"\x00")[0]
                    if name == b".codesign":
                        raise ValueError(
                            f"{in_path} already has a .codesign section.\n"
            "  This tool only adds a signature, it does not strip old ones.\n"
            "  To re-sign, first strip the old section with:\n"
            "    llvm-objcopy --remove-section .codesign {in_path}\n"
            "  then run self-sign again."
                        )

    # 1. Inject a 4 KiB placeholder .codesign section into tmp.
    tmp, cs_off = inject_codesign_section(raw)
    file_size = len(tmp)

    # 2. Calculate the Merkle root, zeroing the leaf hash for the section page.
    root = merkle_root_hash(tmp, cs_off, PAGE_SIZE)

    # 3/4. Build a descriptor with signSize=0 for the digest.
    desc_for_digest = build_descriptor(0, file_size, root, FLAG_SELF_SIGN)
    # 5. signature = SHA256(descriptor)
    signature = sha256(desc_for_digest)
    assert len(signature) == HASH_OUT
    # 6. Build a descriptor with signSize=32 for output.
    desc_on_disk = build_descriptor(32, file_size, root, FLAG_SELF_SIGN)

    # 7. Assemble ElfSignInfo: 8-byte header + 256-byte descriptor + 32-byte signature = 296 bytes.
    payload = bytearray()
    payload += FS_VERITY_DESCRIPTOR_TYPE.to_bytes(4, "little")  # type
    payload += (DESC_SIZE + HASH_OUT).to_bytes(4, "little")  # length = 288
    payload += desc_on_disk
    payload += signature

    # 8. Write the payload into the section in place (the section starts at cs_off).
    tmp = bytearray(tmp)
    tmp[cs_off : cs_off + len(payload)] = payload

    # 9. Write the output file.
    with open(out_path, "wb") as f:
        f.write(tmp)

    print(f"self-sign ok: {in_path} → {out_path} (tmp={file_size}, cs_off=0x{cs_off:x}, payload={len(payload)})")


def main() -> int:
    if len(sys.argv) < 2 or len(sys.argv) > 3:
        sys.stderr.write(
            f"usage: {sys.argv[0]} <input_elf> [output_elf]\n"
            "  (output defaults to input, in-place)\n")
        return 1
    in_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) == 3 else in_path
    try:
        self_sign(in_path, out_path)
    except (OSError, ValueError) as e:
        sys.stderr.write(f"error: {e}\n")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
