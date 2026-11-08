"""Control tool for c3coop.dll inside Crysis3Remastered.exe.

  python scripts/c3ctl.py ps                      list game processes
  python scripts/c3ctl.py inject [--pid N]        inject build/c3coop.dll (copied to a unique name first)
  python scripts/c3ctl.py exec "map jailbreak s" [--pid N] [--wait 5]
  python scripts/c3ctl.py tail [--pid N] [--wait 30]
  python scripts/c3ctl.py launch [game args...]   start the game through Steam
  python scripts/c3ctl.py start [game args...]    launch, wait for the process and inject right away
  python scripts/c3ctl.py host [level] [players] [dedicated]
                                                  start, inject, enlarge net id pools, `map <level> s` (listen server)
  python scripts/c3ctl.py client [connect=HOST] [forcesp] [game args...]
                                                  second instance on this PC (exe + -devmode, prompt auto-answered)
"""
import argparse
import ctypes
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from ctypes import wintypes

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLL = os.path.join(ROOT, 'build', 'c3coop.dll')
PAK = os.path.join(ROOT, 'build', 'c3coop.pak')  # scripts/build_pak.py
INJECT_DIR = os.path.join(ROOT, 'local', 'inject')
EXE = 'Crysis3Remastered.exe'
APP_ID = '2096610'


def steam_dir():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam') as key:
            return os.path.normpath(winreg.QueryValueEx(key, 'SteamPath')[0])
    except OSError:
        return r'C:\Program Files (x86)\Steam'


def game_dir():
    """C3_GAME_DIR, else the Steam library that has the game (steamapps/libraryfolders.vdf)."""
    if os.environ.get('C3_GAME_DIR'):
        return os.environ['C3_GAME_DIR']
    steam = steam_dir()
    libraries = [steam]
    try:
        with open(os.path.join(steam, 'steamapps', 'libraryfolders.vdf'), encoding='utf-8') as f:
            libraries += [p.replace('\\\\', '\\') for p in re.findall(r'"path"\s+"([^"]+)"', f.read())]
    except OSError:
        pass
    for library in libraries:
        path = os.path.join(library, 'steamapps', 'common', 'Crysis3Remastered')
        if os.path.isdir(path):
            return path
    return os.path.join(steam, 'steamapps', 'common', 'Crysis3Remastered')


STEAM = os.path.join(steam_dir(), 'steam.exe')
GAME_DIR = game_dir()

k32 = ctypes.WinDLL('kernel32', use_last_error=True)


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD), ('th32ProcessID', wintypes.DWORD),
                ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wintypes.DWORD),
                ('cntThreads', wintypes.DWORD), ('th32ParentProcessID', wintypes.DWORD),
                ('pcPriClassBase', ctypes.c_long), ('dwFlags', wintypes.DWORD), ('szExeFile', ctypes.c_wchar * 260)]


k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
k32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
k32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
k32.OpenProcess.restype = wintypes.HANDLE
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.VirtualAllocEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD]
k32.WriteProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.POINTER(ctypes.c_size_t)]
k32.GetModuleHandleW.restype = wintypes.HMODULE
k32.GetProcAddress.restype = ctypes.c_void_p
k32.GetProcAddress.argtypes = [wintypes.HMODULE, ctypes.c_char_p]
k32.CreateRemoteThread.restype = wintypes.HANDLE
k32.CreateRemoteThread.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                   ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p]
k32.GetExitCodeThread.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]


def game_pids():
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    pids = []
    ok = k32.Process32FirstW(snap, ctypes.byref(entry))
    while ok:
        if entry.szExeFile.lower() == EXE.lower():
            pids.append(entry.th32ProcessID)
        ok = k32.Process32NextW(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return pids


def pick_pid(pid):
    if pid:
        return pid
    pids = game_pids()
    if not pids:
        sys.exit(f'{EXE} is not running')
    if len(pids) > 1:
        sys.exit(f'several game processes running, pass --pid: {pids}')
    return pids[0]


def inject(pid):
    os.makedirs(INJECT_DIR, exist_ok=True)
    # unique copy so the build output is never locked by a running game
    path = os.path.join(INJECT_DIR, f'c3coop_{pid}_{int(time.time())}.dll')
    shutil.copy2(DLL, path)
    if os.path.exists(PAK):
        shutil.copy2(PAK, path[:-4] + '.pak')  # the dll mounts <its name>.pak over C3/
    proc = k32.OpenProcess(0x1F0FFF, False, pid)
    if not proc:
        sys.exit(f'OpenProcess failed: {ctypes.get_last_error()}')
    data = ctypes.create_unicode_buffer(path)
    size = ctypes.sizeof(data)
    remote = k32.VirtualAllocEx(proc, None, size, 0x3000, 0x04)
    k32.WriteProcessMemory(proc, remote, data, size, None)
    load = k32.GetProcAddress(k32.GetModuleHandleW('kernel32.dll'), b'LoadLibraryW')
    thread = k32.CreateRemoteThread(proc, None, 0, load, remote, 0, None)
    if not thread:
        sys.exit(f'CreateRemoteThread failed: {ctypes.get_last_error()}')
    k32.WaitForSingleObject(thread, 10000)
    code = wintypes.DWORD()
    k32.GetExitCodeThread(thread, ctypes.byref(code))
    k32.CloseHandle(thread)
    k32.CloseHandle(proc)
    print(f'injected {path} into {pid}, module handle (low 32 bits) {code.value:#x}')
    if not code.value:
        sys.exit('LoadLibraryW returned NULL')


def open_pipe(pid, timeout=10):
    name = rf'\\.\pipe\c3coop_{pid}'
    end = time.time() + timeout
    while True:
        try:
            return open(name, 'r+b', buffering=0)
        except OSError:
            if time.time() > end:
                sys.exit(f'cannot open {name} (is the DLL injected?)')
            time.sleep(0.2)


def stream(pipe, seconds, command=None):
    def reader():
        buf = b''
        while True:
            try:
                chunk = pipe.read(4096)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            *lines, buf = buf.split(b'\n')
            for line in lines:
                print(line.decode('utf-8', 'replace'), flush=True)

    # Synchronous pipe handles serialize I/O: a pending ReadFile would block this write, so write first.
    if command:
        pipe.write(command.encode() + b'\n')
    threading.Thread(target=reader, daemon=True).start()
    time.sleep(seconds)
    sys.stdout.flush()
    os._exit(0)  # the reader is still blocked in ReadFile


def run(pid, command, wait=2.0):
    """Send one command through a fresh `exec` process and return its output lines."""
    out = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), 'exec', command, '--pid', str(pid),
                          '--wait', str(wait)], capture_output=True, text=True, encoding='utf-8', errors='replace')
    return out.stdout.splitlines()


def wait_ready(pid, timeout=180):
    end = time.time() + timeout
    while time.time() < end:
        if pid not in game_pids():
            sys.exit(f'game process {pid} exited')
        if any('pong' in line for line in run(pid, '!ping', 1)):
            return
        time.sleep(3)
    sys.exit(f'game {pid} did not become ready')


user32 = ctypes.WinDLL('user32', use_last_error=True)
WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]


def answer_too_many_apps(pid, timeout=60):
    """Click "Yes" on the game's "Too many apps / There is already a Crytek application running" prompt."""
    end = time.time() + timeout
    while time.time() < end:
        found = []

        def check(hwnd, _):
            owner = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            title = ctypes.create_unicode_buffer(64)
            user32.GetWindowTextW(hwnd, title, 64)
            if owner.value == pid and title.value == 'Too many apps':
                found.append(hwnd)
            return True

        user32.EnumWindows(WNDENUMPROC(check), 0)
        if found:
            user32.PostMessageW(found[0], 0x0111, 6, 0)  # WM_COMMAND, IDYES
            print(f'answered "Too many apps" prompt for {pid}')
            return
        time.sleep(0.2)


def prepare_session(pid):
    # campaign levels need far more net objects than MP maps
    for line in run(pid, '!netids 8464', 1):
        if 'net id pool' in line:
            print(line)
    if not any('c3coop.pak is mounted' in line for line in run(pid, 'exec c3coop.cfg', 2)):
        print('warning: c3coop.pak is not mounted (run scripts/build_pak.py)')
    # extra console commands before the level loads, e.g. C3COOP_PRE="fg_inspectorLog 1;fg_Inspector 1"
    for command in filter(None, (c.strip() for c in os.environ.get('C3COOP_PRE', '').split(';'))):
        run(pid, command, 0.5)


def launch_direct(game_args):
    """Run the exe directly (Steam must be running). SteamAppId/SteamGameId stop steam_api from relaunching through
    Steam, so the user's Steam launch options are not applied; -devmode turns the single-instance block into the
    "Too many apps" prompt, which is answered automatically (second instance on one PC)."""
    env = dict(os.environ, SteamAppId=APP_ID, SteamGameId=APP_ID)
    bin_dir = os.path.join(GAME_DIR, 'Bin64')
    proc = subprocess.Popen([os.path.join(bin_dir, EXE), '-devmode', '+g_skipIntro', '1', *game_args], cwd=bin_dir,
                            env=env)
    threading.Thread(target=answer_too_many_apps, args=(proc.pid,), daemon=True).start()
    time.sleep(1)
    inject(proc.pid)
    return proc.pid


def host(level, players, dedicated=False):
    pid = launch_direct(['+r_Width', '1280', '+r_Height', '720', '+log_Verbosity', '4'])
    wait_ready(pid)
    time.sleep(3)  # let the front end settle
    prepare_session(pid)
    run(pid, f'sv_maxplayers {players}', 1)
    # listen server: the host plays (SP flowgraph needs a local player); dedicated has no local player
    run(pid, f'map {level} s' + (' dedicated' if dedicated else ''), 1)
    started = time.time()
    while time.time() - started < 180:
        if pid not in game_pids():
            sys.exit('host died while loading the level (see local/logs)')
        status = run(pid, 'status', 1)
        if any('Server Status' in line for line in status) and any(f'level: {level}' in line for line in status):
            break
        time.sleep(2)
    else:
        sys.exit('level did not finish loading in 180 s')
    print(f'level {level} up in {time.time() - started:.0f} s')
    print(f'host pid {pid}; clients connect to <this PC>:' + ('64090' if dedicated else '64087 (sv_port)'))


def client(connect, game_args, force_sp=False):
    # Second instance on the same PC: run the exe directly. SteamAppId/SteamGameId stop steam_api from
    # relaunching through Steam (which refuses while the first copy runs); -devmode turns the
    # single-instance block into the "Too many apps" prompt, which is answered automatically.
    pid = launch_direct(game_args)
    wait_ready(pid)
    time.sleep(3)
    prepare_session(pid)
    if force_sp:
        print('\n'.join(l for l in run(pid, '!forcesp on', 1) if 'forcesp' in l))
    if connect:
        host, _, port = connect.partition(':')
        for line in run(pid, f'connect {host} {port or 64087}', 5):
            if any(k in line for k in ('connection requested', 'Game rules', 'Failed connecting', 'timed out')):
                print(line)
    print(f'client pid {pid}')


def main():
    if len(sys.argv) > 1 and sys.argv[1] == 'host':
        # host [level] [players] [dedicated]
        args = sys.argv[2:]
        dedicated = 'dedicated' in args
        args = [a for a in args if a != 'dedicated']
        host(args[0] if args else 'Jailbreak', int(args[1]) if len(args) > 1 else 4, dedicated)
        return
    if len(sys.argv) > 1 and sys.argv[1] == 'client':
        # client [connect=HOST[:PORT]] [forcesp] [game args...]  (listen server: 64087 = sv_port)
        rest = sys.argv[2:]
        target = rest.pop(0)[len('connect='):] if rest and rest[0].startswith('connect=') else None
        force_sp = bool(rest) and rest[0] == 'forcesp'
        if force_sp:
            rest.pop(0)
        client(target, rest or ['+r_Width', '1280', '+r_Height', '720', '+log_Verbosity', '4'], force_sp)
        return
    if len(sys.argv) > 1 and sys.argv[1] in ('launch', 'start'):
        # game args may start with '-' (e.g. -devmode), so pass them through untouched
        before = set(game_pids())
        subprocess.Popen([STEAM, '-applaunch', APP_ID, *sys.argv[2:]])
        if sys.argv[1] == 'start':
            # inject early: the Leech crash workaround must be in place before its Steam callback fires
            end = time.time() + 60
            while not (new := set(game_pids()) - before):
                if time.time() > end:
                    sys.exit('game did not start')
                time.sleep(0.5)
            time.sleep(3)
            inject(new.pop())
        return
    ap = argparse.ArgumentParser()
    ap.add_argument('action', choices=['ps', 'inject', 'exec', 'tail'])
    ap.add_argument('rest', nargs='*')
    ap.add_argument('--pid', type=int)
    ap.add_argument('--wait', type=float, default=3)
    args = ap.parse_intermixed_args()

    if args.action == 'ps':
        print(game_pids())
    elif args.action == 'inject':
        inject(pick_pid(args.pid))
    elif args.action == 'exec':
        stream(open_pipe(pick_pid(args.pid)), args.wait, ' '.join(args.rest))
    elif args.action == 'tail':
        stream(open_pipe(pick_pid(args.pid)), args.wait)


if __name__ == '__main__':
    main()
