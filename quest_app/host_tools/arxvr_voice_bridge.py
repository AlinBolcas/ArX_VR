"""Local Quest dictation over USB, using OpenWispr's existing whisper.cpp model."""
from array import array
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from datetime import datetime, timezone
import queue
import hmac
import io
import json
import math
import os
import re
import secrets
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import wave

PORT = 47999
VIDEO_PORT = 47997
MIRROR_PORT = 47996
# Extra desktops beyond the main one; the Mac companion's ARX_VIDEO_MAX_SLOTS is this plus one
MAX_DESKTOPS = 4
# One dictation, sent whole when you stop talking: up to four hours
MAX_SECONDS = 14400
MAX_BYTES = MAX_SECONDS * 16000 * 2 + 44
PACKAGE = 'ai.arvolve.arxvr'
MARKERS = {'blank_audio', 'music', 'applause', 'laughter', 'silence', 'sound', 'noise', 'inaudible'}


def mac_lan_address():
    # The headset streams from this address; handed over so a new DHCP lease is not a dead end
    for interface in ('en0', 'en1', 'en2'):
        try:
            address = subprocess.check_output(['ipconfig', 'getifaddr', interface], text=True, timeout=2).strip()
            if re.fullmatch(r'\d{1,3}(\.\d{1,3}){3}', address): return address
        except (subprocess.SubprocessError, OSError): pass
    return ''




def validate_audio(data):
    if not 44 <= len(data) <= MAX_BYTES:
        raise ValueError('Recording size is outside the supported range')
    try:
        with wave.open(io.BytesIO(data), 'rb') as source:
            if (source.getnchannels(), source.getsampwidth(), source.getframerate(), source.getcomptype()) != (1, 2, 16000, 'NONE'):
                raise ValueError('Expected mono 16 kHz PCM16 WAV')
            count = source.getnframes()
            if count < 1600 or count > MAX_SECONDS * 16000:
                raise ValueError(f'Record between 0.1 and {MAX_SECONDS} seconds')
            pcm = source.readframes(count)
            if len(pcm) != count * 2:
                raise ValueError('Recording transfer is incomplete')
    except (wave.Error, EOFError) as error:
        raise ValueError('Invalid WAV recording') from error
    samples = array('h', pcm)
    if sys.byteorder != 'little': samples.byteswap()
    # Every 16th sample is plenty to tell silence from speech, and keeps hours of audio quick
    samples = samples[::16]
    rms = math.sqrt(sum(x * x for x in samples) / len(samples)) / 32768
    return rms


def clean_text(value):
    value = re.sub(r'[\[(]\s*([^\]\)]+?)\s*[\])]',
        lambda m: '' if m.group(1).lower() in MARKERS else m.group(0), value)
    return ' '.join(value.split())


class LocalWhisper:
    def __init__(self):
        config_dir = Path.home() / '.config/open-wispr'
        # OpenWispr's settings when it is installed; plain whisper.cpp defaults otherwise
        config_file = config_dir / 'config.json'
        config = json.loads(config_file.read_text()) if config_file.is_file() else {}
        self.model_name = config.get('modelSize', 'small.en')
        if not re.fullmatch(r'[a-zA-Z0-9_.-]+', self.model_name):
            raise ValueError('Invalid model name in OpenWispr configuration')
        # OpenWispr's model first, then the one arxvr.py downloads for the bridge itself
        own = Path.home() / 'Library/Application Support/ArX VR Bridge/models'
        candidates = [folder / f'ggml-{self.model_name}.bin' for folder in
                      (config_dir / 'models', own, Path('/opt/homebrew/share/whisper-cpp/models'))]
        self.model = next((p for p in candidates if p.is_file()), None)
        self.binary = shutil.which('whisper-cli') or shutil.which('whisper-cpp')
        if not self.binary and Path('/opt/homebrew/bin/whisper-cli').is_file(): self.binary = '/opt/homebrew/bin/whisper-cli'
        # Dictation is optional: the bridge still runs, and the headset is told why it cannot dictate
        self.missing = None if self.binary and self.model else \
            f'Dictation is not set up on the Mac: run arxvr.py, "Set up local dictation"'
        if self.missing: print(json.dumps({'event': 'dictation_off', 'reason': self.missing}), flush=True)
        self.language = config.get('language', 'en')
        self.prompt = config.get('whisperPrompt')
        self.busy = threading.Lock()
        self.process_lock = threading.Lock()
        self.process = None
        self.stopping = False

    def transcribe(self, audio):
        if self.missing: raise LookupError(self.missing)
        if validate_audio(audio) < 0.002: return ''
        if not self.busy.acquire(blocking=False): raise BlockingIOError('Transcription is already running')
        started = time.monotonic()
        try:
            with tempfile.TemporaryDirectory(prefix='arxvr-dictation-') as directory:
                recording = Path(directory) / 'recording.wav'
                recording.write_bytes(audio)
                recording.chmod(0o600)
                args = [self.binary, '-m', str(self.model), '-f', str(recording), '-l', self.language, '--no-timestamps', '-nt']
                if self.prompt: args += ['--prompt', self.prompt]
                with self.process_lock:
                    if self.stopping: raise RuntimeError('Bridge is stopping')
                    self.process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    process = self.process
                try:
                    # Proportional to the recording, since one dictation can be long
                    stdout, _ = process.communicate(timeout=90 + len(audio) / 32000)
                except subprocess.TimeoutExpired:
                    process.kill(); process.communicate(); raise
                finally:
                    with self.process_lock: self.process = None
                if process.returncode: raise RuntimeError('Local Whisper could not transcribe this recording')
                text = clean_text(stdout)
                if len(text) > MAX_SECONDS * 40: raise RuntimeError('Transcript exceeds supported length')
                print(json.dumps({'event': 'transcribed', 'seconds': round(time.monotonic()-started, 2), 'characters': len(text)}), flush=True)
                return text
        finally: self.busy.release()

    def close(self):
        with self.process_lock:
            self.stopping = True
            if self.process and self.process.poll() is None: self.process.terminate()
        # Let the worker remove its private temporary recording before exiting.
        if self.busy.acquire(timeout=5): self.busy.release()


class MacHost:
    """Bounded request/reply pipe to the signed Cocoa companion."""
    def __init__(self):
        binary = Path.home() / 'Applications/ArX VR Bridge.app/Contents/MacOS/ArxMacHost'
        if not binary.is_file(): raise RuntimeError('Build the Mac companion with build_arxvr_mac.py first')
        # The companion's own messages go wherever the bridge's do, dist/bridge.log from the app
        self.process = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=sys.stderr, text=True, bufsize=1)
        self.lock = threading.Lock()
        self.pending = {}
        self.sequence = 0
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()
    def read(self):
        for line in self.process.stdout:
            try:
                result = json.loads(line)
                with self.lock: waiter = self.pending.get(result.get('request_id'))
                if waiter: waiter.put_nowait(result)
            except (ValueError, queue.Full): pass
        with self.lock:
            for waiter in self.pending.values():
                try: waiter.put_nowait({'error': 'Mac companion was closed. Restart the bridge.'})
                except queue.Full: pass
    def call(self, request):
        waiter = queue.Queue(maxsize=1)
        with self.lock:
            if self.process.poll() is not None: raise RuntimeError('Mac companion was closed. Restart the bridge.')
            self.sequence += 1
            identifier = self.sequence
            self.pending[identifier] = waiter
            self.process.stdin.write(json.dumps(dict(request, request_id=identifier)) + '\n')
            self.process.stdin.flush()
        try:
            result = waiter.get(timeout=8)
            result.pop('request_id', None)
            # An empty string is not a fault: status replies may carry the key with nothing in it
            if result.get('error'): raise RuntimeError(result['error'])
            return result
        except queue.Empty: raise TimeoutError('Mac companion timed out')
        finally:
            with self.lock: self.pending.pop(identifier, None)
    def close(self):
        try: self.process.stdin.close()
        except BrokenPipeError: pass
        try: self.process.wait(timeout=5)
        except subprocess.TimeoutExpired: self.process.terminate()


class VoiceServer(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 4
    def __init__(self, address, token, transcriber, host=None):
        self.token = token
        self.host = host
        self.transcriber = transcriber
        self.slots = threading.BoundedSemaphore(4)
        super().__init__(address, VoiceHandler)
    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request); return
        super().process_request(request, address)
    def process_request_thread(self, request, address):
        try: super().process_request_thread(request, address)
        finally: self.slots.release()


class VoiceHandler(BaseHTTPRequestHandler):
    server_version = 'ArXVR-LocalVoice/0.3'
    def setup(self):
        super().setup(); self.connection.settimeout(12)
    def log_message(self, *args): pass
    def respond(self, status, data):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        try: self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError): pass
    def authorized(self):
        supplied = self.headers.get('Authorization', '')
        return hmac.compare_digest(supplied, 'Bearer ' + self.server.token)
    def do_GET(self):
        if not self.authorized(): self.respond(401, {'error': 'Unauthorized'}); return
        if self.path == '/health':
            result = {'ready': True, 'engine': 'whisper.cpp', 'transport': 'USB localhost', 'cloud': False}
            if self.server.host:
                try: result.update(self.server.host.call({'op': 'status'}))
                except (RuntimeError, TimeoutError) as error: result['host_error'] = str(error)
            self.respond(200, result)
        else: self.respond(404, {'error': 'Unknown endpoint'})
    def do_POST(self):
        if not self.authorized(): self.respond(401, {'error': 'Unauthorized'}); return
        if self.path == '/workspace':
            if not self.server.host: self.respond(503, {'error': 'Mac companion is not running'}); return
            try:
                size = int(self.headers.get('Content-Length', '0'))
                if not 2 <= size <= 65536 or self.headers.get('Transfer-Encoding') or self.headers.get('Content-Type') != 'application/json':
                    raise ValueError('Invalid workspace request')
                request = json.loads(self.rfile.read(size))
                if not isinstance(request, dict) or request.get('op') not in ('status', 'input', 'add', 'remove', 'monitor', 'overview', 'mic', 'video_start', 'video_stop', 'video_status'):
                    raise ValueError('Unsupported workspace request')
                if request.get('op') in ('add', 'remove', 'monitor') and request.get('slot') not in range(1, MAX_DESKTOPS + 1):
                    raise ValueError('Invalid desktop slot')
                if request.get('op') == 'monitor' and (type(request.get('display')) is not int or request['display'] <= 0):
                    raise ValueError('Invalid display')
                if request.get('op') == 'mic' and type(request.get('on')) is not bool:
                    raise ValueError('Invalid microphone state')
                if request.get('op') == 'input':
                    if request.get('kind') not in ('pointer', 'text', 'key', 'release'): raise ValueError('Invalid input action')
                    if request.get('kind') == 'text' and (not isinstance(request.get('text'), str) or len(request['text']) > 16000): raise ValueError('Invalid input text')
                    if request.get('kind') == 'key':
                        for name in ('key', 'modifiers'):
                            if type(request.get(name)) is not int: raise ValueError('Invalid key')
                        if not 0 <= request['key'] <= 255 or not 0 <= request['modifiers'] <= 15: raise ValueError('Invalid key')
                    if request.get('kind') == 'pointer':
                        for name in ('buttons', 'scroll'):
                            if type(request.get(name)) is not int: raise ValueError('Invalid pointer buttons')
                        if not 0 <= request['buttons'] <= 7 or not -20 <= request['scroll'] <= 20: raise ValueError('Invalid pointer buttons')
                        # Sideways scroll is optional, so an older headset build still gets through
                        hscroll = request.get('hscroll', 0)
                        if type(hscroll) is not int or not -20 <= hscroll <= 20: raise ValueError('Invalid pointer buttons')
                        if request.get('slot') not in range(0, MAX_DESKTOPS + 1): raise ValueError('Invalid desktop slot')
                        for name in ('u', 'v'):
                            if not isinstance(request.get(name), (int, float)) or not math.isfinite(request[name]): raise ValueError('Invalid pointer')
                self.respond(200, self.server.host.call(request))
            except (ValueError, TypeError): self.respond(400, {'error': 'Invalid workspace request'})
            except (RuntimeError, TimeoutError) as error: self.respond(503, {'error': str(error)})
            return
        if self.path != '/transcribe': self.respond(404, {'error': 'Unknown endpoint'}); return
        if self.headers.get('Transfer-Encoding') or self.headers.get('Content-Type') != 'audio/wav':
            self.respond(415, {'error': 'Expected bounded audio/wav body'}); return
        try:
            size = int(self.headers.get('Content-Length', '0'))
            if not 44 <= size <= MAX_BYTES: raise ValueError('Recording size is outside the supported range')
            data = self.rfile.read(size)
            if len(data) != size: raise ValueError('Recording transfer is incomplete')
            validate_audio(data)
            self.respond(200, {'text': self.server.transcriber(data)})
        except ValueError as error: self.respond(400, {'error': str(error)})
        except LookupError as error: self.respond(503, {'error': str(error)})
        except BlockingIOError: self.respond(409, {'error': 'Mac is transcribing another recording. Retry shortly.'})
        except (TimeoutError, subprocess.TimeoutExpired): self.respond(504, {'error': 'Local transcription timed out'})
        except Exception:
            self.respond(500, {'error': 'Local transcription failed. Check the Mac bridge.'})


def pairing_token():
    # Kept across restarts, off Google Drive and readable by this user only. A new secret on every
    # start orphaned a running headset session, and a headset on Wi-Fi alone has no cable to be re-provisioned over.
    path = Path.home() / 'Library/Application Support/ArX VR Bridge/token'
    try:
        token = path.read_text().strip()
        if len(token) >= 32: return token
    except OSError: pass
    path.parent.mkdir(parents=True, exist_ok=True)
    token = secrets.token_urlsafe(32)
    path.write_text(token)
    path.chmod(0o600)
    return token


def serve():
    tools = Path(os.environ.get('ARXVR_TOOLCHAIN_ROOT', Path.home() / '.arxvr_toolchain'))
    adb = tools / 'android-sdk/platform-tools/adb'
    transcriber = LocalWhisper()
    token = pairing_token()
    # Every interface, so the headset reaches it over Wi-Fi as well as the cable. Every request
    # still has to carry the pairing secret, compared in constant time, exactly as the video port does.
    server = VoiceServer(('0.0.0.0', PORT), token, transcriber.transcribe)
    host = None
    stopping = threading.Event()
    provisioned = set()
    def stop(signum, frame): raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)

    def attach():
        # A thread that dies quietly takes the headset link with it, so say so in the log
        try: attach_loop()
        except Exception:
            import traceback; traceback.print_exc()
            print('Bridge attach loop stopped. Restart the bridge.', flush=True)

    def attach_loop():
        previous = None
        # Each fault is said once, so a three second loop does not bury the log in repeats
        video_warned = status_warned = False
        while not stopping.is_set():
            # Closing the Bridge window quits the companion, and that is how you switch the
            # bridge off: everything stops with it. Nothing is ever restarted behind your back.
            if host.process.poll() is not None:
                print(f'Mac companion closed (code {host.process.returncode}), bridge stopping', flush=True)
                server.shutdown(); return
            try:
                listing = subprocess.check_output([str(adb), 'devices', '-l'], text=True, timeout=5)
                devices = [line.split() for line in listing.splitlines()[1:] if len(line.split()) >= 2 and line.split()[1] == 'device']
                if len(devices) == 1:
                    serial = devices[0][0]
                    transport = next((x for x in devices[0] if x.startswith('transport_id:')), '')
                    current = (serial, transport)
                    if previous != current:
                        command = [str(adb), '-s', serial]
                        # Replace only our loopback endpoint; never touch any other reverse port.
                        existing = subprocess.check_output(command + ['reverse', '--list'], text=True, timeout=5)
                        matches = [line.split() for line in existing.splitlines() if f'tcp:{PORT}' in line.split()]
                        if any(parts[-2:] != [f'tcp:{PORT}', f'tcp:{PORT}'] for parts in matches):
                            raise RuntimeError('USB bridge port is mapped to a different endpoint')
                        subprocess.run(command + ['reverse', f'tcp:{PORT}', f'tcp:{PORT}'], check=True, capture_output=True, timeout=5)
                        # Video goes over Wi-Fi; this loopback is the fallback for when it cannot
                        subprocess.run(command + ['reverse', f'tcp:{VIDEO_PORT}', f'tcp:{VIDEO_PORT}'], capture_output=True, timeout=5)
                        # The mirror of the headset's view comes back the same way when there is no Wi-Fi
                        subprocess.run(command + ['reverse', f'tcp:{MIRROR_PORT}', f'tcp:{MIRROR_PORT}'], capture_output=True, timeout=5)
                        provisioned.add(serial)
                        subprocess.run(command + ['shell', 'run-as', PACKAGE, 'mkdir', '-p', 'files'], check=True, capture_output=True, timeout=5)
                        config = json.dumps({'url': f'http://127.0.0.1:{PORT}', 'token': token,
                                             'host': mac_lan_address()}).encode()
                        subprocess.run(command + ['shell', 'run-as', PACKAGE, 'sh', '-c',
                            "'umask 077; cat > files/arx_voice_bridge.json.tmp && mv files/arx_voice_bridge.json.tmp files/arx_voice_bridge.json'"],
                            input=config, check=True, capture_output=True, timeout=5)
                        # Normal sleep when the headset is taken off, so nothing acts on the Mac
                        # while nobody is wearing it; undoes any stay-awake left by testing
                        subprocess.run(command + ['shell', 'am', 'broadcast', '-a', 'com.oculus.vrpowermanager.automation_disable'],
                                       capture_output=True, timeout=5)
                        previous = current
                        host.call({'op': 'usb', 'connected': True})
                        print('Quest connected. Local controls, extra desktops and dictation are ready for permission checks.', flush=True)
                else:
                    if previous is not None:
                        try:
                            host.call({'op': 'input', 'kind': 'release'})
                            host.call({'op': 'usb', 'connected': False})
                        except (RuntimeError, TimeoutError): pass
                    previous = None
            except (RuntimeError, OSError, subprocess.SubprocessError):
                # App not installed, unauthorized headset or transient disconnect. Retry without printing secrets.
                previous = None
            # Our own desktop video, started once and left up for the headset to attach to
            try:
                if not host.call({'op': 'video_status'}).get('running'):
                    host.call({'op': 'video_start', 'port': VIDEO_PORT, 'fps': 60, 'bitrate': 30000, 'token': token})
                    print(f'Desktop video ready on port {VIDEO_PORT}.', flush=True)
            except (RuntimeError, TimeoutError) as error:
                if not video_warned:
                    video_warned = True
                    granted = host.call({'op': 'status'}).get('screen_recording')
                    print(f'Desktop video could not start: {error} (screen recording granted: {granted})', flush=True)
            try:
                snapshot = host.call({'op': 'status'})
                snapshot.update({'updated_at': datetime.now(timezone.utc).isoformat(), 'running': True,
                    'voice_model': transcriber.model_name, 'transport': 'USB localhost',
                    'video': host.call({'op': 'video_status'}), 'mac_address': mac_lan_address()})
                status_path = Path(__file__).parent / 'dist/bridge-status.json'
                temporary = status_path.with_suffix('.tmp')
                temporary.write_text(json.dumps(snapshot, indent=2) + '\n')
                temporary.chmod(0o600)
                temporary.replace(status_path)
            except (RuntimeError, TimeoutError, OSError) as error:
                if not status_warned:
                    status_warned = True
                    print('Bridge status could not be written:', repr(error), flush=True)
            stopping.wait(3)
    worker = None
    try:
        host = MacHost()
        server.host = host
        if os.environ.get('ARXVR_VIDEO_TEST'):
            sys.path.insert(0, str(Path(__file__).parent))
            from test_arxvr_video import run_video_selftest
            # A diagnostic must never take the bridge down with it
            try: outcome = run_video_selftest(host.call, token)
            except Exception as error: outcome = {'error': f'{type(error).__name__}: {error}'}
            (Path(__file__).parent / 'dist/video-selftest.json').write_text(json.dumps(outcome, indent=2) + '\n')
            print('video self test:', outcome, flush=True)
        worker = threading.Thread(target=attach, daemon=True); worker.start()
        print(f'ArX VR Bridge ready: {transcriber.model_name}, {transcriber.language}. Waiting for authorized Quest USB.', flush=True)
        print('Audio stays on this Mac. Close the companion window to stop the bridge.', flush=True)
        server.serve_forever(poll_interval=0.25)
    finally:
        stopping.set()
        server.server_close()
        if worker: worker.join(timeout=6)
        if host: host.close()
        transcriber.close()
        status_path = Path(__file__).parent / 'dist/bridge-status.json'
        try: status_path.write_text(json.dumps({'running': False, 'updated_at': datetime.now(timezone.utc).isoformat()}) + '\n')
        except OSError: pass
        for serial in provisioned:
            for port in (PORT, VIDEO_PORT, MIRROR_PORT):
                try: subprocess.run([str(adb), '-s', serial, 'reverse', '--remove', f'tcp:{port}'], capture_output=True, timeout=5)
                except subprocess.TimeoutExpired: pass


if __name__ == '__main__':
    gui = os.environ.get('ARXVR_GUI_LAUNCH') == '1'
    if gui:
        # Launched from the app there is no terminal to print into, and a bridge that fails
        # silently is the thing that wastes the most time. Everything it says lands here.
        log = Path(__file__).parent / 'dist/bridge.log'
        log.parent.mkdir(parents=True, exist_ok=True)
        stream = open(log, 'w', buffering=1)
        sys.stdout = sys.stderr = stream
    if gui or input('Start ArX VR Bridge for Mac control, extra desktops and local dictation? (y/n): ').strip().lower() == 'y':
        try: serve()
        except KeyboardInterrupt: print('\nArX VR Bridge stopped.')
        except (RuntimeError, OSError, subprocess.SubprocessError) as error: raise SystemExit(str(error))
