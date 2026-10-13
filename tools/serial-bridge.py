#!/usr/bin/env python3
"""Міст Serial -> сайт: замінює Serial Monitor і пересилає рядки з даними на сервер."""

import argparse
import os
import sys
import threading
import urllib.error
import urllib.request

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("Потрібен pyserial:  pip install pyserial")


def post_line(url, line, api_key):
    req = urllib.request.Request(
        url.rstrip("/") + "/api/line",
        data=line.encode("utf-8"),
        headers={"Content-Type": "text/plain; charset=utf-8", **({"X-API-Key": api_key} if api_key else {})},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=3) as r:
            return r.status, ""
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except urllib.error.URLError as e:
        return None, str(e.reason)


def forward(args, line, tag):
    status, err = post_line(args.url, line, args.api_key)
    if status == 200:
        mark = "✓"
    elif status == 400:
        mark = "· пропущено (не дані)"
    else:
        mark = f"✗ сервер: {status or err}"
    if not args.quiet or status not in (200, 400):
        print(f"{tag} {line}   {mark}", flush=True)


def reader(ser, args):
    while True:
        try:
            raw = ser.readline()
        except serial.SerialException as e:
            print(f"Порт закрився: {e}", flush=True)
            os._exit(1)
        line = raw.decode("utf-8", "replace").strip()
        if line:
            forward(args, line, "ESP>")


def main():
    p = argparse.ArgumentParser(description="Serial -> ecg-web міст")
    p.add_argument("--port", help="COM5, /dev/ttyACM0, /dev/cu.usbmodem…")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--url", default="http://localhost:3000")
    p.add_argument("--api-key", default=os.environ.get("API_KEY", ""))
    p.add_argument("--post-typed", action="store_true", help="також надсилати на сайт те, що набрано в консолі")
    p.add_argument("--quiet", action="store_true", help="не друкувати успішно переслані рядки")
    p.add_argument("--list", action="store_true", help="показати доступні порти")
    args = p.parse_args()

    if args.list or not args.port:
        ports = list(list_ports.comports())
        print("Доступні порти:" if ports else "Портів не знайдено. ESP32 підключена?")
        for pt in ports:
            print(f"  {pt.device}  {pt.description}")
        if not args.port:
            print("\nВкажіть порт: --port <назва>")
        return

    try:
        ser = serial.serial_for_url(args.port, baudrate=args.baud, timeout=1)
    except serial.SerialException as e:
        sys.exit(f"Не вдалося відкрити {args.port}: {e}\nЗакрийте Serial Monitor в Arduino IDE, якщо він відкритий.")

    print(f"{args.port} @ {args.baud} -> {args.url}/api/line. Набирайте рядки й Enter, Ctrl+C — вихід.", flush=True)
    threading.Thread(target=reader, args=(ser, args), daemon=True).start()

    try:
        for typed in sys.stdin:
            typed = typed.strip()
            if not typed:
                continue
            ser.write((typed + "\n").encode("utf-8"))
            if args.post_typed:
                forward(args, typed, "ВИ> ")
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()


if __name__ == "__main__":
    main()
