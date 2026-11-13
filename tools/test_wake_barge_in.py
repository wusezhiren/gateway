#!/usr/bin/env python3
"""通过网关 HTTP 接口灌入 16 kHz PCM，回归唤醒期间再次唤醒打断。

示例：
  python3 tools/test_wake_barge_in.py 192.168.1.20 --token TOKEN audio.wav
  python3 tools/test_wake_barge_in.py --scan --token TOKEN audio.pcm

PCM 必须是 little-endian、16-bit、16 kHz、mono。WAV 可以是同样格式，脚本会去掉
WAV 头。音频应按顺序包含：第一次唤醒词+第一轮指令、第一轮回复期间的第二次唤醒词+
第二轮指令。网关会在一次 POST 中执行完整的文件测试和打断流程。
"""
import argparse
import ipaddress
import json
import socket
import struct
import sys
import time
import urllib.error
import urllib.request
import wave


OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def get_info(host):
    with OPENER.open("http://%s/api/info" % host, timeout=3) as response:
        return json.load(response)


def scan_hosts(network):
    for address in ipaddress.ip_network(network, strict=False).hosts():
        host = str(address)
        try:
            info = get_info(host)
        except (OSError, urllib.error.URLError, urllib.error.HTTPError):
            continue
        if info.get("project") == "esp_s3_gateway":
            yield host, info


def read_audio(path):
    if path.lower().endswith((".wav", ".wave")):
        with wave.open(path, "rb") as source:
            if (source.getnchannels(), source.getsampwidth(), source.getframerate()) != (1, 2, 16000):
                raise ValueError("WAV 必须是 16 kHz/16-bit/mono")
            return source.readframes(source.getnframes())
    data = open(path, "rb").read()
    if len(data) % 2:
        raise ValueError("PCM 字节数必须为偶数")
    return data


def post_pcm(host, token, data):
    request = urllib.request.Request(
        "http://%s/api/wake/test" % host,
        data=data,
        method="POST",
        headers={"Authorization": "Bearer " + token, "Content-Type": "application/octet-stream"},
    )
    with OPENER.open(request, timeout=30) as response:
        return response.status, response.read().decode(errors="replace")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("host", nargs="?", help="网关 IP 或主机名")
    parser.add_argument("--scan", action="store_true", help="扫描 --network 中的网关")
    parser.add_argument("--network", default=None, help="扫描网段，例如 192.168.137.0/24")
    parser.add_argument("--token", required=True)
    parser.add_argument("audio", help="16 kHz mono WAV 或裸 PCM 文件")
    args = parser.parse_args()

    data = read_audio(args.audio)
    if not data or len(data) > 320000:
        parser.error("音频长度必须为 1 字节到 320000 字节")
    hosts = []
    if args.host:
        hosts = [(args.host, get_info(args.host))]
    elif args.scan:
        network = args.network
        if not network:
            parser.error("--scan 必须同时指定 --network")
        hosts = list(scan_hosts(network))
    else:
        parser.error("请指定 host，或使用 --scan --network")
    if not hosts:
        print("未发现网关", file=sys.stderr)
        return 2
    for host, info in hosts:
        print("网关 %s: %s v%s, 音频 %.2fs (%d bytes)" %
              (host, info.get("project"), info.get("version"), len(data) / 32000.0, len(data)))
        try:
            status, body = post_pcm(host, args.token, data)
        except urllib.error.HTTPError as error:
            print("灌音失败: HTTP %d %s" % (error.code, error.read().decode(errors="replace")), file=sys.stderr)
            return 1
        print("灌音已接受: HTTP %d %s" % (status, body))
        print("等待文件测试和第二轮交互完成，请查看串口中的 file_test_detections、barge-in 日志")
    return 0


if __name__ == "__main__":
    sys.exit(main())
