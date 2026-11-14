"""Offline Android/XNUP contract checks; never opens a device or runs fastboot."""
from __future__ import annotations
import gzip
import hashlib
import struct
import zlib
from pathlib import Path

class ImageError(ValueError):
    pass

def number(contract, key):
    value = contract[key]
    return int(value, 0) if isinstance(value, str) else int(value)

def aligned(n, alignment=16):
    return (n + alignment - 1) // alignment * alignment

def pack_xnup(entries):
    body = bytearray()
    for name, data in entries.items():
        raw = name.encode('ascii')
        if not raw or len(raw) > 15 or b'\0' in raw:
            raise ImageError('XNUP entry names must be 1..15 ASCII bytes')
        data = bytes(data)
        body += struct.pack('<16sII', raw, len(data), 0)
        body += data + b'\0' * (-len(data) % 16)
    body += b'\0' * 24
    return struct.pack('<4sI', b'XNUP', len(body)) + body

def parse_xnup(data, cap):
    if len(data) < 32 or data[:4] != b'XNUP':
        raise ImageError('missing bootstrap XNUP container')
    body_size = struct.unpack_from('<I', data, 4)[0]
    total = 8 + body_size
    if total != len(data) or aligned(total) > cap:
        raise ImageError('XNUP declared size or reservation mismatch')
    result, pos = {}, 8
    while pos + 24 <= total:
        name_raw, size, reserved = struct.unpack_from('<16sII', data, pos)
        pos += 24
        if name_raw[0] == 0:
            if name_raw != b'\0' * 16 or size or reserved or pos != total:
                raise ImageError('invalid XNUP terminator')
            return result
        if b'\0' not in name_raw or reserved:
            raise ImageError('invalid XNUP entry header')
        try:
            name = name_raw.split(b'\0', 1)[0].decode('ascii')
        except UnicodeDecodeError as exc:
            raise ImageError('non-ASCII XNUP entry') from exc
        if name in result or pos + aligned(size) > total:
            raise ImageError('duplicate or truncated XNUP entry')
        result[name] = data[pos:pos + size]
        if any(data[pos + size:pos + aligned(size)]):
            raise ImageError('nonzero XNUP alignment padding')
        pos += aligned(size)
    raise ImageError('missing XNUP terminator')

def movz_values(data):
    values = []
    for offset in range(0, len(data) - 3, 4):
        word = struct.unpack_from('<I', data, offset)[0]
        # MOVZ Wn zero-extends into Xn; LLVM 22 uses this equally valid
        # encoding for the below-4-GiB payload and panic addresses.
        if word & 0x7F800000 == 0x52800000:
            values.append(((word >> 5) & 0xFFFF) << (((word >> 21) & 3) * 16))
    return values

def inspect_loader(data, contract):
    if len(data) < 64 or data[:2] != b'MZ':
        raise ImageError('loader is not a PE executable')
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if pe + 24 > len(data) or data[pe:pe+4] != b'PE\0\0':
        raise ImageError('invalid PE header')
    machine, sections = struct.unpack_from('<HH', data, pe + 4)
    optional_size = struct.unpack_from('<H', data, pe + 20)[0]
    optional = pe + 24
    if optional_size < 70 or optional + optional_size > len(data):
        raise ImageError('invalid PE optional header')
    if machine != 0xAA64 or struct.unpack_from('<H', data, optional)[0] != 0x20B:
        raise ImageError('loader is not ARM64 PE32+')
    if struct.unpack_from('<H', data, optional + 68)[0] != 10:
        raise ImageError('loader is not an EFI application')
    text = None
    for index in range(sections):
        section = optional + optional_size + index * 40
        if section + 40 > len(data):
            raise ImageError('truncated PE section table')
        name = data[section:section+8].rstrip(b'\0')
        size, start = struct.unpack_from('<II', data, section + 16)
        if start + size > len(data):
            raise ImageError('truncated PE section contents')
        if name == b'.text':
            text = data[start:start+size]
    if text is None:
        raise ImageError('missing loader .text')
    values = movz_values(text)
    base = number(contract, 'payload_base')
    panic = number(contract, 'panic_log_base')
    if base not in values or panic not in values or 0xF0000000 in values or 0xF3700000 in values:
        raise ImageError('loader payload/panic address relocation is stale')
    if contract.get('ram_only') is not True or b'K60PRO_USB_RAM_ONLY_V1' not in data:
        raise ImageError('loader RAM-only build markers are absent')
    return {'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data), 'machine': 'arm64',
            'payload_base': hex(base), 'panic_log_base': hex(panic), 'ram_only': True}

def inspect_shim(shim, contract):
    if len(shim) != 368 or shim[56:60] != b'ARM\x64':
        raise ImageError('unexpected BootShim shape')
    if struct.unpack_from('<QQ', shim, 8) != (number(contract, 'fd_base'), number(contract, 'fd_size')):
        raise ImageError('BootShim FD configuration is stale')
    values = movz_values(shim)
    if number(contract, 'payload_base') not in values or 0xF0000000 in values:
        raise ImageError('BootShim payload address is stale')

def inspect_blob(blob, contract):
    fd_size = number(contract, 'fd_size')
    if len(blob) < 368 + fd_size + 32:
        raise ImageError('missing bootstrap loader after BootShim and FD')
    shim = blob[:368]
    inspect_shim(shim, contract)
    tail = blob[368 + fd_size:]
    if tail[:4] != b'XNUP':
        raise ImageError('missing bootstrap XNUP after FD')
    total = 8 + struct.unpack_from('<I', tail, 4)[0]
    if len(tail) != aligned(total) or any(tail[total:]):
        raise ImageError('invalid bootstrap tail alignment')
    entries = parse_xnup(tail[:total], number(contract, 'payload_cap'))
    if 'loader.efi' not in entries or entries.get('usb') != b'1\0':
        raise ImageError('bootstrap requires loader.efi and usb=1')
    if 'kernel' in entries or 'ramdisk' in entries:
        raise ImageError('RAM bootstrap must not embed full kernel/ramdisk')
    loader = inspect_loader(entries['loader.efi'], contract)
    return {'uncompressed_bytes': len(blob), 'shim_bytes': len(shim), 'fd_bytes': fd_size,
            'bootstrap_bytes': total, 'bootstrap_entries': sorted(entries), 'loader': loader,
            'fd_sha256': hashlib.sha256(blob[368:368+fd_size]).hexdigest()}

def build_blob(shim, fd, bootstrap, contract):
    if len(fd) != number(contract, 'fd_size') or len(fd) % 16 or len(shim) % 16:
        raise ImageError('FD/shim size mismatch')
    blob = shim + fd + bootstrap + b'\0' * (-len(bootstrap) % 16)
    inspect_blob(blob, contract)
    return blob

def read_android_image(data):
    if len(data) < 44 or data[:8] != b'ANDROID!':
        raise ImageError('not an Android boot image')
    version = struct.unpack_from('<I', data, 40)[0]
    kernel_size = struct.unpack_from('<I', data, 8)[0]
    if version in (0, 1, 2):
        page = struct.unpack_from('<I', data, 36)[0]
        ramdisk_size = struct.unpack_from('<I', data, 16)[0]
        minimum = {0: 1632, 1: 1648, 2: 1660}[version]
    elif version in (3, 4):
        page = 4096
        ramdisk_size = struct.unpack_from('<I', data, 12)[0]
        minimum = {3: 1580, 4: 1584}[version]
    else:
        raise ImageError('unsupported Android header version')
    if page < minimum or page & (page - 1):
        raise ImageError('invalid Android page size')
    kernel_end = page + kernel_size
    ramdisk_start = aligned(kernel_end, page)
    if kernel_size == 0 or kernel_end > len(data) or ramdisk_start + ramdisk_size > len(data):
        raise ImageError('truncated Android image')
    return version, data[page:kernel_end]

def inspect_boot_image(path, contract, expected_dtb=None):
    raw = Path(path).read_bytes()
    version, kernel = read_android_image(raw)
    inflater = zlib.decompressobj(16 + zlib.MAX_WBITS)
    try:
        blob = inflater.decompress(kernel) + inflater.flush()
    except zlib.error as exc:
        raise ImageError('bad kernel gzip') from exc
    if not inflater.eof:
        raise ImageError('incomplete kernel gzip')
    suffix = inflater.unused_data
    if version in (1, 2) and expected_dtb is not None and suffix != expected_dtb:
        raise ImageError('appended device tree mismatch')
    if version in (3, 4) and suffix:
        raise ImageError('v3/v4 must use embedded/vendor DTB, not appended DTB')
    result = inspect_blob(blob, contract)
    result.update({'sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(raw),
                   'header_version': version, 'appended_dtb_bytes': len(suffix)})
    return result

def build_android_image(blob, dtb=b'', version=1, ramdisk=b''):
    kernel = gzip.compress(blob, compresslevel=6, mtime=0) + (dtb if version == 1 else b'')
    if version == 1:
        page = 2048
        header = bytearray(page)
        header[:8] = b'ANDROID!'
        struct.pack_into('<10I', header, 8, len(kernel), 0x10008000, len(ramdisk), 0x11000000,
                         0, 0x10F00000, 0x10000100, page, version, 0)
        header[48:64] = b'socrates'.ljust(16, b'\0')
        checksum = hashlib.sha1()
        for part in (kernel, ramdisk, b'', b''):
            checksum.update(part)
            checksum.update(struct.pack('<I', len(part)))
        header[576:608] = checksum.digest().ljust(32, b'\0')
        struct.pack_into('<IQI', header, 1632, 0, 0, 1648)
    elif version == 3:
        page = 4096
        header = bytearray(page)
        header[:8] = b'ANDROID!'
        struct.pack_into('<4I', header, 8, len(kernel), len(ramdisk), 0, 1580)
        struct.pack_into('<I', header, 40, version)
    else:
        raise ImageError('writer supports header v1 and v3')
    return bytes(header) + kernel + b'\0' * (-len(kernel) % page) + ramdisk + b'\0' * (-len(ramdisk) % page)
