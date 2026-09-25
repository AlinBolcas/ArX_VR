package com.limelight;
import org.junit.Test;
import static org.junit.Assert.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class ArxVoiceTest {
    // The header goes up ahead of the recording streamed from its file, so it alone has to be right
    @Test public void headerDescribesMonoPcm16kAudioOfTheGivenLength() {
        long pcm = 8;
        byte[] wav = ArxVoice.wavHeader(pcm);
        ByteBuffer header = ByteBuffer.wrap(wav).order(ByteOrder.LITTLE_ENDIAN);
        assertEquals(44, wav.length);
        assertEquals(36 + pcm, header.getInt(4));
        assertEquals(1, header.getShort(20));
        assertEquals(1, header.getShort(22));
        assertEquals(16000, header.getInt(24));
        assertEquals(32000, header.getInt(28));
        assertEquals(16, header.getShort(34));
        assertEquals(pcm, header.getInt(40));
    }
}
