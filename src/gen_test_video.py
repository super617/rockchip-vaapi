#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""
gen_test_video.py — genera vídeo de test determinístico para rockchip-vaapi.

Características:
  - Barcode en los primeros BARCODE_W píxeles de cada frame (banda izquierda)
    Codifica el número de frame mostrado (display order) en binario:
      3 bandas de sincronización (blanco/negro/blanco) + 10 bits de datos
      Cada banda = BAND_H filas, Y=235 (blanco) ó Y=16 (negro)
  - 5 sprites de colores con movimiento senoidal
  - Fondo degradado diagonal en scroll
  - 25 s + de vídeo a 30 fps

Uso:
  python3 gen_test_video.py [NFRAMES]  > /tmp/test_src.y4m
  python3 gen_test_video.py            > /tmp/test_src.y4m   (usa 900 frames)

El stream es YUV4MPEG2 (y4m) en stdout.  Pásalo a ffmpeg para codificar.

Constantes que deben coincidir con va_barcode_test.c:
  BARCODE_W = 64   píxeles de ancho de la banda de barcode
  N_BANDS   = 13   bandas totales (3 sync + 10 datos)
"""

import sys
import math
import numpy as np

# ── Parámetros ──────────────────────────────────────────────────────────────

W, H   = 640, 480
FPS    = 30
NFRAMES = int(sys.argv[1]) if len(sys.argv) > 1 else 900   # 30 s

BARCODE_W = 64      # ancho de la banda de barcode (debe coincidir con el .c)
N_BANDS   = 13      # 3 sync + 10 data bits
BAND_H    = H // N_BANDS   # altura de cada banda en filas (= 36 px para 480)

# ── Sprites ──────────────────────────────────────────────────────────────────
# Cada entrada: (periodo_x, periodo_y, fase_x, fase_y, amp_x, amp_y,
#                centro_x, centro_y, Y, U, V, ancho, alto)
SPRITES = [
    # Rojo rápido
    (47, 61,  0,  0, 220, 160, 380, 240,  81,  90, 240, 70, 70),
    # Verde lento
    (83, 97, 30, 50, 180, 130, 350, 220, 145,  54, 34,  80, 60),
    # Azul diagonal
    (53, 71, 60, 10, 200, 180, 420, 260,  41, 240, 110, 65, 65),
    # Amarillo pequeño rápido
    (29, 43, 15, 80, 250, 170, 300, 200, 210, 146,  16, 50, 50),
    # Cian grande lento
    (113, 89, 45, 35, 170, 120, 500, 300, 170, 166,  10, 90, 75),
]

# ── Generador de frames ───────────────────────────────────────────────────────

def make_frame(n: int):
    """Devuelve (Y[H,W], U[H/2,W/2], V[H/2,W/2]) para el frame n."""

    # Fondo: gradiente diagonal en scroll
    scroll = (n * 3) % (W + H)
    xs = np.arange(W, dtype=np.int32)
    ys = np.arange(H, dtype=np.int32)
    xx, yy = np.meshgrid(xs, ys)
    Y = ((xx + yy + scroll) % 96 + 80).astype(np.uint8)
    U = np.full((H // 2, W // 2), 128, dtype=np.uint8)
    V = np.full((H // 2, W // 2), 128, dtype=np.uint8)

    # Sprites
    for (px, py, phx, phy, ax, ay, cx, cy, yv, uv, vv, sw, sh) in SPRITES:
        x = int(cx + ax * math.sin(2 * math.pi * (n + phx) / px))
        y = int(cy + ay * math.sin(2 * math.pi * (n + phy) / py))
        x1, x2 = max(BARCODE_W, x - sw // 2), min(W, x + sw // 2)
        y1, y2 = max(0, y - sh // 2),          min(H, y + sh // 2)
        if x2 > x1 and y2 > y1:
            Y [y1:y2,     x1:x2    ] = yv
            U [y1//2:y2//2, x1//2:x2//2] = uv
            V [y1//2:y2//2, x1//2:x2//2] = vv

    # Barcode (escribe sobre la banda izquierda por encima del fondo)
    bits = [1, 0, 1]                          # sync: blanco/negro/blanco
    for i in range(9, -1, -1):                # 10 bits MSB primero
        bits.append((n >> i) & 1)
    for b, bit in enumerate(bits):
        luma = 235 if bit else 16
        r0 = b * BAND_H
        r1 = H if b == N_BANDS - 1 else (b + 1) * BAND_H
        Y [r0:r1,     :BARCODE_W     ] = luma
        U [r0//2:r1//2, :BARCODE_W//2] = 128
        V [r0//2:r1//2, :BARCODE_W//2] = 128

    return Y, U, V

# ── Escritura y4m ────────────────────────────────────────────────────────────

out = sys.stdout.buffer
out.write(f"YUV4MPEG2 W{W} H{H} F{FPS}:1 Ip A1:1 C420mpeg2\n".encode())

for n in range(NFRAMES):
    Y, U, V = make_frame(n)
    out.write(b"FRAME\n")
    out.write(Y.tobytes())
    out.write(U.tobytes())
    out.write(V.tobytes())
    if n % 150 == 0:
        print(f"  frame {n}/{NFRAMES}", file=sys.stderr)

print(f"gen_test_video: {NFRAMES} frames ({NFRAMES/FPS:.1f} s) done", file=sys.stderr)
