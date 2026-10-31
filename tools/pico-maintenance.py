#!/usr/bin/env python3
"""Bounded Pico serial/UF2 maintenance using only the Python standard library.

`console` is read-only by default (protocol and CEC diagnostics). `flash` changes
firmware and must be explicitly authorized by the operator before invocation.
No command installs packages/services or changes PC power state.
"""
import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import select
import struct
import subprocess
import termios
import time


class Console:
    def __init__(self, device):
        self.fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.exclusive = False
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.ioctl(self.fd, termios.TIOCEXCL)
            self.exclusive = True
            self.original = termios.tcgetattr(self.fd)
            cfg = termios.tcgetattr(self.fd)
            cfg[0] = cfg[1] = cfg[3] = 0
            cfg[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            cfg[4] = cfg[5] = termios.B115200
            cfg[6][termios.VMIN] = cfg[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, cfg)
            fcntl.ioctl(self.fd, termios.TIOCMBIS, struct.pack('I', termios.TIOCM_DTR))
            self.receive(0.2)
        except BaseException:
            self.close()
            raise

    def close(self):
        with contextlib.suppress(OSError, termios.error):
            if hasattr(self, 'original'):
                termios.tcsetattr(self.fd, termios.TCSANOW, self.original)
        with contextlib.suppress(OSError, termios.error):
            if self.exclusive:
                fcntl.ioctl(self.fd, termios.TIOCNXCL)
        os.close(self.fd)

    def receive(self, seconds):
        deadline = time.monotonic() + seconds
        chunks = []
        size = 0
        while time.monotonic() < deadline:
            if select.select([self.fd], [], [], max(0, deadline-time.monotonic()))[0]:
                try:
                    data = os.read(self.fd, 8192)
                except BlockingIOError:
                    continue
                if not data:
                    break
                size += len(data)
                if size > 65536:
                    raise RuntimeError('serial output exceeded the diagnostic limit')
                chunks.append(data)
        return re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', b''.join(chunks).decode('utf-8', 'replace'))

    def command(self, command, seconds=0.8):
        if '\r' in command or '\n' in command:
            raise ValueError('expected one command')
        data = (command+'\r').encode()
        offset = 0
        deadline = time.monotonic()+1
        while offset < len(data) and time.monotonic() < deadline:
            try:
                offset += os.write(self.fd, data[offset:])
            except BlockingIOError:
                select.select([], [self.fd], [], 0.05)
        if offset != len(data):
            raise RuntimeError('serial write deadline expired')
        return self.receive(seconds)


def usb_parent(path):
    for candidate in [path.resolve(), *path.resolve().parents]:
        if (candidate/'idVendor').is_file() and (candidate/'idProduct').is_file():
            return candidate
    raise RuntimeError('cannot identify USB ancestry')


def validate_uf2(data, checksum):
    if hashlib.sha256(data).hexdigest() != checksum:
        raise ValueError('firmware checksum mismatch')
    if not data or len(data) % 512:
        raise ValueError('invalid UF2 length')
    count = len(data)//512
    addresses = set()
    for index in range(count):
        block = data[index*512:(index+1)*512]
        magic0, magic1, flags, address, size, number, total, family = struct.unpack_from('<8I', block)
        if ((magic0, magic1) != (0x0a324655, 0x9e5d5157) or flags != 0x2000
                or struct.unpack_from('<I', block, 508)[0] != 0x0ab16f30
                or number != index or total != count or family != 0xe48bff56
                or size != 256 or address % 256 or address in addresses
                or not 0x10000000 <= address < address+size <= 0x101fc000):
            raise ValueError('invalid RP2040 UF2 block or reserved-flash write')
        addresses.add(address)
    if min(addresses) != 0x10000000:
        raise ValueError('UF2 does not include the flash boot image')


def boot_drive(topology):
    data = json.loads(subprocess.check_output(['lsblk', '--json', '-o', 'PATH,LABEL,MOUNTPOINTS'], text=True))
    def walk(entries):
        for entry in entries:
            if entry.get('label') == 'RPI-RP2':
                block = Path(entry['path'])
                parent = usb_parent(Path('/sys/class/block')/block.name)
                if (parent.name == topology and (parent/'idVendor').read_text().strip() == '2e8a'
                        and (parent/'idProduct').read_text().strip() == '0003'):
                    mounts = [p for p in entry.get('mountpoints', []) if p]
                    return block, Path(mounts[0]) if mounts else None
            result = walk(entry.get('children', []))
            if result:
                return result
        return None
    return walk(data['blockdevices'])


def flash(args):
    data = args.image.read_bytes()
    validate_uf2(data, args.sha256)
    serial = None
    if args.bootloader_port:
        if not re.fullmatch(r'[0-9]+-[0-9]+(?:[.][0-9]+)*', args.bootloader_port):
            raise ValueError('invalid physical USB port')
        topology = args.bootloader_port
    else:
        tty = Path(args.device).resolve().name
        parent = usb_parent(Path('/sys/class/tty')/tty/'device')
        if ((parent/'idVendor').read_text().strip(), (parent/'idProduct').read_text().strip()) != ('cafe', '4001'):
            raise RuntimeError('selected tty is not the Couch CEC controller')
        serial = (parent/'serial').read_text().strip()
        topology = parent.name
        console = Console(args.device)
        try:
            protocol = console.command('tv protocol')
            if not re.search(r'CECCTRL/[12] OK command=protocol\b', protocol) or 'busy=1' in protocol:
                raise RuntimeError('controller did not confirm compatibility or is busy')
            cec = console.command('show cec')
            if not re.search(r'CEC GPIO\s*:\s*GP'+str(args.gpio)+r'\b', cec):
                raise RuntimeError('current controller GPIO differs from the approved GPIO')
            print("Requesting BOOTSEL on USB port "+topology+".", flush=True)
        # The operator authorized this exact image/checksum before this subcommand.
            try:
                console.command('reboot bootsel', 0.2)
            except OSError:
                pass  # USB disconnect is expected after the bootloader request.
        finally:
            console.close()
    print('Locating BOOTSEL on USB port '+topology+'.', flush=True)
    drive = None
    deadline = time.monotonic()+12
    while time.monotonic() < deadline:
        drive = boot_drive(topology)
        if drive and drive[1]:
            break
        time.sleep(0.2)
    if not drive:
        raise RuntimeError('matching BOOTSEL drive did not appear')
    block, mount = drive
    if mount is None:
        subprocess.run(['udisksctl', 'mount', '--no-user-interaction', '--block-device', str(block)], check=True)
        drive = boot_drive(topology)
        mount = drive[1] if drive else None
    if mount is None:
        raise RuntimeError('BOOTSEL mount requires a local operator')
    info = (mount/'INFO_UF2.TXT').read_text()
    if 'Model: Raspberry Pi RP2' not in info or 'Board-ID: RPI-RP2' not in info:
        raise RuntimeError('unexpected bootloader identity')
    with (mount/'pico-cec.uf2').open('wb', buffering=0) as output:
        if output.write(data) != len(data):
            raise RuntimeError('incomplete UF2 write')
        os.fsync(output.fileno())
    print('Approved GP%d UF2 copied and flushed.' % args.gpio, flush=True)
    deadline = time.monotonic()+32
    while time.monotonic() < deadline:
        if Path(args.device).exists():
            try:
                tty = Path(args.device).resolve().name
                returned = usb_parent(Path('/sys/class/tty')/tty/'device')
                if returned.name == topology and (serial is None or (returned/'serial').read_text().strip() == serial):
                    break
            except (OSError, RuntimeError):
                pass
        time.sleep(0.2)
    else:
        raise RuntimeError('controller did not return on the expected USB port')
    console = Console(args.device)
    try:
        protocol = console.command('tv protocol')
        if not re.search(r'CECCTRL/'+str(args.protocol)+r' OK command=protocol\b', protocol):
            raise RuntimeError('flashed controller reports an unexpected protocol')
        if args.version and not re.search(r'firmware='+re.escape(args.version)+r'(?:\s|$)', protocol):
            raise RuntimeError('flashed controller reports an unexpected build')
        cec = console.command('show cec')
        if not re.search(r'CEC GPIO\s*:\s*GP'+str(args.gpio)+r'\b', cec):
            raise RuntimeError('flashed controller reports an unexpected GPIO')
        print(protocol.strip())
        print(cec.strip())
    finally:
        console.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', default='/dev/cec-controller')
    commands = parser.add_subparsers(dest='action', required=True)
    commands.add_parser('console')
    update = commands.add_parser('flash')
    update.add_argument('--image', type=Path, required=True)
    update.add_argument('--sha256', required=True)
    update.add_argument('--gpio', type=int, choices=range(30), required=True)
    update.add_argument('--protocol', type=int, choices=(1, 2), required=True)
    update.add_argument('--version')
    update.add_argument('--bootloader-port', help='explicit recovery on this physical USB port when the runtime tty is unavailable')
    args = parser.parse_args()
    if args.action == 'flash':
        flash(args)
    else:
        console = Console(args.device)
        try:
            for command in ('tv protocol', 'show cec', 'show stats cec'):
                print(console.command(command).strip())
        finally:
            console.close()


if __name__ == '__main__':
    main()
