// Layout: RIFF 'AVI ' { LIST 'hdrl' { avih, LIST 'strl' { strh, strf },
//                                     [LIST 'strl' { strh, strf }] },
//                       LIST 'movi' { '00db' / '01wb' chunks... }, idx1 }
// Sizes and lengths are patched in aviClose().

#include <stdlib.h>
#include <string.h>

#include "avi.h"

#define OFS_RIFF_SIZE    4
#define HEADER_MAX       512

#define AVIF_HASINDEX       0x10
#define AVIF_ISINTERLEAVED  0x100
#define AVIIF_KEYFRAME      0x10
#define INDEX_AUDIO         0x80000000u
#define INDEX_GROW          4096
#define AUDIO_BUFFER_HINT   8192

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static uint8_t *fourcc(uint8_t *p, const char *s) { memcpy(p, s, 4); return p + 4; }
static uint8_t *word(uint8_t *p, uint16_t v) { put16(p, v); return p + 2; }
static uint8_t *dword(uint8_t *p, uint32_t v) { put32(p, v); return p + 4; }

static bool writeAll(AviWriter *a, const void *data, uint32_t size)
{
    if (fwrite(data, 1, size, a->f) != size)
        return false;
    a->pos += size;
    return true;
}

static bool patch32(FILE *f, long offset, uint32_t v)
{
    uint8_t b[4];
    put32(b, v);
    return fseek(f, offset, SEEK_SET) == 0 && fwrite(b, 1, 4, f) == 4;
}

bool aviOpen(AviWriter *a, const char *path, int width, int height, int fps, int sampleRate)
{
    memset(a, 0, sizeof(*a));
    a->frameBytes = width * height * 2;
    a->sampleRate = sampleRate;
    bool audio = sampleRate > 0;

    a->f = fopen(path, "wb");
    if (a->f == NULL)
        return false;
    setvbuf(a->f, NULL, _IOFBF, 32 * 1024);

    uint8_t h[HEADER_MAX] = { 0 };
    uint8_t *p = h;

    p = fourcc(p, "RIFF");
    p = dword(p, 0);                      // patched
    p = fourcc(p, "AVI ");

    p = fourcc(p, "LIST");
    p = dword(p, audio ? 292 : 192);
    p = fourcc(p, "hdrl");

    p = fourcc(p, "avih");
    p = dword(p, 56);
    p = dword(p, 1000000 / fps);          // microseconds per frame
    p = dword(p, a->frameBytes * fps + sampleRate * 2); // max bytes per second
    p = dword(p, 0);                      // padding granularity
    p = dword(p, AVIF_HASINDEX | (audio ? AVIF_ISINTERLEAVED : 0));
    a->ofsTotalFrames = p - h;
    p = dword(p, 0);                      // total frames, patched
    p = dword(p, 0);                      // initial frames
    p = dword(p, audio ? 2 : 1);          // streams
    p = dword(p, a->frameBytes);          // suggested buffer size
    p = dword(p, width);
    p = dword(p, height);
    p += 16;                              // reserved

    // Video stream
    p = fourcc(p, "LIST");
    p = dword(p, 116);
    p = fourcc(p, "strl");

    p = fourcc(p, "strh");
    p = dword(p, 56);
    p = fourcc(p, "vids");
    p = fourcc(p, "DIB ");
    p = dword(p, 0);                      // flags
    p = dword(p, 0);                      // priority, language
    p = dword(p, 0);                      // initial frames
    p = dword(p, 1);                      // scale
    p = dword(p, fps);                    // rate (fps = rate / scale)
    p = dword(p, 0);                      // start
    a->ofsVideoLen = p - h;
    p = dword(p, 0);                      // length in frames, patched
    p = dword(p, a->frameBytes);          // suggested buffer size
    p = dword(p, 0xFFFFFFFF);             // quality: default
    p = dword(p, 0);                      // sample size: varies (repeats are empty)
    put16(p + 4, width);                  // rcFrame: left, top, right, bottom
    put16(p + 6, height);
    p += 8;

    p = fourcc(p, "strf");
    p = dword(p, 40);
    p = dword(p, 40);                     // BITMAPINFOHEADER size
    p = dword(p, width);
    p = dword(p, height);                 // positive: rows are bottom-up
    p = word(p, 1);                       // planes
    p = word(p, 16);                      // bits per pixel
    p = dword(p, 0);                      // BI_RGB
    p = dword(p, a->frameBytes);
    p += 16;                              // resolution, palette

    // Audio stream
    if (audio)
    {
        p = fourcc(p, "LIST");
        p = dword(p, 92);
        p = fourcc(p, "strl");

        p = fourcc(p, "strh");
        p = dword(p, 56);
        p = fourcc(p, "auds");
        p = dword(p, 0);                  // handler
        p = dword(p, 0);                  // flags
        p = dword(p, 0);                  // priority, language
        p = dword(p, 0);                  // initial frames
        p = dword(p, 2);                  // scale: bytes per block
        a->ofsAudioRate = p - h;
        p = dword(p, sampleRate * 2);     // rate: bytes per second, patched
        p = dword(p, 0);                  // start
        a->ofsAudioLen = p - h;
        p = dword(p, 0);                  // length in samples, patched
        p = dword(p, AUDIO_BUFFER_HINT);  // suggested buffer size
        p = dword(p, 0xFFFFFFFF);         // quality: default
        p = dword(p, 2);                  // sample size
        p += 8;                           // rcFrame

        p = fourcc(p, "strf");
        p = dword(p, 16);
        a->ofsAudioFormat = p - h;
        p = word(p, 1);                   // WAVE_FORMAT_PCM
        p = word(p, 1);                   // channels
        p = dword(p, sampleRate);         // patched
        p = dword(p, sampleRate * 2);     // bytes per second, patched
        p = word(p, 2);                   // block align
        p = word(p, 16);                  // bits per sample
    }

    p = fourcc(p, "LIST");
    a->ofsMoviSize = p - h;
    p = dword(p, 0);                      // patched
    a->ofsMovi = p - h;
    p = fourcc(p, "movi");

    return writeAll(a, h, p - h);
}

static bool addIndex(AviWriter *a, uint32_t info)
{
    if (a->indexCount == a->indexCap)
    {
        AviIndexEntry *grown = realloc(a->index, (a->indexCap + INDEX_GROW) * sizeof(*grown));
        if (grown == NULL)
            return false;
        a->index = grown;
        a->indexCap += INDEX_GROW;
    }
    a->index[a->indexCount++] = (AviIndexEntry){ a->pos - a->ofsMovi, info };
    return true;
}

bool aviWriteFrame(AviWriter *a, uint8_t *chunk)
{
    memcpy(chunk, "00db", 4);
    put32(chunk + 4, a->frameBytes);
    if (!addIndex(a, a->frameBytes) || !writeAll(a, chunk, AVI_CHUNK_HEADER + a->frameBytes))
        return false;
    a->frames++;
    return true;
}

bool aviWriteRepeat(AviWriter *a)
{
    uint8_t chunk[AVI_CHUNK_HEADER] = { '0', '0', 'd', 'b', 0, 0, 0, 0 };
    if (!addIndex(a, 0) || !writeAll(a, chunk, sizeof(chunk)))
        return false;
    a->frames++;
    return true;
}

bool aviWriteAudio(AviWriter *a, const void *pcm, uint32_t bytes)
{
    uint8_t chunk[AVI_CHUNK_HEADER] = { '0', '1', 'w', 'b' };
    put32(chunk + 4, bytes);
    if (!addIndex(a, bytes | INDEX_AUDIO) || !writeAll(a, chunk, sizeof(chunk))
        || !writeAll(a, pcm, bytes))
        return false;
    a->audioBytes += bytes;
    return true;
}

void aviSetSampleRate(AviWriter *a, uint32_t sampleRate)
{
    if (a->sampleRate > 0)
        a->sampleRate = sampleRate;
}

bool aviClose(AviWriter *a)
{
    uint32_t moviEnd = a->pos;
    uint8_t hdr[8];
    memcpy(hdr, "idx1", 4);
    put32(hdr + 4, a->indexCount * 16);
    bool ok = writeAll(a, hdr, sizeof(hdr));

    uint8_t buf[64 * 16];
    uint32_t n = 0;
    for (uint32_t i = 0; ok && i < a->indexCount; i++)
    {
        uint8_t *e = buf + n * 16;
        bool audio = a->index[i].info & INDEX_AUDIO;
        memcpy(e, audio ? "01wb" : "00db", 4);
        put32(e + 4, AVIIF_KEYFRAME);
        put32(e + 8, a->index[i].offset);
        put32(e + 12, a->index[i].info & ~INDEX_AUDIO);
        if (++n == 64 || i == a->indexCount - 1)
        {
            ok = writeAll(a, buf, n * 16);
            n = 0;
        }
    }

    ok = ok && patch32(a->f, OFS_RIFF_SIZE, a->pos - 8)
            && patch32(a->f, a->ofsTotalFrames, a->frames)
            && patch32(a->f, a->ofsVideoLen, a->frames)
            && patch32(a->f, a->ofsMoviSize, moviEnd - a->ofsMovi);
    if (a->sampleRate > 0)
    {
        ok = ok && patch32(a->f, a->ofsAudioRate, a->sampleRate * 2)
                && patch32(a->f, a->ofsAudioLen, a->audioBytes / 2)
                && patch32(a->f, a->ofsAudioFormat + 4, a->sampleRate)
                && patch32(a->f, a->ofsAudioFormat + 8, a->sampleRate * 2);
    }

    ok = (fclose(a->f) == 0) && ok;
    free(a->index);
    a->f = NULL;
    a->index = NULL;
    return ok;
}
