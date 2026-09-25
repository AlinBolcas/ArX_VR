"""USB capability boundary, malformed audio and host-command rejection tests."""
from pathlib import Path
import importlib.util
import http.client
import io
import json
import math
import struct
import threading
import unittest
import wave

spec = importlib.util.spec_from_file_location('arxvr_voice_bridge', Path(__file__).resolve().parents[1] / 'host_tools' / 'arxvr_voice_bridge.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


def recording(channels=1, rate=16000, frames=3200):
    buffer=io.BytesIO()
    with wave.open(buffer,'wb') as output:
        output.setnchannels(channels); output.setsampwidth(2); output.setframerate(rate)
        output.writeframes(b''.join(struct.pack('<h',int(9000*math.sin(i/10))) for i in range(frames*channels)))
    return buffer.getvalue()

class FakeHost:
    def __init__(self): self.calls=[]
    def call(self, request): self.calls.append(request); return {'ok':True, 'accessibility':True}

class BridgeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.host=FakeHost()
        cls.server=bridge.VoiceServer(('127.0.0.1',0),'test-token',lambda data:'hello from the headset',cls.host)
        cls.worker=threading.Thread(target=cls.server.serve_forever); cls.worker.start()
    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown(); cls.server.server_close(); cls.worker.join()
    def call(self,path,body=None,token='test-token',content='application/json',extra=None):
        client=http.client.HTTPConnection('127.0.0.1',self.server.server_port,timeout=3)
        headers={'Authorization':'Bearer '+token,'Content-Type':content}
        headers.update(extra or {})
        client.request('GET' if body is None else 'POST',path,body=body,headers=headers)
        result=client.getresponse(); status=result.status; value=json.loads(result.read()); client.close()
        return status,value
    def test_rejects_unpaired_requests(self):
        for path in ['/health']:
            self.assertEqual(self.call(path,token='wrong')[0],401)
        self.assertEqual(self.call('/workspace',b'{"op":"add","slot":1}',token='wrong')[0],401)
    def test_health_reports_host_permission(self):
        status,result=self.call('/health'); self.assertEqual(status,200)
        self.assertTrue(result['accessibility']); self.assertFalse(result['cloud'])
    def test_valid_voice_transfer(self):
        self.assertEqual(self.call('/transcribe',recording(),content='audio/wav'),(200,{'text':'hello from the headset'}))
    def test_rejects_wrong_audio_format(self):
        for audio in [b'invalid'*20,recording(channels=2),recording(rate=44100),recording()[:-40],recording(frames=100)]:
            self.assertEqual(self.call('/transcribe',audio,content='audio/wav')[0],400)
    def test_rejects_chunked_audio(self):
        self.assertEqual(self.call('/transcribe',b'',content='audio/wav',extra={'Transfer-Encoding':'chunked'})[0],415)
    def test_rejects_audio_over_limit_without_reading_body(self):
        self.assertEqual(self.call('/transcribe',b'',content='audio/wav',extra={'Content-Length':str(bridge.MAX_BYTES+1)})[0],400)
    def test_workspace_rejects_unsupported_operations_and_native_types(self):
        bad=[{'op':'shell','command':'anything'}, {'op':'remove','slot':0}, {'op':'add','slot':bridge.MAX_DESKTOPS+1},
            {'op':'probe'}, {'op':'frame','slot':1}, [],
            {'op':'input','kind':'key','key':[], 'modifiers':0},
            {'op':'input','kind':'pointer','slot':0,'u':float('nan'),'v':0,'buttons':0,'scroll':0},
            {'op':'input','kind':'pointer','slot':0,'u':0,'v':0,'buttons':{},'scroll':0},
            {'op':'input','kind':'text','text':{}}]
        for value in bad:
            before=len(self.host.calls)
            self.assertEqual(self.call('/workspace',json.dumps(value).encode())[0],400)
            self.assertEqual(len(self.host.calls),before)
    def test_allowed_display_and_input_request(self):
        for request in [{'op':'add','slot':1},{'op':'remove','slot':2},{'op':'input','kind':'key','key':67,'modifiers':8},
            {'op':'input','kind':'pointer','slot':1,'u':.5,'v':.5,'buttons':1,'scroll':0}]:
            self.assertEqual(self.call('/workspace',json.dumps(request).encode())[0],200)
            self.assertEqual(self.host.calls[-1],request)
    def test_silence_and_marker_filter(self):
        out=io.BytesIO()
        with wave.open(out,'wb') as stream:
            stream.setnchannels(1); stream.setsampwidth(2); stream.setframerate(16000); stream.writeframes(b'\0'*6400)
        self.assertEqual(bridge.validate_audio(out.getvalue()),0)
        self.assertEqual(bridge.clean_text('[BLANK_AUDIO] (music) Hello   world.'),'Hello world.')

if __name__=='__main__': unittest.main()
