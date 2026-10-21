// AVI writer: uncompressed 16-bit video (X1R5G5B5, bottom-up), optional mono 16-bit PCM.

#ifndef AVI_H__
#define AVI_H__

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// Space aviWriteFrame() needs before the pixels.
#define AVI_CHUNK_HEADER 8

typedef struct {
    uint32_t offset;     // from the 'movi' fourcc
    uint32_t info;       // chunk size, top bit set for audio
} AviIndexEntry;

typedef struct {
    FILE *f;
    uint32_t pos;        // current file offset
    uint32_t frameBytes;
    uint32_t frames;     // including repeats
    uint32_t sampleRate; // 0 = no audio
    uint32_t audioBytes;
    AviIndexEntry *index;
    uint32_t indexCount, indexCap;
    // Patched on close
    uint32_t ofsTotalFrames, ofsVideoLen, ofsAudioRate, ofsAudioLen, ofsAudioFormat;
    uint32_t ofsMoviSize, ofsMovi;
} AviWriter;

bool aviOpen(AviWriter *a, const char *path, int width, int height, int fps, int sampleRate);

// chunk: AVI_CHUNK_HEADER bytes of space, then the frame.
bool aviWriteFrame(AviWriter *a, uint8_t *chunk);

// Empty chunk; players repeat the last frame.
bool aviWriteRepeat(AviWriter *a);

// bytes should be a multiple of 4 so the frames after it stay word aligned for the SD driver.
bool aviWriteAudio(AviWriter *a, const void *pcm, uint32_t bytes);

// Rate written on close, e.g. the measured one.
void aviSetSampleRate(AviWriter *a, uint32_t sampleRate);

// Writes the index and sizes. Always closes the file.
bool aviClose(AviWriter *a);

#endif // AVI_H__
