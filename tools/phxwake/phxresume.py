#!/usr/bin/env python3
"""phxresume: wake from hibernation straight into OpenCore when a boot menu comes first.

A wake from hibernation (the only sleep on Phoenix laptops, docs/SLEEP.md) is a cold boot. When a menu (Limine,
systemd-boot, GRUB, rEFInd) comes before OpenCore, it shows up on every wake and the resuming OpenCore has to be picked
by hand. Both ways of telling the next boot where to go are one-shot: they apply once and are dropped.

  bli       The Boot Loader Interface (systemd's spec; Limine and systemd-boot implement it). The menu reports itself in
            LoaderInfo and the entry it started in LoaderEntrySelected; LoaderEntryOneShot picks the entry of the next
            boot and LoaderConfigTimeoutOneShot="menu-disabled" skips the menu once (a one-shot "0"
            would instead show the menu with no timeout). Used whenever the running boot came through such
            a menu. setup records the entry this macOS was started from (it has to be the resuming one).
  bootnext  UEFI BootNext to a firmware boot option (Boot####, not put into BootOrder) for the OpenCore whose config has
            HibernateMode other than None. Only for menus without the interface, and only when OpenCore's
            UEFI > Quirks > RequestBootVarRouting is off: with it on, OpenCore moves every Boot* variable macOS writes
            into its own namespace (so that Startup Disk cannot reorder the firmware's options), and the firmware never
            sees them.

setup writes /usr/local/etc/phxwake-resume ("bli <entry id>" or "bootnext <####>"); phxwaked sets the one-shot when the
system goes to sleep (only when PhxSleepPolicy makes that sleep a hibernation) and deletes it after the wake or an
aborted sleep. status shows it, remove undoes it. Run as root. With OpenCore first, none of this is needed.
"""
import argparse, os, plistlib, re, struct, subprocess, sys, uuid

GLOBAL = '8BE4DF61-93CA-11D2-AA0D-00E098032B8C'
LOADER = '4A67B082-0A4C-41CF-B6C7-440B29BB8C4F'      # systemd Boot Loader Interface vendor GUID
CONF = '/usr/local/etc/phxwake-resume'
DESC = 'macOS hibernation resume (nullarc)'
LOAD_OPTION_ACTIVE = 0x1


def sh(*a, check=True):
    r = subprocess.run(a, capture_output=True, text=True)
    if check and r.returncode:
        sys.exit(f'!! {" ".join(a)}: {r.stderr.strip() or r.stdout.strip()}')
    return r.stdout


def nvram_get(guid, name):
    r = subprocess.run(['nvram', '-x', f'{guid}:{name}'], capture_output=True)
    if r.returncode:
        return None
    return next(iter(plistlib.loads(r.stdout).values()))


def nvram_set(guid, name, data):
    sh('nvram', f'{guid}:{name}=' + ''.join(f'%{b:02x}' for b in data))


def nvram_del(guid, name):
    sh('nvram', '-d', f'{guid}:{name}', check=False)


def u16(v):
    return v.decode('utf-16-le').split('\0')[0] if v else None


def hibernating_boot():
    r = subprocess.run(['sysctl', '-n', 'debug.phxsleep_installed'], capture_output=True, text=True)
    return r.stdout.strip() == '1'


# ---- bootnext: firmware boot option for the resuming OpenCore ----
def boot_disk_esp():
    """The ESP on the disk that holds the booted APFS container."""
    info = plistlib.loads(subprocess.run(['diskutil', 'info', '-plist', '/'], capture_output=True).stdout)
    cont = info.get('APFSContainerReference') or info['ParentWholeDisk']
    stores = plistlib.loads(subprocess.run(['diskutil', 'apfs', 'list', '-plist', cont], capture_output=True).stdout)
    phys = stores['Containers'][0]['PhysicalStores'][0]['DeviceIdentifier']
    whole = re.match(r'(disk\d+)', phys).group(1)
    lst = plistlib.loads(subprocess.run(['diskutil', 'list', '-plist', whole], capture_output=True).stdout)
    for d in lst['AllDisksAndPartitions']:
        for p in d.get('Partitions', []):
            if p.get('Content') == 'EFI':
                return p['DeviceIdentifier']
    sys.exit(f'!! no EFI partition on {whole}')


def partition(dev):
    i = plistlib.loads(subprocess.run(['diskutil', 'info', '-plist', dev], capture_output=True).stdout)
    bs = i.get('DeviceBlockSize', 512)
    num = int(re.search(r's(\d+)$', dev).group(1))
    return dict(num=num, start=i['PartitionMapPartitionOffset'] // bs, size=i['TotalSize'] // bs,
                guid=uuid.UUID(i['DiskUUID']), mount=i.get('MountPoint') or '')


def find_loaders(dev):
    """\\EFI\\*\\OpenCore.efi whose config.plist resumes from hibernation (HibernateMode != None)."""
    mounted_here = False
    mnt = partition(dev)['mount']
    if not mnt:
        sh('diskutil', 'mount', 'readOnly', dev); mounted_here = True
        mnt = partition(dev)['mount']
    try:
        found = []
        efi = os.path.join(mnt, 'EFI')
        for d in sorted(os.listdir(efi)):
            cfg = os.path.join(efi, d, 'config.plist')
            if not (os.path.isfile(os.path.join(efi, d, 'OpenCore.efi')) and os.path.isfile(cfg)):
                continue
            try:
                p = plistlib.load(open(cfg, 'rb'))
            except Exception:
                continue
            if p.get('Misc', {}).get('Boot', {}).get('HibernateMode', 'None') != 'None':
                found.append((f'\\EFI\\{d}\\OpenCore.efi', bool(p.get('UEFI', {}).get('Quirks', {}).get('RequestBootVarRouting'))))
        return found
    finally:
        if mounted_here:
            sh('diskutil', 'unmount', dev, check=False)


def load_option(part, path):
    hd = struct.pack('<BBH', 4, 1, 42) + struct.pack('<IQQ', part['num'], part['start'], part['size']) \
        + part['guid'].bytes_le + bytes([2, 2])                                            # GPT, GUID signature
    f = (path + '\0').encode('utf-16-le')
    dp = hd + struct.pack('<BBH', 4, 4, 4 + len(f)) + f + bytes([0x7F, 0xFF, 4, 0])
    return struct.pack('<IH', LOAD_OPTION_ACTIVE, len(dp)) + (DESC + '\0').encode('utf-16-le') + dp


def parse_option(v):
    attr, fpl = struct.unpack_from('<IH', v)
    i = 6
    while v[i:i + 2] != b'\0\0':
        i += 2
    desc = v[6:i].decode('utf-16-le')
    dp = v[i + 2:i + 2 + fpl]
    paths, j = [], 0
    while j + 4 <= len(dp):                                 # walk the device path nodes; keep the file paths
        t, st, ln = struct.unpack_from('<BBH', dp, j)
        if ln < 4 or t == 0x7F:
            break
        if (t, st) == (4, 4):
            paths.append(dp[j + 4:j + ln].decode('utf-16-le').rstrip('\0'))
        j += ln
    return attr, desc, paths


def boot_options():
    order = nvram_get(GLOBAL, 'BootOrder') or b''
    nums = {n for n in struct.unpack(f'<{len(order) // 2}H', order)} | set(range(0x10)) | set(range(0x80, 0x100))
    return {f'{n:04X}': v for n in sorted(nums) if (v := nvram_get(GLOBAL, f'Boot{n:04X}'))}


def setup_bootnext(a):
    esp = boot_disk_esp(); part = partition(esp)
    c = find_loaders(esp)
    if a.loader:
        c = [x for x in c if x[0].lower() == a.loader.lower()] or [(a.loader, False)]
    if len(c) != 1:
        sys.exit(f'!! {len(c)} OpenCore with hibernation on {esp}: {[x[0] for x in c] or "none"}. Give one with --loader.')
    loader, routed = c[0]
    if routed:
        sys.exit(f'!! {loader}: RequestBootVarRouting is on, so the firmware would never see BootNext written from macOS.')
    opt = load_option(part, loader)
    opts = boot_options()
    num = next((n for n, v in opts.items() if v == opt), None)
    if num is None:
        num = next(f'{n:04X}' for n in range(0x0090, 0x0100) if f'{n:04X}' not in opts)
        nvram_set(GLOBAL, f'Boot{num}', opt)
        if nvram_get(GLOBAL, f'Boot{num}') != opt:
            sys.exit(f'!! Boot{num} did not read back as written')
    return f'bootnext {num}', f'Boot{num} "{DESC}" -> {esp} {loader}'


# ---- commands ----
def cmd_setup(a):
    if not hibernating_boot():
        sys.exit('!! this boot does not hibernate (debug.phxsleep_installed != 1): run setup from the resuming entry')
    loader = u16(nvram_get(LOADER, 'LoaderInfo'))
    mode = a.mode or ('bli' if loader else 'bootnext')
    if mode == 'bli':
        entry = a.entry or u16(nvram_get(LOADER, 'LoaderEntrySelected'))
        if not loader or not entry:
            sys.exit('!! no Boot Loader Interface menu reported this boot (LoaderInfo / LoaderEntrySelected missing)')
        feat = nvram_get(LOADER, 'LoaderFeatures')
        f = int.from_bytes(feat[:8], 'little') if feat else 0
        if not (f & 0x8):
            sys.exit(f'!! {loader} does not support LoaderEntryOneShot (LoaderFeatures {f:#x})')
        rec, what = f'bli {entry}', f'{loader}: entry "{entry}" once, menu skipped' + ('' if f & 0x2 else ' (no one-shot timeout: menu still shown)')
    else:
        rec, what = setup_bootnext(a)
    os.makedirs(os.path.dirname(CONF), exist_ok=True)
    open(CONF, 'w').write(rec + '\n')
    print(f'{what}; recorded in {CONF}. phxwaked sets it on each hibernation.')


def cmd_status(a):
    rec = open(CONF).read().strip() if os.path.exists(CONF) else None
    print('record:', rec or 'none')
    print('menu:', u16(nvram_get(LOADER, 'LoaderInfo')) or '-', '| started entry:', u16(nvram_get(LOADER, 'LoaderEntrySelected')) or '-')
    print('one-shot now: LoaderEntryOneShot', repr(u16(nvram_get(LOADER, 'LoaderEntryOneShot'))),
          '| BootNext', (nvram_get(GLOBAL, 'BootNext') or b'').hex() or '-')
    if a.verbose:
        for n, v in boot_options().items():
            attr, desc, paths = parse_option(v)
            print(f'  Boot{n} attr={attr:#x} {desc!r} {paths}')


def cmd_remove(a):
    if os.path.exists(CONF):
        rec = open(CONF).read().split()
        if rec and rec[0] == 'bootnext':
            v = nvram_get(GLOBAL, f'Boot{rec[1]}')
            if v and parse_option(v)[1] == DESC:
                nvram_del(GLOBAL, f'Boot{rec[1]}'); print(f'Boot{rec[1]} deleted')
        os.remove(CONF)
    for g, n in ((LOADER, 'LoaderEntryOneShot'), (LOADER, 'LoaderConfigTimeoutOneShot'), (GLOBAL, 'BootNext')):
        nvram_del(g, n)
    print('removed')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('setup'); s.add_argument('--mode', choices=['bli', 'bootnext'])
    s.add_argument('--entry', help='bli: menu entry id (default: the one this boot came from)')
    s.add_argument('--loader', help="bootnext: '\\EFI\\X\\OpenCore.efi' (default: the one with HibernateMode)")
    st = sub.add_parser('status'); st.add_argument('-v', '--verbose', action='store_true')
    sub.add_parser('remove')
    a = ap.parse_args()
    if a.cmd != 'status' and os.geteuid():
        sys.exit('run as root')
    {'setup': cmd_setup, 'status': cmd_status, 'remove': cmd_remove}[a.cmd](a)


if __name__ == '__main__':
    main()
