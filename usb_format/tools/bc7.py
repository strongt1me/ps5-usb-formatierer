"""Einfacher BC7-Kodierer (nur Modus 6: ein Farbpaar je 4x4-Block, 4-Bit-Indizes, deckend) und DDS-Schreiber
im Format, das der PS5-Installer für den Kachel-Hintergrund ablegt (pic0.dds: BC7_UNORM, DX10-Kopf, 1 Stufe).
Benötigt numpy und Pillow."""
import struct

import numpy as np

WEIGHTS = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], np.int32)


def _blocks(rgb):
    h, w, _ = rgb.shape
    b = rgb.reshape(h // 4, 4, w // 4, 4, 3).transpose(0, 2, 1, 3, 4)
    return b.reshape(-1, 16, 3).astype(np.float64)


def _endpoints(px):
    """Endpunkte entlang der Hauptachse der Blockfarben, auf 7 Bit + P-Bit 1 (=ungerade Werte) gerundet."""
    mean = px.mean(axis=1, keepdims=True)
    d = px - mean
    cov = np.einsum("nki,nkj->nij", d, d)
    axis = np.ones((px.shape[0], 3)) / np.sqrt(3)
    for _ in range(8):  # Potenzmethode
        axis = np.einsum("nij,nj->ni", cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-9)
    t = np.einsum("nki,ni->nk", d, axis)
    e0 = mean[:, 0] + t.min(axis=1, keepdims=True) * axis
    e1 = mean[:, 0] + t.max(axis=1, keepdims=True) * axis
    q = lambda e: np.clip(np.round((np.clip(e, 0, 255) - 1) / 2), 0, 127).astype(np.int32)
    return q(e0), q(e1)


def _indices(px, c0, c1):
    v0, v1 = (c0 * 2 + 1)[:, None, :], (c1 * 2 + 1)[:, None, :]
    pal = ((64 - WEIGHTS)[None, :, None] * v0 + WEIGHTS[None, :, None] * v1 + 32) >> 6  # (n,16,3)
    dist = ((px[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)                     # (n,16 Pixel,16)
    return dist.argmin(-1).astype(np.int32)


def _put(bits, pos, val, n):
    for k in range(n):
        bits[:, pos + k] = (val >> k) & 1


def encode(rgb):
    """rgb: uint8-Array (H, W, 3), H und W Vielfache von 4 -> BC7-Blöcke (bytes), zeilenweise"""
    px = _blocks(rgb)
    out = np.zeros((px.shape[0], 16), np.uint8)
    for s in range(0, px.shape[0], 4096):
        p = px[s:s + 4096]
        c0, c1 = _endpoints(p)
        idx = _indices(p, c0, c1)
        # Anker (Pixel 0) hat nur 3 Bit: liegt sein Index >= 8, Endpunkte tauschen und Indizes spiegeln
        swap = idx[:, 0] >= 8
        c0[swap], c1[swap] = c1[swap].copy(), c0[swap].copy()
        idx[swap] = 15 - idx[swap]
        n = p.shape[0]
        bits = np.zeros((n, 128), np.uint8)
        bits[:, 6] = 1                                   # Modus 6
        pos = 7
        for ch in range(3):                              # R0 R1 G0 G1 B0 B1, je 7 Bit
            _put(bits, pos, c0[:, ch], 7)
            _put(bits, pos + 7, c1[:, ch], 7)
            pos += 14
        _put(bits, pos, np.full(n, 127), 7)              # A0, A1 = 127 (mit P-Bit 1 -> 255)
        _put(bits, pos + 7, np.full(n, 127), 7)
        pos += 14
        bits[:, pos] = 1                                 # P0
        bits[:, pos + 1] = 1                             # P1
        pos += 2
        _put(bits, pos, idx[:, 0], 3)
        pos += 3
        for k in range(1, 16):
            _put(bits, pos, idx[:, k], 4)
            pos += 4
        out[s:s + n] = np.packbits(bits, axis=1, bitorder="little")
    return out.tobytes()


def dds_header(width, height):
    """DDS-Kopf wie die vom PS5-Installer erzeugte pic0.dds (BC7_UNORM, DX10, eine Stufe)"""
    linear = (width // 4) * (height // 4) * 16
    head = struct.pack("<4s7I", b"DDS ", 124, 0xA1007, height, width, linear, 0, 1)
    head += b"\0" * 44                                         # reserved1
    head += struct.pack("<II4s5I", 32, 4, b"DX10", 0, 0, 0, 0, 0)
    head += struct.pack("<5I", 0x1000, 0, 0, 0, 0)            # caps (TEXTURE), reserved2
    head += struct.pack("<5I", 98, 3, 0, 1, 0)                 # DXGI_FORMAT_BC7_UNORM, 2D, Array 1
    return head


def save_dds(img, path):
    rgb = np.asarray(img.convert("RGB"), np.uint8)
    with open(path, "wb") as f:
        f.write(dds_header(img.width, img.height))
        f.write(encode(rgb))
