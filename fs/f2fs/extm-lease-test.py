#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Negative tests ONLY on an explicitly disposable, already ACTIVE lease.

Never creates a target, mounts, freezes, resizes or accesses a block device.
A missing lease can MODIFY the supplied file: do not use real /data here.
Run: python3 -I extm-lease-test.py --disposable-active-lease /test/file
"""
import argparse
import errno
import fcntl
import mmap
import os

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--disposable-active-lease', required=True)
args = parser.parse_args()
path = os.path.abspath(args.disposable_active_lease)
if path.startswith('/data/'):
    parser.error('real /data paths are deliberately prohibited')


def rejected(name, operation, accepted):
    try:
        result = operation()
    except OSError as error:
        if error.errno not in accepted:
            raise AssertionError((name, error)) from error
        print('PASS', name, errno.errorcode[error.errno])
        return
    if hasattr(result, 'close'):
        result.close()
    elif isinstance(result, int) and name == 'open writable':
        os.close(result)
    raise AssertionError(name + ' was not rejected; file may not have an ACTIVE lease')


fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC)
try:
    rejected('open writable', lambda: os.open(path, os.O_WRONLY), {errno.ETXTBSY})
    rejected('truncate', lambda: os.truncate(path, 0), {errno.EBUSY, errno.ETXTBSY})
    rejected('read', lambda: os.read(fd, 4096), {errno.EBUSY})
    rejected('mmap read', lambda: mmap.mmap(fd, 4096, access=mmap.ACCESS_READ), {errno.EBUSY})
    # GETFLAGS intentionally also blocked: all per-file ioctls are gated.
    rejected('ioctl GETFLAGS', lambda: fcntl.ioctl(fd, 0x80086601, bytes(8)), {errno.EBUSY})
    read_pipe, write_pipe = os.pipe()
    try:
        rejected('sendfile/splice read', lambda: os.sendfile(write_pipe, fd, 0, 4096), {errno.EBUSY})
    finally:
        os.close(read_pipe)
        os.close(write_pipe)
finally:
    os.close(fd)
