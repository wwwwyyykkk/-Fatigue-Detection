#!/usr/bin/env python3
"""Convert WAV files in Project/audio/ to 8000Hz C byte arrays.

Device only supports 8000Hz/16bit/mono (not 16kHz).
We resample all WAV files to 8000Hz before embedding.
"""
import os
import sys
import struct
import wave
import io

AUDIO_DIR = os.path.join(os.path.dirname(__file__), '..', 'Project', 'audio')
OUTPUT_DIR = os.path.join(os.path.dirname(__file__), '..', 'Project', 'components', 'voice_prompter')
os.makedirs(OUTPUT_DIR, exist_ok=True)

TARGET_RATE = 8000
TARGET_CHANNELS = 1
TARGET_SAMPWIDTH = 2  # 16-bit

def simple_resample(pcm_data, src_rate, dst_rate, sampwidth, channels):
    """Linear interpolation resampling of PCM data."""
    if src_rate == dst_rate:
        return pcm_data
    if sampwidth != 2:
        raise ValueError("Only 16-bit supported")
    if channels != 1:
        raise ValueError("Only mono supported")

    src_samples = len(pcm_data) // 2
    dst_samples = int(src_samples * dst_rate / src_rate)

    import array
    src = array.array('h')
    src.frombytes(pcm_data)

    dst = array.array('h', [0]) * dst_samples
    ratio = src_rate / dst_rate
    for i in range(dst_samples):
        src_pos = i * ratio
        idx = int(src_pos)
        frac = src_pos - idx
        if idx + 1 < len(src):
            sample = int(src[idx] * (1 - frac) + src[idx + 1] * frac)
        else:
            sample = src[idx]
        # Clamp to 16-bit
        if sample > 32767: sample = 32767
        if sample < -32768: sample = -32768
        dst[i] = sample

    return dst.tobytes()


def build_wav(pcm_data, sample_rate, sampwidth, channels):
    """Build a complete WAV file in memory from raw PCM data."""
    buf = io.BytesIO()
    with wave.open(buf, 'wb') as w:
        w.setnchannels(channels)
        w.setsampwidth(sampwidth)
        w.setframerate(sample_rate)
        w.writeframes(pcm_data)
    return buf.getvalue()


FILES = [
    ('boot_wav',        'boot.wav'),
    ('face_detect_wav', 'face_detect.wav'),
    ('face_auth_wav',   'face_auth.wav'),
    ('mild_wav',        'mild.wav'),
    ('severe_wav',      'severe.wav'),
    ('drive_long_wav',  'drive_long.wav'),
]

HEADER = """/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Auto-generated from WAV files by tools/wav_to_carray.py
 * WAV files resampled to 8000Hz/16bit/mono for device compatibility.
 */

#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

"""

FOOTER = """
#ifdef __cplusplus
}
#endif
"""


def main():
    entries = []
    for sym_name, filename in FILES:
        filepath = os.path.join(AUDIO_DIR, filename)
        if not os.path.exists(filepath):
            print(f"WARNING: {filepath} not found, skipping", file=sys.stderr)
            continue

        with wave.open(filepath, 'rb') as w:
            src_rate = w.getframerate()
            src_channels = w.getnchannels()
            src_sampwidth = w.getsampwidth()
            src_frames = w.getnframes()
            src_pcm = w.readframes(src_frames)

        print(f"  {filename}: {src_rate}Hz/{src_sampwidth*8}bit/{src_channels}ch "
              f"{src_frames} frames ({len(src_pcm)}B PCM)")

        # Convert to mono if stereo (average channels)
        if src_channels > 1:
            import array
            raw = array.array('h')
            raw.frombytes(src_pcm)
            mono = array.array('h', [0]) * (len(raw) // src_channels)
            for i in range(len(mono)):
                s = 0
                for ch in range(src_channels):
                    s += raw[i * src_channels + ch]
                s //= src_channels
                mono[i] = s
            src_pcm = mono.tobytes()
            src_channels = 1

        # Resample to 8000Hz
        pcm_8k = simple_resample(src_pcm, src_rate, TARGET_RATE,
                                 TARGET_SAMPWIDTH, TARGET_CHANNELS)

        # Build WAV container (with proper header for the device's rate)
        wav_data = build_wav(pcm_8k, TARGET_RATE, TARGET_SAMPWIDTH, TARGET_CHANNELS)

        print(f"    -> {TARGET_RATE}Hz/{TARGET_SAMPWIDTH*8}bit/mono "
              f"{len(pcm_8k)//2} samples ({len(wav_data)}B total)")

        # Generate hex C array
        array_name = sym_name
        data_name = f"{sym_name}_len"
        hex_lines = []
        for i in range(0, len(wav_data), 12):
            chunk = wav_data[i:i+12]
            hex_str = ' '.join(f'0x{b:02x},' for b in chunk)
            hex_lines.append(f'    {hex_str}')
        hex_body = '\n'.join(hex_lines)

        entries.append(f"""/* {filename} -> {TARGET_RATE}Hz/16bit/mono */
static const uint8_t {array_name}[] __attribute__((aligned(4))) = {{
{hex_body}
}};
static const size_t {data_name} = sizeof({array_name});

""")

    output = HEADER
    output += ''.join(entries)
    output += FOOTER

    outpath = os.path.join(OUTPUT_DIR, 'audio_resources.h')
    with open(outpath, 'w', encoding='utf-8') as f:
        f.write(output)

    print(f"\nGenerated: {outpath} ({len(entries)} files)")
    total = sum(len(open(os.path.join(OUTPUT_DIR, 'audio_resources.h'), 'rb').read())
                for _ in [1])
    print(f"Total header size: ~{total/1024:.0f} KB")


if __name__ == '__main__':
    main()
