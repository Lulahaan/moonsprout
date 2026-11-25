"""disc.py - read files from your own Pikmin 2 disc image (tools/mods, M5.2).

Standard library only. A plain GameCube ISO/GCM image is read directly (FST, Yaz0 and RARC
readers below). Any other image (.rvz, .ciso, ...) goes through `dtk vfs cp`: $MPPM_DTK, then
build/tools/dtk in the repo, then dtk on PATH.

Every reader treats its input as untrusted: offsets and counts are checked against the data, and
a problem raises DiscError, never an IndexError deep inside.

Paths are disc paths below files/ (for example "user/Abe/vs/stages.txt"); archive inner paths are
relative to the RARC root folder. Both are compared case-insensitively.
"""

import os
import shutil
import struct
import subprocess
import tempfile

GC_MAGIC = 0xC2339F3D  # at 0x1C of a GameCube disc header
MAX_FILE = 64 << 20  # largest file this tool reads (the mod limit)
MAX_YAZ0_OUT = 64 << 20


class DiscError(Exception):
    pass


def _be32(b, off):
    if off < 0 or off + 4 > len(b):
        raise DiscError("read past the end at 0x%x" % off)
    return struct.unpack_from(">I", b, off)[0]


def _be16(b, off):
    if off < 0 or off + 2 > len(b):
        raise DiscError("read past the end at 0x%x" % off)
    return struct.unpack_from(">H", b, off)[0]


def _cstr(b, off, limit):
    if off < 0 or off >= limit:
        raise DiscError("string offset 0x%x out of range" % off)
    end = b.find(b"\0", off, limit)
    if end < 0:
        raise DiscError("unterminated string at 0x%x" % off)
    return b[off:end].decode("latin-1")


# --------------------------------------------------------------------------------------------
# Yaz0
# --------------------------------------------------------------------------------------------

def yaz0_decode(src):
    """Decode a Yaz0 block. Bounded: output size from the header, at most 64 MiB."""
    if len(src) < 16 or src[:4] != b"Yaz0":
        raise DiscError("not Yaz0 data")
    size = _be32(src, 4)
    if size > MAX_YAZ0_OUT:
        raise DiscError("Yaz0 output too large (%d bytes)" % size)
    out = bytearray(size)
    sp, dp, n = 16, 0, len(src)
    while dp < size:
        if sp >= n:
            raise DiscError("Yaz0 source overrun")
        code = src[sp]
        sp += 1
        for bit in range(8):
            if dp >= size:
                break
            if code & (0x80 >> bit):
                if sp >= n:
                    raise DiscError("Yaz0 source overrun")
                out[dp] = src[sp]
                dp += 1
                sp += 1
            else:
                if sp + 2 > n:
                    raise DiscError("Yaz0 source overrun")
                b1, b2 = src[sp], src[sp + 1]
                sp += 2
                dist = ((b1 & 0x0F) << 8 | b2) + 1
                if b1 >> 4 == 0:
                    if sp >= n:
                        raise DiscError("Yaz0 source overrun")
                    length = src[sp] + 0x12
                    sp += 1
                else:
                    length = (b1 >> 4) + 2
                cp = dp - dist
                if cp < 0:
                    raise DiscError("Yaz0 back reference before the start")
                for _ in range(length):
                    if dp >= size:
                        break
                    out[dp] = out[cp]
                    dp += 1
                    cp += 1
    return bytes(out)


def maybe_yaz0(data):
    return yaz0_decode(data) if data[:4] == b"Yaz0" else data


# --------------------------------------------------------------------------------------------
# RARC
# --------------------------------------------------------------------------------------------

def rarc_files(data):
    """Map lower(inner path) -> (inner path as stored, bytes) for every file of a RARC archive.
    Inner paths are relative to the root folder (the root's own name is not part of them)."""
    n = len(data)
    if n < 0x40 or data[:4] != b"RARC":
        raise DiscError("not a RARC archive")
    hdr_len = _be32(data, 0x08)
    data_off = _be32(data, 0x0C)
    if hdr_len != 0x20:
        raise DiscError("unexpected RARC header length 0x%x" % hdr_len)
    info = 0x20
    num_nodes = _be32(data, info + 0x00)
    node_off = _be32(data, info + 0x04) + info
    num_entries = _be32(data, info + 0x08)
    entry_off = _be32(data, info + 0x0C) + info
    str_len = _be32(data, info + 0x10)
    str_off = _be32(data, info + 0x14) + info
    data_base = info + data_off
    if num_nodes == 0 or num_nodes > 65535 or num_entries > (1 << 20):
        raise DiscError("RARC table counts out of range")
    if node_off + num_nodes * 16 > n or entry_off + num_entries * 20 > n:
        raise DiscError("RARC tables past the end")
    if str_off + str_len > n:
        raise DiscError("RARC string table past the end")
    str_end = str_off + str_len
    out = {}
    visited = set()

    def walk(node, prefix, depth):
        if depth > 32:
            raise DiscError("RARC folders nested too deeply")
        if node in visited:
            raise DiscError("RARC folder visited twice")
        visited.add(node)
        base = node_off + node * 16
        count = _be16(data, base + 10)
        first = _be32(data, base + 12)
        if first + count > num_entries:
            raise DiscError("RARC folder entries out of range")
        for i in range(first, first + count):
            e = entry_off + i * 20
            word = _be32(data, e + 4)
            flags = word >> 24
            name = _cstr(data, str_off + (word & 0xFFFFFF), str_end)
            off = _be32(data, e + 8)
            size = _be32(data, e + 12)
            if name in (".", ".."):
                continue
            path = prefix + name
            if flags & 0x02:
                if off >= num_nodes:
                    raise DiscError("RARC folder index out of range")
                walk(off, path + "/", depth + 1)
            else:
                start = data_base + off
                if start + size > n or size > MAX_FILE:
                    raise DiscError("RARC file %s past the end" % path)
                out[path.lower()] = (path, data[start:start + size])

    walk(0, "", 0)
    return out


# --------------------------------------------------------------------------------------------
# Disc access
# --------------------------------------------------------------------------------------------

def find_dtk(repo_root):
    env = os.environ.get("MPPM_DTK")
    if env:
        return env
    cand = os.path.join(repo_root, "build", "tools", "dtk")
    if os.path.isfile(cand) and os.access(cand, os.X_OK):
        return cand
    return shutil.which("dtk")


class Disc(object):
    """Files of a disc image. use_dtk forces the dtk path (a test of the fallback)."""

    def __init__(self, path, repo_root, use_dtk=False):
        self.path = path
        self.repo_root = repo_root
        self.dtk = None
        self.fst = None  # lower(path) -> (path as stored, offset, size)
        with open(path, "rb") as f:
            head = f.read(0x440)
        plain = len(head) >= 0x440 and struct.unpack_from(">I", head, 0x1C)[0] == GC_MAGIC
        if plain and not use_dtk:
            self.game_id = head[:6].decode("latin-1")
            self._read_fst()
        else:
            self.dtk = find_dtk(repo_root)
            if not self.dtk:
                raise DiscError("this is not a plain GameCube ISO and dtk was not found "
                                "(set MPPM_DTK, or put dtk in build/tools or on PATH)")
            boot = self._dtk_cp("sys/boot.bin")
            self.game_id = boot[:6].decode("latin-1")

    def _read_fst(self):
        with open(self.path, "rb") as f:
            f.seek(0x424)
            fst_off, fst_size = struct.unpack(">II", f.read(8))
            if fst_size < 12 or fst_size > (16 << 20):
                raise DiscError("FST size out of range")
            f.seek(fst_off)
            fst = f.read(fst_size)
        if len(fst) != fst_size:
            raise DiscError("FST past the end of the image")
        count = _be32(fst, 8)
        if count == 0 or count * 12 > fst_size:
            raise DiscError("FST entry count out of range")
        strings = count * 12
        table = {}
        # stack of (end index, prefix) for open folders
        stack = [(count, "")]
        i = 1
        while i < count:
            while stack and i >= stack[-1][0]:
                stack.pop()
            if not stack:
                raise DiscError("FST folder structure broken")
            prefix = stack[-1][1]
            w0 = _be32(fst, i * 12)
            a = _be32(fst, i * 12 + 4)
            b = _be32(fst, i * 12 + 8)
            name = _cstr(fst, strings + (w0 & 0xFFFFFF), fst_size)
            if w0 >> 24:
                if b <= i or b > count:
                    raise DiscError("FST folder end out of range")
                stack.append((b, prefix + name + "/"))
            else:
                p = prefix + name
                table[p.lower()] = (p, a, b)
            i += 1
        self.fst = table

    def _dtk_cp(self, path):
        tmp = tempfile.mkdtemp(prefix="mppm-dtk-")
        try:
            dest = os.path.join(tmp, "out.bin")
            r = subprocess.run([self.dtk, "vfs", "cp", "%s:%s" % (self.path, path), dest],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300)
            if r.returncode != 0 or not os.path.isfile(dest):
                raise DiscError("dtk vfs cp %s failed: %s" % (path, r.stderr.decode("utf-8", "replace").strip()))
            if os.path.getsize(dest) > MAX_FILE:
                raise DiscError("%s is too large" % path)
            with open(dest, "rb") as f:
                return f.read()
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def real_path(self, path):
        """The disc's own spelling of a path below files/ (plain ISO only; else as given)."""
        if self.fst is None:
            return path
        e = self.fst.get(path.lower())
        if e is None:
            raise DiscError("files/%s is not on the disc" % path)
        return e[0]

    def read(self, path):
        """Bytes of files/<path> as stored on the disc (still Yaz0 if it is)."""
        if self.fst is None:
            return self._dtk_cp("files/" + path)
        e = self.fst.get(path.lower())
        if e is None:
            raise DiscError("files/%s is not on the disc" % path)
        _, off, size = e
        if size > MAX_FILE:
            raise DiscError("files/%s is too large" % path)
        with open(self.path, "rb") as f:
            f.seek(off)
            data = f.read(size)
        if len(data) != size:
            raise DiscError("files/%s past the end of the image" % path)
        return data

    def read_inner(self, archive, inner):
        """(inner path as stored, bytes) of one file inside a disc archive."""
        if self.fst is None:
            return inner, self._dtk_cp("files/%s:%s" % (archive, inner))
        files = rarc_files(maybe_yaz0(self.read(archive)))
        e = files.get(inner.lower())
        if e is None:
            raise DiscError("files/%s has no file %s" % (archive, inner))
        return e
