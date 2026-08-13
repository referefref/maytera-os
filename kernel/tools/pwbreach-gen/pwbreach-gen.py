#!/usr/bin/env python3
# pwbreach-gen.py - build kernel/rustkern/pwbreach.bin, the breached-password
# membership table the kernel password policy checks against.
#
# WHY A GENERATED TABLE AND NOT THE LIST
# --------------------------------------
# The source list is 50,000 plaintext passwords (403 KB). Shipping that inside
# the kernel image would be 403 KB of .rodata AND a plaintext dictionary sitting
# in a bootable artifact that also gets published. What the kernel actually
# needs is a MEMBERSHIP TEST, not the strings, so this converts the list to a
# sorted array of 32-bit truncated hashes that rustkern/pwpolicy.rs binary
# searches.
#
# WHY ONLY ENTRIES >= MIN_LEN BYTES
# ---------------------------------
# The policy checks length BEFORE membership, so any list entry shorter than
# PW_MIN_LEN is already rejected by the length rule and can never reach the
# table lookup. Filtering them out takes the table from 50,000 entries to
# ~16,620 without weakening the policy by one password: the two rules together
# still reject all 50,000. The filter length is written into the file header and
# rustkern/pwpolicy.rs asserts at boot that it equals its own PW_MIN_LEN, so
# lowering the minimum without regenerating this file goes LOUD instead of
# silently un-covering 33,382 passwords.
#
# WHY FNV-1a AND NOT SHA-256
# --------------------------
# The only property required is that two DIFFERENT strings rarely land on the
# same 32-bit value by accident. Collision resistance against an ADVERSARY buys
# nothing here, because the failure a collision causes is a FALSE POSITIVE: a
# password that is not on the list gets rejected and the user picks another one.
# An attacker cannot use a collision to get a breached password ACCEPTED, since
# every list entry's own hash is present by construction, so false negatives are
# structurally impossible. FNV-1a is four lines in both Python and no_std Rust
# with no dependency on the kernel's SHA-256 plumbing.
#
# CASE. Each entry contributes its own hash and, if different, the hash of its
# ASCII-lowercased form. The policy looks up both the candidate and its
# lowercased form, so "PASSWORD1" and "Password1" are caught by the lowercase
# entry for "password1".
#
# SOURCE
#   https://raw.githubusercontent.com/Tok3n-git/Wordlists/refs/heads/main/rockyou-top50k.txt
#   md5 fd6fb8f7b78253fd1200f023e47236d1, 50000 lines, 403881 bytes.
#
# USAGE
#   curl -sSo rockyou-top50k.txt <url>
#   python3 pwbreach-gen.py rockyou-top50k.txt ../../rustkern/pwbreach.bin
#
# FILE FORMAT (little endian throughout)
#   0  u32  magic 'MPWB' (0x4257504D as a LE u32)
#   4  u32  format version (1)
#   8  u32  min_len the source list was filtered at
#  12  u32  entry count N
#  16  N*u32 truncated hashes, sorted ascending, deduplicated

import sys

MIN_LEN = 8          # must equal PW_MIN_LEN in rustkern/pwpolicy.rs
MAGIC = b"MPWB"
VERSION = 1


def fnv1a32(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h & 0xFFFFFFFF


def ascii_lower(b: bytes) -> bytes:
    return bytes(c + 32 if 0x41 <= c <= 0x5A else c for c in b)


def build(words):
    table = set()
    used = 0
    for w in words:
        # BYTE length, not character count. The kernel hashes BYTES and applies
        # its length rule to BYTES, so a 7-character word that is 8 bytes of
        # UTF-8 ("pequena" with a tilde) passes the kernel length check and MUST
        # be in the table. Filtering on characters left exactly two such words
        # accepted by the policy; the host harness caught it.
        if len(w) < MIN_LEN:
            continue
        used += 1
        table.add(fnv1a32(w))
        lw = ascii_lower(w)
        if lw != w:
            table.add(fnv1a32(lw))
    return sorted(table), used


def read_words(path):
    """Read the list as BYTES. Decoding it would silently rewrite any non-UTF-8
    line, and those lines are exactly the ones that got missed the first time."""
    with open(path, "rb") as f:
        raw = f.read()
    return [line.rstrip(b"\r") for line in raw.split(b"\n") if line != b""]


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: pwbreach-gen.py <wordlist.txt> <out.bin>\n")
        return 2
    words = read_words(sys.argv[1])
    table, used = build(words)
    if len(table) < 10000:
        sys.stderr.write("REFUSING: only %d entries; that is not the expected list\n" % len(table))
        return 1
    out = bytearray()
    out += MAGIC
    out += VERSION.to_bytes(4, "little")
    out += MIN_LEN.to_bytes(4, "little")
    out += len(table).to_bytes(4, "little")
    for h in table:
        out += h.to_bytes(4, "little")
    with open(sys.argv[2], "wb") as f:
        f.write(bytes(out))
    sys.stderr.write(
        "pwbreach: %d source words, %d >= %d bytes, %d table entries, %d bytes\n"
        % (len(words), used, MIN_LEN, len(table), len(out)))
    # Self-check: every source word >= MIN_LEN bytes must be found by the same
    # binary search the kernel does. This is the false-negative assertion and it
    # runs every time the table is generated.
    import bisect
    misses = 0
    for w in words:
        if len(w) < MIN_LEN:
            continue
        h = fnv1a32(w)
        i = bisect.bisect_left(table, h)
        if not (i < len(table) and table[i] == h):
            misses += 1
    if misses:
        sys.stderr.write("REFUSING: %d source words are NOT in the table\n" % misses)
        return 1
    sys.stderr.write("pwbreach: verified 0 false negatives over the source list\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
