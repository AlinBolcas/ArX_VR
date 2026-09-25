"""Self test for the Mac video path: start the encoder, connect as the headset would, read frames,
decode them and check the picture is real rather than blank.

Screen capture belongs to the app bundle, so this runs inside the bridge process
(ARXVR_VIDEO_TEST=1), never from a plain shell, which has no capture permission.
"""
from pathlib import Path
import socket
import struct
import subprocess
import time

PORT = 47997


def read_exactly(client, count):
    data = b''
    while len(data) < count:
        chunk = client.recv(count - len(data))
        if not chunk:
            raise RuntimeError('server closed the connection')
        data += chunk
    return data


def run_video_selftest(call, token, seconds=5):
    """call: sends one request to the Mac companion and returns its reply."""
    result = {'started': call({'op': 'video_start', 'port': PORT, 'fps': 60, 'bitrate': 25000, 'token': token})}
    if 'error' in result['started']:
        return result
    try:
        client = socket.create_connection(('127.0.0.1', PORT), timeout=10)
        client.settimeout(10)
        client.sendall((token + '\n').encode())
        header = read_exactly(client, 16)
        if header[:4] != b'ARXV':
            return dict(result, error=f'bad header {header[:4]!r}')
        width, height, fps = struct.unpack('>HHH', header[8:14])
        result.update(width=width, height=height, fps=fps)

        stream, frames, keyframes = bytearray(), 0, 0
        deadline = time.time() + seconds
        while time.time() < deadline and frames < 120:
            head = read_exactly(client, 13)
            length, flags = struct.unpack('>IB', head[:5])
            stream += read_exactly(client, length)
            frames += 1
            keyframes += flags & 1
        client.close()
        result.update(frames=frames, keyframes=keyframes, kilobytes=round(len(stream) / 1024))
        if frames < 5 or keyframes < 1:
            return dict(result, error='too few frames or no keyframe')

        raw = Path(__file__).parent / 'dist/selftest.h264'
        png = Path(__file__).parent / 'dist/selftest.png'
        raw.write_bytes(bytes(stream))
        subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-f', 'h264', '-i', str(raw),
                        '-frames:v', '1', '-vf', 'scale=640:-1', str(png)], check=True, timeout=60)
        from PIL import Image
        image = Image.open(png).convert('RGB').resize((80, 45))
        colours = len(set(image.getdata()))
        raw.unlink(missing_ok=True)
        png.unlink(missing_ok=True)
        result['distinct_colours'] = colours
        result['verdict'] = 'PASS: real picture' if colours > 20 else 'FAIL: blank picture'
        return result
    except Exception as error:
        return dict(result, error=f'{type(error).__name__}: {error}')
    finally:
        call({'op': 'video_stop'})
