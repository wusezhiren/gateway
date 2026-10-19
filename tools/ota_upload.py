#!/usr/bin/env python3
"""上传签名固件到网关进行 OTA。

用法:
  tools/ota_upload.py <网关IP> <固件.bin> --token <token> [--force]

token 可在网关串口执行 `token` 获取，或在配网成功时由 /api/prov/status 返回。
"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.request


# 局域网设备，不走系统代理（urllib 不认 NO_PROXY 里的网段写法）
_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def get_info(host):
    with _opener.open(f'http://{host}/api/info', timeout=5) as r:
        return json.load(r)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('host')
    ap.add_argument('image')
    ap.add_argument('--token', required=True)
    ap.add_argument('--force', action='store_true', help='允许相同或更低版本')
    ap.add_argument('--no-wait', action='store_true', help='上传后不等待网关重启')
    args = ap.parse_args()

    data = open(args.image, 'rb').read()
    before = get_info(args.host)
    print(f"网关当前: {before['project']} v{before['version']} ({before['partition']})")
    print(f'上传 {args.image} ({len(data)} 字节)...')

    url = f'http://{args.host}/api/ota/gateway' + ('?force=1' if args.force else '')
    req = urllib.request.Request(url, data=data, method='POST', headers={
        'Authorization': f'Bearer {args.token}',
        'Content-Type': 'application/octet-stream',
    })
    t0 = time.time()
    try:
        with _opener.open(req, timeout=180) as r:
            resp = json.load(r)
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors='replace')
        print(f'失败: HTTP {e.code} {body}')
        return 1
    print(f'完成 ({time.time() - t0:.1f}s): {resp}')
    if args.no_wait:
        return 0

    print('等待网关重启...')
    time.sleep(resp.get('reboot_in_ms', 1500) / 1000 + 3)
    for _ in range(30):
        try:
            after = get_info(args.host)
            print(f"网关现在: {after['project']} v{after['version']} ({after['partition']})")
            return 0 if after['version'] == resp['version'] else 2
        except Exception:
            time.sleep(1)
    print('网关没有回来，检查串口日志')
    return 3


if __name__ == '__main__':
    sys.exit(main())
