#!/usr/bin/env python3
"""发布 App（内置网关固件）到 gitee 发布仓库，并通过云端通知在线的 App。

  tools/release_app.py 0.2.0 --notes "更新说明" [--gw-version 0.4.1] [--skip-gateway] [--skip-nodes] [--dry-run]

步骤：
  1. 编译网关固件（签名），版本 --gw-version（默认取 gateway/CMakeLists.txt）；
     编译各种节点固件（NODE_BUILDS），版本取 node/CMakeLists.txt
  2. 打 App release 包（发布签名，内置网关固件），版本号即参数
  3. 在 gitee 发布仓库建 Release vX.Y.Z，上传 APK、网关固件和节点固件，更新 version.json
  4. 往云端 espgw/app/latest 发布保留消息（需要 pip install paho-mqtt）

本地凭据（local/，不上传）：
  local/gitee_token          gitee 私人令牌（projects 权限）
  local/keystore.properties  App 发布签名
  local/cloud.json           云端 broker（host/port/username/password）
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import ssl
import struct
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
import uuid

ROOT = pathlib.Path(__file__).resolve().parent.parent
LOCAL = ROOT / 'local'
OWNER, REPO = 'yu_ji_hai', 'app_release'
API = f'https://gitee.com/api/v5/repos/{OWNER}/{REPO}'
GW_BIN = ROOT / 'gateway' / 'build' / 'esp_s3_gateway.bin'
# 节点固件种类：(编译目录, 额外 idf.py 参数)；工程名和芯片从生成的镜像里读
NODE_BUILDS = [
    ('build_c3', ['-DIDF_TARGET=esp32c3', '-DSDKCONFIG=build_c3/sdkconfig']),
    ('build_s3', ['-DIDF_TARGET=esp32s3', '-DSDKCONFIG=build_s3/sdkconfig']),
    ('build_korvo', ['-DIDF_TARGET=esp32s3', '-DSDKCONFIG=build_korvo/sdkconfig', '-DNODE_PROJECT=esp_s3_korvo',
                     '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.korvo']),
]
CHIPS = {5: 'esp32c3', 9: 'esp32s3', 13: 'esp32c6'}
APK = ROOT / 'android' / 'app' / 'build' / 'outputs' / 'apk' / 'release' / 'app-release.apk'


def die(msg):
    sys.exit(f'错误：{msg}')


def run(cmd, cwd=None, env=None):
    print('$', cmd if isinstance(cmd, str) else ' '.join(cmd))
    shell = isinstance(cmd, str)
    subprocess.run(cmd, cwd=cwd, env=env, check=True, shell=shell, executable='/bin/bash' if shell else None)


def fw_version(path):
    """读固件 esp_app_desc_t：返回 (project, version)"""
    return fw_info(path)[:2]


def fw_info(path):
    """读固件镜像头和 esp_app_desc_t：返回 (project, version, chip)"""
    head = path.read_bytes()[:256]
    if head[0] != 0xE9 or struct.unpack_from('<I', head, 32)[0] != 0xABCD5432:
        die(f'{path} 不是 ESP app 镜像')
    s = lambda off: head[off:off + 32].split(b'\0')[0].decode()
    chip = CHIPS.get(struct.unpack_from('<H', head, 12)[0], 'unknown')
    return s(32 + 48), s(32 + 16), chip


def build_nodes():
    """编译所有节点固件，返回 [(path, project, version, chip)]"""
    ver = re.search(r'set\(PROJECT_VER "([^"]+)"\)', (ROOT / 'node' / 'CMakeLists.txt').read_text()).group(1)
    idf = os.environ.get('IDF_PATH', str(pathlib.Path.home() / 'esp' / 'esp-idf'))
    out = []
    for bdir, args in NODE_BUILDS:
        quoted = ' '.join(f"'{a}'" for a in args)
        run(f'. "{idf}/export.sh" >/dev/null && idf.py -B {bdir} {quoted} -DPROJECT_VER={ver} build',
            cwd=ROOT / 'node')
        bins = [p for p in (ROOT / 'node' / bdir).glob('*.bin')
                if not p.name.endswith('-unsigned.bin') and p.name not in ('bootloader.bin', 'ota_data_initial.bin')
                and p.read_bytes()[:1] == b'\xe9']
        if len(bins) != 1:
            die(f'node/{bdir} 里找不到唯一的固件：{[p.name for p in bins]}')
        project, v, chip = fw_info(bins[0])
        if v != ver:
            die(f'{bins[0]} 版本是 {v}，应为 {ver}')
        print(f'节点固件 {project} {chip} v{v}')
        out.append((bins[0], project, v, chip))
    return out


def version_code(v):
    a, b, c = (int(x) for x in v.split('.'))
    return a * 10000 + b * 100 + c


def gitee(method, path, data=None, files=None):
    token = (LOCAL / 'gitee_token').read_text().strip()
    url = API + path
    if method == 'GET':
        url += ('&' if '?' in url else '?') + 'access_token=' + token
        body, headers = None, {}
    elif files:
        boundary = uuid.uuid4().hex
        parts = []
        for k, v in {**(data or {}), 'access_token': token}.items():
            parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{k}"\r\n\r\n{v}\r\n'.encode())
        for k, p in files.items():
            parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{k}"; filename="{p.name}"\r\n'
                         f'Content-Type: application/vnd.android.package-archive\r\n\r\n'.encode() + p.read_bytes() + b'\r\n')
        parts.append(f'--{boundary}--\r\n'.encode())
        body = b''.join(parts)
        headers = {'Content-Type': f'multipart/form-data; boundary={boundary}'}
    else:
        body = json.dumps({**(data or {}), 'access_token': token}).encode()
        headers = {'Content-Type': 'application/json'}
    req = urllib.request.Request(url, data=body, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            text = r.read().decode()
            return json.loads(text) if text else None
    except urllib.error.HTTPError as e:
        if e.code == 404 and method == 'GET':
            return None
        die(f'gitee {method} {path}: HTTP {e.code} {e.read().decode()[:300]}')


def notify_cloud(manifest):
    cfg_path = LOCAL / 'cloud.json'
    if not cfg_path.exists():
        print('跳过云端通知：没有 local/cloud.json')
        return
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        print('跳过云端通知：没装 paho-mqtt（pip install paho-mqtt）')
        return
    cfg = json.loads(cfg_path.read_text())
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id='espgw-release-' + uuid.uuid4().hex[:6])
    c.username_pw_set(cfg['username'], cfg['password'])
    c.tls_set(cert_reqs=ssl.CERT_REQUIRED)
    c.connect(cfg['host'], cfg.get('port', 8883), 30)
    c.loop_start()
    payload = json.dumps({k: manifest[k] for k in ('versionCode', 'versionName', 'gatewayVersion')})
    c.publish('espgw/app/latest', payload, qos=1, retain=True).wait_for_publish(10)
    c.loop_stop()
    c.disconnect()
    print('已通知云端：espgw/app/latest', payload)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('version', help='App 版本 x.y.z')
    ap.add_argument('--notes', default='', help='更新说明')
    ap.add_argument('--gw-version', help='网关固件版本，默认取 gateway/CMakeLists.txt')
    ap.add_argument('--skip-gateway', action='store_true', help='不重新编译网关，直接用 gateway/build 里的固件')
    ap.add_argument('--skip-nodes', action='store_true', help='不发布节点固件')
    ap.add_argument('--dry-run', action='store_true', help='只编译，不上传、不通知')
    ap.add_argument('--copy-to', help='另外拷贝 APK 到这个目录')
    a = ap.parse_args()

    if not re.fullmatch(r'\d+\.\d+\.\d+', a.version):
        die('版本号格式应为 x.y.z')
    for f in ('gitee_token', 'keystore.properties'):
        if not (LOCAL / f).exists():
            die(f'缺少 local/{f}')
    tag = 'v' + a.version
    if not a.dry_run and gitee('GET', f'/releases/tags/{tag}'):
        die(f'发布仓库已有 {tag}')

    # 1. 网关固件
    if not a.skip_gateway:
        gw_ver = a.gw_version or re.search(r'set\(PROJECT_VER "([^"]+)"\)',
                                           (ROOT / 'gateway' / 'CMakeLists.txt').read_text()).group(1)
        idf = os.environ.get('IDF_PATH', str(pathlib.Path.home() / 'esp' / 'esp-idf'))
        run(f'. "{idf}/export.sh" >/dev/null && idf.py -DPROJECT_VER={gw_ver} build', cwd=ROOT / 'gateway')
    project, gw_ver = fw_version(GW_BIN)
    if project != 'esp_s3_gateway':
        die(f'固件工程名是 {project}')
    print(f'网关固件 {project} v{gw_ver}')
    nodes = [] if a.skip_nodes else build_nodes()

    # 2. App
    env = dict(os.environ)
    env.setdefault('JAVA_HOME', str(pathlib.Path.home() / '.local' / 'jdk'))
    cmd = ['./gradlew', '-q', f'-PappVersion={a.version}', ':app:assembleRelease']
    proxy = urllib.parse.urlparse(os.environ.get('HTTPS_PROXY') or os.environ.get('https_proxy') or '')
    if proxy.hostname:
        cmd[1:1] = [f'-Dhttps.proxyHost={proxy.hostname}', f'-Dhttps.proxyPort={proxy.port}',
                    f'-Dhttp.proxyHost={proxy.hostname}', f'-Dhttp.proxyPort={proxy.port}']
    run(cmd, cwd=ROOT / 'android', env=env)
    name = f'EspGateway-{a.version}.apk'
    out = ROOT / 'android' / 'app' / 'build' / 'outputs' / name
    shutil.copy(APK, out)
    data = out.read_bytes()
    # 网关固件也作为附件发布：远程升级时网关自己下载（App 通过云端下发地址和 SHA-256）
    gw_name = f'esp_s3_gateway-{gw_ver}.bin'
    gw_out = out.parent / gw_name
    shutil.copy(GW_BIN, gw_out)
    gw_data = gw_out.read_bytes()
    manifest = {
        'versionCode': version_code(a.version),
        'versionName': a.version,
        'url': f'https://gitee.com/{OWNER}/{REPO}/releases/download/{tag}/{name}',
        'sha256': hashlib.sha256(data).hexdigest(),
        'size': len(data),
        'notes': a.notes,
        'gatewayVersion': gw_ver,
        'gatewayUrl': f'https://gitee.com/{OWNER}/{REPO}/releases/download/{tag}/{gw_name}',
        'gatewaySha256': hashlib.sha256(gw_data).hexdigest(),
        'gatewaySize': len(gw_data),
        'nodes': [],
    }
    node_files = []
    for path, project, ver, chip in nodes:
        name_n = f'{project}-{chip}-{ver}.bin'
        dst = out.parent / name_n
        shutil.copy(path, dst)
        data_n = dst.read_bytes()
        node_files.append(dst)
        manifest['nodes'].append({
            'project': project, 'chip': chip, 'version': ver,
            'url': f'https://gitee.com/{OWNER}/{REPO}/releases/download/{tag}/{name_n}',
            'sha256': hashlib.sha256(data_n).hexdigest(), 'size': len(data_n),
        })
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    if a.copy_to:
        shutil.copy(out, pathlib.Path(a.copy_to) / name)
        print('已拷贝到', a.copy_to)
    if a.dry_run:
        return

    # 3. gitee：Release + APK，再更新 version.json（App 以它为准，最后更新）
    rel = gitee('POST', '/releases', {'tag_name': tag, 'name': tag, 'target_commitish': 'master',
                                      'body': (a.notes or tag) + f'\n\n内置网关固件 v{gw_ver}' +
                                      ''.join(f'\n节点固件 {p} {c} v{v}' for _, p, v, c in nodes)})
    att = gitee('POST', f'/releases/{rel["id"]}/attach_files', files={'file': out})
    if att.get('browser_download_url'):
        manifest['url'] = att['browser_download_url']
    print('已上传', manifest['url'])
    att = gitee('POST', f'/releases/{rel["id"]}/attach_files', files={'file': gw_out})
    if att.get('browser_download_url'):
        manifest['gatewayUrl'] = att['browser_download_url']
    print('已上传', manifest['gatewayUrl'])
    for dst, entry in zip(node_files, manifest['nodes']):
        att = gitee('POST', f'/releases/{rel["id"]}/attach_files', files={'file': dst})
        if att.get('browser_download_url'):
            entry['url'] = att['browser_download_url']
        print('已上传', entry['url'])
    old = gitee('GET', '/contents/version.json')
    content = __import__('base64').b64encode(
        (json.dumps(manifest, ensure_ascii=False, indent=2) + '\n').encode()).decode()
    body = {'content': content, 'message': f'release {tag}'}
    if old and isinstance(old, dict) and old.get('sha'):
        gitee('PUT', '/contents/version.json', {**body, 'sha': old['sha']})
    else:
        gitee('POST', '/contents/version.json', body)
    print('已更新 version.json')

    # 4. 云端通知
    notify_cloud(manifest)


if __name__ == '__main__':
    main()
