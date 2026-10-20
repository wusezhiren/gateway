#!/usr/bin/env python3
"""生成网关屏幕用的中文 LVGL 字库（需要 node/npx）。

  tools/gen_fonts.py --font <中文 TTF>

font_cn_16：ASCII + 中文标点 + GB2312 一级汉字（3755）+ UI 源码里出现的字，正文用
font_cn_24：ASCII + UI 源码里出现的字，标题用
图标等缺字回退到 LVGL 自带的 Montserrat（LV_SYMBOL_*）。
UI 文案新增了不常用的字后需要重新生成。
"""
import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
UI_DIR = ROOT / 'gateway' / 'main' / 'ui'
OUT_DIR = UI_DIR / 'fonts'
PUNCT = '，。、：；！？（）【】《》“”‘’—…·％～￥'


def gb2312_level1():
    chars = []
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            if hi == 0xD7 and lo > 0xF9:
                break
            chars.append(bytes([hi, lo]).decode('gb2312'))
    return ''.join(chars)


def ui_chars():
    text = ''.join(p.read_text(encoding='utf-8') for p in sorted(UI_DIR.glob('*.c')))
    return ''.join(sorted(set(re.findall(r'[　-〿一-鿿＀-￯“”‘’—…·]', text))))


def conv(font, size, symbols, name):
    out = OUT_DIR / f'{name}.c'
    cmd = ['npx', '-y', 'lv_font_conv@1.5.3', '--font', str(font), '-r', '0x20-0x7E', '--symbols', symbols,
           '--size', str(size), '--bpp', '4', '--format', 'lvgl', '--lv-include', 'lvgl.h',
           '--lv-font-name', name, '--lv-fallback', f'lv_font_montserrat_{size}', '-o', str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f'lv_font_conv 失败 ({name}):\n{r.stderr.strip()[-2000:]}')
    # 头部注释里的 Opts 行带本机路径和整张字表，换成简短说明
    lines = out.read_text(encoding='utf-8').splitlines(keepends=True)
    for i, line in enumerate(lines[:10]):
        if line.startswith(' * Opts:'):
            lines[i] = (f' * Opts: --font {pathlib.Path(font).name} -r 0x20-0x7E --symbols <{len(symbols)} chars, '
                        f'see tools/gen_fonts.py> --size {size} --bpp 4 --lv-fallback lv_font_montserrat_{size}\n')
    out.write_text(''.join(lines), encoding='utf-8')
    print(f'{out.relative_to(ROOT)}: {len(symbols)} 个非 ASCII 字符, {out.stat().st_size // 1024} KB 源码')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--font', required=True, help='中文 TTF/OTF（不支持 .ttc）')
    args = ap.parse_args()
    OUT_DIR.mkdir(exist_ok=True)
    ui = ui_chars()
    body = ''.join(sorted(set(PUNCT + gb2312_level1() + ui)))
    conv(args.font, 16, body, 'font_cn_16')
    conv(args.font, 24, ''.join(sorted(set(PUNCT + ui))), 'font_cn_24')
    return 0


if __name__ == '__main__':
    sys.exit(main())
