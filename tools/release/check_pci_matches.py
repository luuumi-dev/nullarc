#!/usr/bin/env python3
"""Reject release kexts whose PCI matches differ from the checked-out sources."""
import plistlib
from pathlib import Path
import sys


def pci_matches(path):
    with Path(path).open('rb') as file:
        personalities = plistlib.load(file)['IOKitPersonalities']
    return {name: tuple(sorted((personality.get('IOPCIMatch') or
                               personality.get('IOPCIPrimaryMatch', '')).lower().split()))
            for name, personality in personalities.items()
            if personality.get('IOProviderClass') == 'IOPCIDevice'}


def check(repo, package):
    for component, destination in (
        ('phxbringup', 'EFI/OC/Kexts/PhxBringup.kext'),
        ('phxaccel', 'Library/Extensions/PhxAccel.kext'),
    ):
        expected = pci_matches(Path(repo) / 'src' / component / 'Info.plist')
        actual = pci_matches(Path(package) / destination / 'Contents/Info.plist')
        if not expected or any(not matches for matches in expected.values()) or actual != expected:
            raise ValueError(f'{destination}: PCI matches differ from source; rebuild the kext before packaging')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('usage: check_pci_matches.py <repo> <package>')
    try:
        check(*sys.argv[1:])
    except (OSError, KeyError, ValueError, plistlib.InvalidFileException) as error:
        sys.exit(str(error))
