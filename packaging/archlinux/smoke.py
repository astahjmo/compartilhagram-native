"""Exercise the shipped AppImage on Arch without any host desktop mounts."""
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time


results = Path('/results')
results.mkdir(exist_ok=True)
(results / 'result.json').write_text('{"status": "incomplete"}\n')
work = Path(tempfile.mkdtemp(prefix='compartilhagram-arch-'))
runtime = work / 'runtime'
runtime.mkdir(mode=0o700)
os.environ.update(HOME=str(work), XDG_RUNTIME_DIR=str(runtime),
                  COMPARTILHAGRAM_DISABLE_GPU='1', QT_DEBUG_PLUGINS='1')
app = '/input/Compartilhagram-x86_64.AppImage'


def stop(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()


def smoke(platform):
    env = os.environ.copy()
    env.update(APPIMAGE_EXTRACT_AND_RUN='1', QT_QPA_PLATFORM=platform)
    if platform == 'wayland':
        env.update(WAYLAND_DISPLAY='test-wayland', WAYLAND_DEBUG='client',
                   QT_QUICK_BACKEND='software')
    log_path = results / f'{platform}.log'
    with log_path.open('w') as log:
        process = subprocess.Popen([app], cwd=work, env=env, stdout=log,
                                   stderr=log, start_new_session=True)
        try:
            try:
                code = process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                code = None
            if code is not None:
                raise RuntimeError(f'{platform}: exited early ({code}); see {log_path}')
            text = log_path.read_text()
            plugin = 'libqwayland-generic.so' if platform == 'wayland' else 'libqoffscreen.so'
            if not re.search(r'/tmp/appimage_extracted_[^\n]*' + re.escape(plugin) + r'" loaded library', text):
                raise RuntimeError(f'{platform}: bundled platform plugin did not load')
            if platform == 'wayland':
                if not re.search(r'xdg_surface.*ack_configure', text) or not re.search(r'wl_surface.*attach\(wl_buffer', text):
                    raise RuntimeError('Wayland: no configured, rendered window found in protocol log')
        finally:
            stop(process)
    print(f'PASS: {platform} startup using the bundled platform plugin', flush=True)


packages = subprocess.check_output(['pacman', '-Q'], text=True)
(results / 'packages.txt').write_text(packages)
if any(line.startswith(('qt5-', 'qt6-', 'ffmpeg ', 'libpulse ', 'pulseaudio ', 'pipewire '))
       for line in packages.splitlines()):
    raise RuntimeError('Test container must not contain system application dependencies')
with (results / 'extract.log').open('w') as log:
    subprocess.run([app, '--appimage-extract'], cwd=work, stdout=log, stderr=log, check=True)
with (results / 'dependencies.log').open('w') as log:
    subprocess.run(['python3', '/opt/test/verify-appdir.py', str(work / 'squashfs-root')],
                   stdout=log, stderr=log, check=True)
print('PASS: bundled ELF dependency verification; no system Qt installed', flush=True)
smoke('offscreen')
with (results / 'weston.log').open('w') as log:
    weston = subprocess.Popen(['weston', '--backend=headless', '--renderer=pixman',
                               '--socket=test-wayland', '--idle-time=0', '--no-config'],
                              stdout=log, stderr=log, start_new_session=True)
    try:
        deadline = time.monotonic() + 15
        while not (runtime / 'test-wayland').exists():
            if weston.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('Headless Weston failed; see weston.log')
            time.sleep(0.1)
        smoke('wayland')
    finally:
        stop(weston)
(results / 'result.json').write_text(json.dumps({
    'status': 'passed', 'distribution': 'Arch Linux', 'system_qt': False,
    'checks': ['ELF dependencies', 'offscreen startup', 'Wayland window rendering'],
    'limitations': ['No GPU, screen-capture portal, audio server, or production session tested'],
}, indent=2) + '\n')
