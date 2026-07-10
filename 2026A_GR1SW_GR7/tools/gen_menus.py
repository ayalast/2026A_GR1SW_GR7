"""Generate horror-styled start/pause menu textures (1920x1080)."""
from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageEnhance
import math
import random
import os

W, H = 1920, 1080
random.seed(7)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEX = os.path.join(ROOT, "textures")


def find_font(size, bold=False):
    windir = os.environ.get("WINDIR", r"C:\Windows")
    fonts = os.path.join(windir, "Fonts")
    if bold:
        candidates = [
            os.path.join(fonts, "georgia.ttf"),
            os.path.join(fonts, "georgiab.ttf"),
            os.path.join(fonts, "timesbd.ttf"),
            os.path.join(fonts, "arialbd.ttf"),
            os.path.join(fonts, "segoeuib.ttf"),
            os.path.join(fonts, "GARA.TTF"),
        ]
    else:
        candidates = [
            os.path.join(fonts, "georgia.ttf"),
            os.path.join(fonts, "times.ttf"),
            os.path.join(fonts, "arial.ttf"),
            os.path.join(fonts, "segoeui.ttf"),
            os.path.join(fonts, "GARA.TTF"),
        ]
    for p in candidates:
        if os.path.isfile(p):
            try:
                return ImageFont.truetype(p, size)
            except Exception:
                pass
    return ImageFont.load_default()


def make_bg():
    img = Image.new("RGB", (W, H), (6, 6, 5))
    px = img.load()
    cx, cy = W * 0.5, H * 0.48
    for y in range(H):
        for x in range(0, W, 2):
            dx = (x - cx) / W
            dy = (y - cy) / H
            r = math.sqrt(dx * dx + dy * dy)
            base = 7 + int(8 * max(0.0, 1.0 - r * 1.6))
            g = base + 1
            b = max(0, base - 2)
            scan = 1 if (y % 3 == 0) else 0
            v = max(0, base - scan)
            c = (v, max(0, g - scan), b)
            px[x, y] = c
            if x + 1 < W:
                px[x + 1, y] = c
    for _ in range(90000):
        x = random.randint(0, W - 1)
        y = random.randint(0, H - 1)
        n = random.randint(-10, 12)
        r, g, b = px[x, y]
        px[x, y] = (
            max(0, min(255, r + n)),
            max(0, min(255, g + n)),
            max(0, min(255, b + n // 2)),
        )
    return img.filter(ImageFilter.GaussianBlur(radius=0.6))


def draw_corners(draw, color=(176, 154, 78), inset=52, length=78, width=2):
    pts = [
        [(inset, inset + length), (inset, inset), (inset + length, inset)],
        [(W - inset - length, inset), (W - inset, inset), (W - inset, inset + length)],
        [(inset, H - inset - length), (inset, H - inset), (inset + length, H - inset)],
        [(W - inset - length, H - inset), (W - inset, H - inset), (W - inset, H - inset - length)],
    ]
    for poly in pts:
        draw.line(poly, fill=color, width=width)


def text_center(draw, text, y, font, fill, shadow=True):
    bbox = draw.textbbox((0, 0), text, font=font)
    tw = bbox[2] - bbox[0]
    x = (W - tw) // 2
    if shadow:
        draw.text((x + 2, y + 2), text, font=font, fill=(0, 0, 0))
    draw.text((x, y), text, font=font, fill=fill)


def draw_button(draw, cx, cy, hw, hh, fill, outline, label, font, label_fill, glow=None):
    x0, y0 = int(cx - hw), int(cy - hh)
    x1, y1 = int(cx + hw), int(cy + hh)
    if glow:
        for i in range(8, 0, -1):
            c = tuple(max(0, min(255, v + i * 2)) for v in glow)
            draw.rounded_rectangle(
                [x0 - i, y0 - i, x1 + i, y1 + i],
                radius=6 + i // 2,
                outline=c,
                width=1,
            )
    draw.rounded_rectangle([x0, y0, x1, y1], radius=4, fill=fill, outline=outline, width=2)
    draw.line(
        [(x0 + 8, y0 + 2), (x1 - 8, y0 + 2)],
        fill=tuple(min(255, c + 30) for c in outline),
        width=1,
    )
    bbox = draw.textbbox((0, 0), label, font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    tx = cx - tw // 2
    ty = cy - th // 2 - 1
    draw.text((tx + 1, ty + 1), label, font=font, fill=(0, 0, 0))
    draw.text((tx, ty), label, font=font, fill=label_fill)


def build_menu(kind):
    img = make_bg()
    draw = ImageDraw.Draw(img)

    gold = (196, 172, 96)
    gold_dim = (140, 122, 70)
    muted = (120, 118, 108)
    dim = (88, 86, 78)

    title_font = find_font(92, bold=True)
    sub_font = find_font(28, bold=False)
    btn_font = find_font(30, bold=True)
    foot_font = find_font(22, bold=False)
    tiny_font = find_font(18, bold=False)

    draw_corners(draw, gold_dim, inset=52, length=78, width=2)
    draw.rectangle([70, 70, W - 70, H - 70], outline=(40, 38, 28), width=1)

    for y in (100, H - 100):
        draw.line([(160, y), (W // 2 - 80, y)], fill=(50, 46, 30), width=1)
        draw.line([(W // 2 + 80, y), (W - 160, y)], fill=(50, 46, 30), width=1)

    if kind == "start":
        title = "BACKROOMS"
        subtitle = "no hay salida"
        primary = "COMENZAR"
        footer = "Enter = Comenzar   ·   Esc = Salir"
        brand = "EPN  ·  Sistemas  ·  Grupo 7"
    else:
        title = "PAUSA"
        subtitle = "el silencio se queda"
        primary = "CONTINUAR"
        footer = "Enter / Esc = Continuar"
        brand = "no mires atras"

    y_title = 320
    text_center(draw, title, y_title, title_font, gold)

    y_sub = y_title + 110
    text_center(draw, subtitle, y_sub, sub_font, muted)
    ux0 = (W - 48) // 2
    uy = y_sub + 42
    draw.line([(ux0, uy), (ux0 + 48, uy)], fill=gold_dim, width=2)

    # Hitboxes in main.cpp: cy=0.546 / 0.634, hw=0.115, hh=0.04
    cx = W // 2
    hw = int(0.115 * W)
    hh = int(0.04 * H)
    cy1 = int(0.546 * H)
    cy2 = int(0.634 * H)

    draw.line(
        [(cx - hw - 20, cy1 - hh - 28), (cx + hw + 20, cy1 - hh - 28)],
        fill=(45, 42, 30),
        width=1,
    )

    draw_button(
        draw,
        cx,
        cy1,
        hw,
        hh,
        fill=(58, 50, 22),
        outline=(176, 154, 78),
        label=primary,
        font=btn_font,
        label_fill=(222, 204, 130),
        glow=(90, 78, 30),
    )
    draw_button(
        draw,
        cx,
        cy2,
        hw,
        hh,
        fill=(22, 22, 22),
        outline=(70, 68, 62),
        label="SALIR",
        font=btn_font,
        label_fill=(170, 168, 158),
        glow=None,
    )

    text_center(draw, footer, H - 90, foot_font, dim, shadow=False)
    text_center(draw, brand, H - 58, tiny_font, (55, 54, 48), shadow=False)

    img = ImageEnhance.Contrast(img).enhance(1.08)
    img = ImageEnhance.Color(img).enhance(0.92)
    return img


def main():
    os.makedirs(TEX, exist_ok=True)
    out_start = os.path.join(TEX, "menu_start.png")
    out_pause = os.path.join(TEX, "menu_pause.png")
    build_menu("start").save(out_start, "PNG", optimize=True)
    build_menu("pause").save(out_pause, "PNG", optimize=True)
    print("wrote", out_start, os.path.getsize(out_start))
    print("wrote", out_pause, os.path.getsize(out_pause))


if __name__ == "__main__":
    main()
