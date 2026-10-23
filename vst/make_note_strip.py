# Draws vst/images/note_strip.png. Run in the mpc-vst-html-art image (it has PIL):
#   docker run --rm -u $(id -u):$(id -g) -e HOME=/tmp -v "$PWD/vst":/s:ro -v "$PWD/vst/images":/o mpc-vst-html-art python3 /s/make_note_strip.py
# 43-frame slider filmstrip for the EDIT pages' NOTE sliders: values -7..+35 (one scale octave below the root, five above), root at frame 7.
from PIL import Image, ImageDraw
W, H, N, ZERO, S = 52, 270, 43, 7, 4
PAD, TR = 22, 18
def y_of(k): return PAD + (H - 2 * PAD) * (1 - k / (N - 1))
strip = Image.new("RGBA", (W, H * N), (0, 0, 0, 0))
for k in range(N):
    im = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, W * S - 1, H * S - 1], radius=W * S // 2, fill=(10, 10, 10, 255))
    zy, ty = y_of(ZERO) * S, y_of(k) * S
    d.rectangle([6 * S, zy - S, (W - 6) * S, zy + S], fill=(240, 237, 228, 255))          # the root line
    d.rounded_rectangle([(W / 2 - 4) * S, min(zy, ty), (W / 2 + 4) * S, max(zy, ty)], radius=4 * S, fill=(245, 196, 0, 255))
    d.ellipse([(W / 2 - TR) * S, ty - TR * S, (W / 2 + TR) * S, ty + TR * S], fill=(58, 58, 58, 255), outline=(245, 196, 0, 255) if k == ZERO else (20, 20, 20, 255), width=2 * S)
    strip.paste(im.resize((W, H), Image.LANCZOS), (0, k * H))
import sys
strip.save(sys.argv[1] if len(sys.argv) > 1 else "/o/note_strip.png")
