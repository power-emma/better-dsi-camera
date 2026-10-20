// DSi Camera: a barebones photo and video app.

// Photos are saved as 640x480 24-bit BMPs in /DCIM/100DSI00 on the SD card.
// Videos are saved as 256x192 15 fps uncompressed AVIs with 16 kHz mono sound from the microphone.
// Must be launched in DSi mode (TWiLight Menu++ or Unlaunch).

#include <dirent.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <fat.h>
#include <nds.h>

#include "avi.h"

#define CAM_NDMA    1
#define PHOTO_DIR   "/DCIM/100DSI00"
#define PHOTO_W     640
#define PHOTO_H     480
#define MAX_MEDIA   1000

// Video - The camera is restarted from the DMA interrupt into a ring of frame buffers
#define VIDEO_W         256
#define VIDEO_H         192
#define VIDEO_FPS       15
#define VIDEO_BYTES     (VIDEO_W * VIDEO_H * 2)
#define REC_SLOTS       12                         // 0.75 s
#define REC_MAX_BYTES   (2000u * 1024 * 1024)      // stay under 2 GB

// Sound - The ARM7 samples the mic on a timer
#define MIC_RATE        16000
#define MIC_RATE_REAL   (BUS_CLOCK / ((BUS_CLOCK + MIC_RATE / 2) / MIC_RATE))
#define MIC_HALF_BYTES  2048                       // 64 ms
#define MIC_RING_BYTES  (64 * 1024)                // ~2 s

// Playback streams sound through a looping ring on one sound channel.
#define PLAY_RING       16384                      // ~1 s
#define PLAY_AHEAD_MS   500
#define SOUND_CLOCK     16756991

// Recording and playback timestamps come from a timer pair.
#define CLOCK_TIMER     2                          // uses timers 2 and 3
#define CLOCK_HZ        (BUS_CLOCK / 1024)

// Manual exposure ranges. The sensor frame is 704 rows long so exposure can't be longer than about one frame.
#define SHUTTER_MIN_ROWS  1
#define SHUTTER_MAX_ROWS  700

// 32 = 1.0x.
#define GAIN_MIN_32       32
#define GAIN_MAX_32       256
#define SLIDER_MAX        255

typedef struct {
    int col, row, w, h; // in console cells (8x8 px)
    const char *label;
} Button;

// Camera screen
static const Button btnSnap    = { 2,  1, 13, 4, "PHOTO" };
static Button       btnRec     = { 17, 1, 13, 4, "REC" };
static const Button btnGallery = { 2,  5, 13, 3, "GALLERY" };
static Button       btnExpo    = { 17, 5, 13, 3, "EXP: AUTO" };
static Button       btnCam     = { 2,  8, 28, 3, "CAMERA: BACK" };
static const Button sldShutter = { 2, 12, 28, 3, "" };
static const Button sldGain    = { 2, 17, 28, 3, "" };
// Gallery screen
static const Button btnPrev    = { 2,  8, 13,  5, "< PREV" };
static const Button btnNext    = { 17, 8, 13,  5, "NEXT >" };
static Button       btnPlay    = { 2, 14, 28,  3, "PLAY" };
static const Button btnBack    = { 2, 18, 28,  3, "BACK TO CAMERA" };

#define STATUS_ROW 21

typedef enum { MODE_CAMERA, MODE_GALLERY } Mode;

static u16 *topFb;              // 256x192 16-bit framebuffer
typedef struct {
    int num;
    bool video;
} Media;

static Media media[MAX_MEDIA];  // files found on SD
static int mediaCount;
static int current;             // index into media[]
static bool sdOk;

static CameraDevice camDevice = CAMERA_OUTER;

static bool manualExp;
static int shutterPos;          // slider positions
static int gainPos;


// Recording. recFrameDone() (interrupt) fills the ring at recHead, recService() drains it from recTail.
static volatile bool recActive;
static u16 *recSlot[REC_SLOTS];
static volatile u32 recTime[REC_SLOTS]; // frame completion times
static volatile int recHead, recCount;
static volatile int recOverflows, recErrors, recStalls;
static volatile u32 recLastFrame;   // last NDMA interrupt
static int recTail;
static u8 *recChunk;            // AVI chunk header + converted frame
static AviWriter recAvi;
static u32 recBase;             // time of the first frame
static bool recHaveBase;
static u32 recNextSlot;         // next slot to write
static int recRepeats, recLastStatus;
static Media recMedia;
static char recName[16];

// Mic. micDone() (interrupt) copies each half of micBuf into micRing, recService() writes it out.
// Static because the ARM7 can write one more half after the mic is turned off.
static u8 micBuf[MIC_HALF_BYTES * 2] __attribute__((aligned(32)));
static u8 micRing[MIC_RING_BYTES];
static volatile bool micActive;
static volatile u32 micHead, micTail; // total bytes
static volatile u32 micLastTime;    // last callback
static volatile int micOverflows;
static u32 micStartTime;
static u32 micSkip;                 // bytes recorded before the first frame

// Gallery video playback.
static struct {
    FILE *f;
    u16 *buf;
    u32 moviPos, moviEnd;       // 'movi' list range
    u32 pos;                    // next chunk
    bool bottomUp;
    u32 usPerFrame;
    int frames;
    int frame;                  // next frame
    bool playing;
    u32 offsetUs;               // playback time at clockStart()
    int lastStatus;
    // Sound (mono 16-bit PCM)
    u32 sampleRate;             // 0 = no sound
    u32 hwDivider;              // sound timer divider
    char audioId[5];            // chunk id, e.g. "01wb"
    u32 audioPos;               // sound reader position
    bool audioEnd;
    u32 audioWritten;           // absolute samples written
    u32 ringBase;               // absolute sample at ring offset 0
    u32 soundStartUs;           // playback time at sound start
    int soundId;                // -1 when not playing
} player;

static s16 playRing[PLAY_RING] __attribute__((aligned(32)));
static s16 playRingTmp[PLAY_RING];

// ---------------------------------------------------------------------------
// UI

static void drawButton(const Button *b)
{
    char line[33];

    for (int r = 0; r < b->h; r++)
    {
        bool edge = (r == 0 || r == b->h - 1);
        memset(line + 1, edge ? '-' : ' ', b->w - 2);
        line[0] = line[b->w - 1] = edge ? '+' : '|';
        line[b->w] = '\0';
        printf("\x1b[%d;%dH%s", b->row + r, b->col, line);
    }

    int len = strlen(b->label);
    printf("\x1b[%d;%dH%s", b->row + b->h / 2, b->col + (b->w - len) / 2, b->label);
}

static bool hit(const Button *b, const touchPosition *t)
{
    int c = t->px / 8, r = t->py / 8;
    return c >= b->col && c < b->col + b->w && r >= b->row && r < b->row + b->h;
}

static void status(const char *fmt, ...)
{
    char buf[64];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printf("\x1b[%d;0H%-31.31s", STATUS_ROW, buf);
}

static void mediaName(char *buf, size_t size, const Media *m)
{
    snprintf(buf, size, m->video ? "VID_%04d.AVI" : "IMG_%04d.BMP", m->num);
}

static void mediaPath(char *buf, size_t size, const Media *m)
{
    char name[16];
    mediaName(name, sizeof(name), m);
    snprintf(buf, size, PHOTO_DIR "/%s", name);
}

static int nextMediaNumber(void)
{
    return mediaCount > 0 ? media[mediaCount - 1].num + 1 : 0;
}

static void addMedia(Media m)
{
    if (mediaCount < MAX_MEDIA)
        media[mediaCount++] = m;
}

// ---------------------------------------------------------------------------
// Exposure. Sliders are logarithmic.

static int shutterRows(void)
{
    return SHUTTER_MIN_ROWS * powf((float)SHUTTER_MAX_ROWS / SHUTTER_MIN_ROWS,
                                   (float)shutterPos / SLIDER_MAX) + 0.5f;
}

static int gain32(void)
{
    return GAIN_MIN_32 * powf((float)GAIN_MAX_32 / GAIN_MIN_32,
                              (float)gainPos / SLIDER_MAX) + 0.5f;
}

static int toSliderPos(int value, int min, int max)
{
    if (value <= min)
        return 0;
    if (value >= max)
        return SLIDER_MAX;
    return logf((float)value / min) / logf((float)max / min) * SLIDER_MAX + 0.5f;
}

// Gain register: bits 0-6 initial gain (1/32), bits 7-8 analog multiplier (1x-8x).
static u16 encodeGain(int g)
{
    int mult = 0;
    while (g >= 64 && mult < 3)
    {
        g >>= 1;
        mult++;
    }
    return I2CREG_APT_GAIN_ANALOG(mult) | I2CREG_APT_GAIN_INITIAL(g);
}

// The camera MCU pushes its own exposure to the sensor every frame, so write both.

static void applyShutter(void)
{
    int rows = shutterRows();
    cameraMcuWriteTWL(camDevice, MCUREG_APT_AE_R9, rows);
    cameraI2cWriteTWL(camDevice, I2CREG_APT_COARSE_INTEGRATION_TIME, rows);
}

static void applyGain(void)
{
    int g = gain32();
    cameraMcuWriteTWL(camDevice, MCUREG_APT_AE_CURRENT_VIRTUAL_GAIN, g / 2);
    cameraI2cWriteTWL(camDevice, I2CREG_APT_ANALOG_GAIN_CODE_GLOBAL, encodeGain(g));
}

static void stopPreview(void);

// Per-state AE: 0 off, 1 fast settling, 2 manual, 3 continuous, 4 fast settling + metering.
static const u16 seqAeRegs[] = {
    MCUREG_APT_SEQ_PREVIEW0_AE, // entering preview
    MCUREG_APT_SEQ_PREVIEW1_AE, // preview
    MCUREG_APT_SEQ_PREVIEW2_AE, // leaving preview
    MCUREG_APT_SEQ_PREVIEW3_AE, // entering capture
};
static u16 savedSeqAe[4];

static void setManualExposure(bool manual)
{
    stopPreview();

    u16 seq = cameraMcuReadTWL(camDevice, MCUREG_APT_SEQ_MODE);
    if (manual)
        seq &= ~MCUREG_APT_SEQ_MODE_AE_ENABLE;
    else
        seq |= MCUREG_APT_SEQ_MODE_AE_ENABLE;
    cameraMcuWriteTWL(camDevice, MCUREG_APT_SEQ_MODE, seq);

    // Also per sequencer state, so capture doesn't re-meter.
    for (int i = 0; i < 4; i++)
    {
        if (manual)
        {
            savedSeqAe[i] = cameraMcuReadTWL(camDevice, seqAeRegs[i]);
            cameraMcuWriteTWL(camDevice, seqAeRegs[i], 0);
        }
        else
        {
            cameraMcuWriteTWL(camDevice, seqAeRegs[i], savedSeqAe[i]);
        }
    }

    // Apply and wait for the sequencer.
    cameraMcuWriteTWL(camDevice, MCUREG_APT_SEQ_CMD, MCUREG_APT_SEQ_CMD_REFRESH);
    for (int i = 0; i < 60 && cameraMcuReadTWL(camDevice, MCUREG_APT_SEQ_CMD) != 0; i++)
        swiWaitForVBlank();

    if (manual)
    {
        // Start the sliders at the current auto exposure.
        u16 rows = cameraMcuReadTWL(camDevice, MCUREG_APT_AE_R9);
        u16 virtGain = cameraMcuReadTWL(camDevice, MCUREG_APT_AE_CURRENT_VIRTUAL_GAIN);
        shutterPos = toSliderPos(rows, SHUTTER_MIN_ROWS, SHUTTER_MAX_ROWS);
        gainPos = toSliderPos(virtGain * 2, GAIN_MIN_32, GAIN_MAX_32);
        applyShutter();
        applyGain();
    }

    manualExp = manual;
    btnExpo.label = manual ? "EXP: MANUAL" : "EXP: AUTO";
}

static void switchCamera(void)
{
    bool manual = manualExp;
    if (manual)
        setManualExposure(false); // restore AE on the old camera
    stopPreview();

    CameraDevice next = camDevice == CAMERA_OUTER ? CAMERA_INNER : CAMERA_OUTER;
    if (cameraSelect(next))
        camDevice = next;
    else
        cameraSelect(camDevice);
    btnCam.label = camDevice == CAMERA_OUTER ? "CAMERA: BACK" : "CAMERA: FRONT";

    // Must be in preview mode before setting exposure.
    cameraStartTransfer(topFb, MCUREG_APT_SEQ_CMD_PREVIEW, CAM_NDMA);
    if (manual)
        setManualExposure(true);
}

static void drawSlider(const Button *s, const char *name, int pos, const char *value)
{
    printf("\x1b[%d;%dH%-10s%18s", s->row - 1, s->col, name, value);
    drawButton(s);

    int width = s->w - 2;
    int knob = pos * (width - 1) / SLIDER_MAX;
    char track[33];
    for (int i = 0; i < width; i++)
        track[i] = i < knob ? '=' : i == knob ? '#' : '-';
    track[width] = '\0';
    printf("\x1b[%d;%dH%s", s->row + 1, s->col + 1, track);
}

static void drawShutterSlider(void)
{
    char value[24];
    snprintf(value, sizeof(value), "%d rows", shutterRows());
    drawSlider(&sldShutter, "SHUTTER", shutterPos, value);
}

static void drawGainSlider(void)
{
    char value[24];
    int g = gain32();
    snprintf(value, sizeof(value), "%d.%02dx (0x%03X)", g / 32, (g % 32) * 100 / 32,
             encodeGain(g));
    drawSlider(&sldGain, "GAIN", gainPos, value);
}

static int sliderPosAt(const Button *s, int px)
{
    int left = (s->col + 1) * 8 + 4;
    int span = (s->w - 3) * 8;
    int pos = (px - left) * SLIDER_MAX / span;
    return pos < 0 ? 0 : pos > SLIDER_MAX ? SLIDER_MAX : pos;
}

static void drawUi(Mode mode)
{
    consoleClear();

    if (mode == MODE_CAMERA)
    {
        drawButton(&btnSnap);
        drawButton(&btnRec);
        drawButton(&btnGallery);
        drawButton(&btnExpo);
        drawButton(&btnCam);
        if (manualExp)
        {
            drawShutterSlider();
            drawGainSlider();
        }
        else
        {
            printf("\x1b[14;2HAuto exposure");
        }
        printf("\x1b[23;0HA:pic B:rec Y:gal X:exp SEL:cam");
        if (!sdOk)
            status("No SD card: can't save photos");
    }
    else
    {
        printf("\x1b[1;1HGALLERY");
        bool video = mediaCount > 0 && media[current].video;
        if (mediaCount > 0)
        {
            char name[16];
            mediaName(name, sizeof(name), &media[current]);
            printf("\x1b[3;1H%d / %d", current + 1, mediaCount);
            printf("\x1b[4;1H%s", name);
        }
        else
        {
            printf("\x1b[3;1HNo photos or videos yet");
        }
        drawButton(&btnPrev);
        drawButton(&btnNext);
        if (video)
            drawButton(&btnPlay);
        drawButton(&btnBack);
        printf("\x1b[23;0H%s", video ? "L/R:browse A:play B:back" : "Left/Right: browse  B: back");
    }
}

// ---------------------------------------------------------------------------
// BMP saving / loading

static inline int clamp255(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

// YUV422 (Y1 Cb Y2 Cr) to BGR24.
static void yuvRowToBgr(const u8 *yuv, u8 *bgr, int width)
{
    for (int x = 0; x < width; x += 2, yuv += 4, bgr += 6)
    {
        int y1 = yuv[0], cb = yuv[1] - 128, y2 = yuv[2], cr = yuv[3] - 128;

        int r = cr + (cr >> 2) + (cr >> 3) + (cr >> 5);
        int g = -((cb >> 2) + (cb >> 4) + (cb >> 5))
                - ((cr >> 1) + (cr >> 3) + (cr >> 4) + (cr >> 5));
        int b = cb + (cb >> 1) + (cb >> 2) + (cb >> 6);

        bgr[0] = clamp255(y1 + b);
        bgr[1] = clamp255(y1 + g);
        bgr[2] = clamp255(y1 + r);
        bgr[3] = clamp255(y2 + b);
        bgr[4] = clamp255(y2 + g);
        bgr[5] = clamp255(y2 + r);
    }
}

static void put16(u8 *p, u16 v) { p[0] = v; p[1] = v >> 8; }
static void put32(u8 *p, u32 v) { put16(p, v); put16(p + 2, v >> 16); }
static u16 get16(const u8 *p) { return p[0] | (p[1] << 8); }
static u32 get32(const u8 *p) { return get16(p) | ((u32)get16(p + 2) << 16); }

static bool saveBmp(const char *path, const u8 *yuv)
{
    const u32 rowBytes = PHOTO_W * 3; // already 4-byte aligned
    static u8 row[PHOTO_W * 3];

    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return false;
    setvbuf(f, NULL, _IOFBF, 32 * 1024);

    u8 hdr[54] = { 'B', 'M' };
    put32(hdr + 2, sizeof(hdr) + rowBytes * PHOTO_H); // file size
    put32(hdr + 10, sizeof(hdr));                     // pixel data offset
    put32(hdr + 14, 40);                              // BITMAPINFOHEADER size
    put32(hdr + 18, PHOTO_W);
    put32(hdr + 22, PHOTO_H);
    put16(hdr + 26, 1);                               // planes
    put16(hdr + 28, 24);                              // bits per pixel
    put32(hdr + 34, rowBytes * PHOTO_H);              // image size

    bool ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);

    // Bottom-up
    for (int y = PHOTO_H - 1; ok && y >= 0; y--)
    {
        yuvRowToBgr(yuv + y * PHOTO_W * 2, row, PHOTO_W);
        ok = fwrite(row, 1, rowBytes, f) == rowBytes;
    }

    return (fclose(f) == 0) && ok;
}

// Draws a 24-bit BMP scaled to the top screen.
static bool showBmp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;

    u8 hdr[54];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || hdr[0] != 'B' || hdr[1] != 'M'
        || get16(hdr + 28) != 24)
    {
        fclose(f);
        return false;
    }

    u32 offset = get32(hdr + 10);
    s32 w = (s32)get32(hdr + 18);
    s32 h = (s32)get32(hdr + 22);
    bool bottomUp = h > 0;
    if (h < 0)
        h = -h;
    if (w <= 0 || h == 0)
    {
        fclose(f);
        return false;
    }

    u32 rowBytes = (w * 3 + 3) & ~3;
    u8 *row = malloc(rowBytes);
    if (row == NULL)
    {
        fclose(f);
        return false;
    }

    // File order, so seeks only go forwards.
    for (int i = 0; i < SCREEN_HEIGHT; i++)
    {
        int sy = bottomUp ? SCREEN_HEIGHT - 1 - i : i;
        int srcY = sy * h / SCREEN_HEIGHT;
        int fileRow = bottomUp ? h - 1 - srcY : srcY;

        fseek(f, offset + fileRow * rowBytes, SEEK_SET);
        if (fread(row, 1, rowBytes, f) != rowBytes)
            break;

        u16 *dst = topFb + sy * 256;
        for (int sx = 0; sx < SCREEN_WIDTH; sx++)
        {
            const u8 *p = row + (sx * w / SCREEN_WIDTH) * 3;
            dst[sx] = ARGB16(1, p[2] >> 3, p[1] >> 3, p[0] >> 3);
        }
    }

    free(row);
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Media on SD

static int cmpMedia(const void *a, const void *b)
{
    const Media *ma = a, *mb = b;
    if (ma->num != mb->num)
        return ma->num - mb->num;
    return ma->video - mb->video;
}

static void scanMedia(void)
{
    mediaCount = 0;

    DIR *d = opendir(PHOTO_DIR);
    if (d == NULL)
        return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL && mediaCount < MAX_MEDIA)
    {
        size_t len = strlen(e->d_name);
        if (len > 8 && strncasecmp(e->d_name, "IMG_", 4) == 0
            && strcasecmp(e->d_name + len - 4, ".BMP") == 0)
        {
            media[mediaCount++] = (Media){ atoi(e->d_name + 4), false };
        }
        else if (len > 8 && strncasecmp(e->d_name, "VID_", 4) == 0
                 && strcasecmp(e->d_name + len - 4, ".AVI") == 0)
        {
            media[mediaCount++] = (Media){ atoi(e->d_name + 4), true };
        }
    }
    closedir(d);

    qsort(media, mediaCount, sizeof(Media), cmpMedia);
}

// ---------------------------------------------------------------------------
// Camera

// Stops the transfer without waiting for the frame.
static void abortCamera(void)
{
    REG_NDMA_CR(CAM_NDMA) &= ~NDMA_ENABLE;
    cameraStopTransfer();
    REG_CAM_CNT |= CAM_CNT_TRANSFER_FLUSH;
}

// A FIFO overrun stops the camera feeding the NDMA, which then never finishes. Happens on the front camera.
static bool cameraStalled(void)
{
    return ndmaBusy(CAM_NDMA)
        && (!cameraTransferActive() || (REG_CAM_CNT & CAM_CNT_TRANSFER_ERROR));
}

// Waits for the frame in flight unless it's stuck.
static bool waitCamera(int maxVblanks)
{
    for (int i = 0; i < maxVblanks && ndmaBusy(CAM_NDMA) && !cameraStalled(); i++)
        swiWaitForVBlank();
    return !ndmaBusy(CAM_NDMA);
}

static void stopPreview(void)
{
    waitCamera(20);
    abortCamera();
}

static void takePhoto(void)
{
    if (!sdOk)
    {
        status("No SD card: can't save photos");
        return;
    }
    if (mediaCount >= MAX_MEDIA)
    {
        status("Photo limit reached");
        return;
    }

    status("Capturing...");
    stopPreview(); // keeps the last frame on screen

    const size_t size = PHOTO_W * PHOTO_H * sizeof(u16);
    u16 *yuv = memalign(32, size);
    if (yuv == NULL)
    {
        status("Out of memory");
        return;
    }

    // NDMA bypasses the cache.
    DC_InvalidateRange(yuv, size);
    cameraStartTransfer(yuv, MCUREG_APT_SEQ_CMD_CAPTURE, CAM_NDMA);
    bool captured = waitCamera(120);
    abortCamera();
    DC_InvalidateRange(yuv, size);

    // Returning to preview restores the camera's own exposure, so re-apply ours.
    if (manualExp)
    {
        cameraSetCaptureMode(MCUREG_APT_SEQ_CMD_PREVIEW);
        applyShutter();
        applyGain();
    }

    if (!captured)
    {
        free(yuv);
        status("Capture failed, try again");
        return;
    }

    Media m = { nextMediaNumber(), false };
    char path[64];
    mediaPath(path, sizeof(path), &m);

    status("Saving...");
    bool ok = saveBmp(path, (const u8 *)yuv);
    free(yuv);

    if (ok)
    {
        addMedia(m);
        status("Saved IMG_%04d.BMP", m.num);
    }
    else
    {
        remove(path);
        status("Save failed!");
    }
}

// ---------------------------------------------------------------------------
// Video recording

static void clockStart(void)
{
    TIMER_CR(CLOCK_TIMER) = 0;
    TIMER_CR(CLOCK_TIMER + 1) = 0;
    TIMER_DATA(CLOCK_TIMER) = 0;
    TIMER_DATA(CLOCK_TIMER + 1) = 0;
    TIMER_CR(CLOCK_TIMER + 1) = TIMER_ENABLE | TIMER_CASCADE;
    TIMER_CR(CLOCK_TIMER) = TIMER_ENABLE | TIMER_DIV_1024;
}

static void clockStop(void)
{
    TIMER_CR(CLOCK_TIMER) = 0;
    TIMER_CR(CLOCK_TIMER + 1) = 0;
}

// Ticks (CLOCK_HZ) since clockStart().
static u32 clockNow(void)
{
    u16 hi, lo;
    do
    {
        hi = TIMER_DATA(CLOCK_TIMER + 1);
        lo = TIMER_DATA(CLOCK_TIMER);
    } while (hi != TIMER_DATA(CLOCK_TIMER + 1));
    return ((u32)hi << 16) | lo;
}

// cameraStartTransfer() for preview plus an NDMA interrupt. Register writes only, so safe in an interrupt.
// The camera must already be in preview mode.
static void recStartFrame(u16 *buf)
{
    DC_InvalidateRange(buf, VIDEO_BYTES);

    REG_CAM_CNT &= ~CAM_CNT_TRANSFER_ENABLE;
    REG_CAM_CNT &= ~0x200F;
    REG_CAM_CNT |= CAM_CNT_FORMAT_RGB | CAM_CNT_SCANLINES(4);
    REG_CAM_CNT |= CAM_CNT_TRANSFER_FLUSH;
    REG_CAM_CNT |= CAM_CNT_TRANSFER_ENABLE;

    REG_NDMA_SRC(CAM_NDMA) = (u32)&REG_CAM_DATA;
    REG_NDMA_DEST(CAM_NDMA) = (u32)buf;
    REG_NDMA_LENGTH(CAM_NDMA) = VIDEO_BYTES / 4;
    REG_NDMA_BLENGTH(CAM_NDMA) = 512;
    REG_NDMA_BDELAY(CAM_NDMA) = 2;
    REG_NDMA_CR(CAM_NDMA) = NDMA_SRC_FIX | NDMA_BLOCK_SCALER(4) | NDMA_START_CAMERA
                          | NDMA_IRQ_REQ | NDMA_ENABLE;
}

// NDMA interrupt: queue the frame, start the next.
static void recFrameDone(void)
{
    if (!recActive)
        return;

    recLastFrame = clockNow();
    if (REG_CAM_CNT & CAM_CNT_TRANSFER_ERROR)
    {
        recErrors++; // torn frame, recapture
    }
    else if (recCount < REC_SLOTS - 1)
    {
        recTime[recHead] = clockNow();
        recHead = (recHead + 1) % REC_SLOTS;
        recCount++;
    }
    else
    {
        recOverflows++; // SD behind, drop
    }

    recStartFrame(recSlot[recHead]);
}

// Bottom-up, red/blue swapped (AVI keeps blue in the low bits).
static void toAviFrame(const u16 *src, u32 *dst)
{
    for (int y = VIDEO_H - 1; y >= 0; y--)
    {
        const u32 *s = (const u32 *)(src + y * VIDEO_W);
        for (int x = 0; x < VIDEO_W / 2; x++)
        {
            u32 p = s[x]; // two pixels
            *dst++ = ((p & 0x001F001F) << 10) | ((p >> 10) & 0x001F001F) | (p & 0x03E003E0);
        }
    }
}

// Mic interrupt: queue the half buffer the ARM7 filled.
static void micDone(void *buf, int length)
{
    if (!micActive)
        return;

    DC_InvalidateRange(buf, length); // written by the ARM7
    if (micHead - micTail + length > MIC_RING_BYTES)
    {
        micOverflows++;
        return;
    }

    u32 ofs = micHead % MIC_RING_BYTES;
    u32 first = MIC_RING_BYTES - ofs < (u32)length ? MIC_RING_BYTES - ofs : (u32)length;
    memcpy(micRing + ofs, buf, first);
    memcpy(micRing, (u8 *)buf + first, length - first);
    micHead += length;
    micLastTime = clockNow();
}

static void startMic(void)
{
    micHead = micTail = 0;
    micOverflows = 0;
    micSkip = 0;
    DC_InvalidateRange(micBuf, sizeof(micBuf));
    micActive = true;
    if (!soundMicRecord(micBuf, sizeof(micBuf), MicFormat_12Bit, MIC_RATE, micDone))
        micActive = false;
    micStartTime = micLastTime = clockNow();
}

// Sound before the first video frame is dropped so they line up.
static bool micService(void)
{
    if (micSkip > 0)
    {
        u32 n = micHead - micTail < micSkip ? micHead - micTail : micSkip;
        micTail += n;
        micSkip -= n;
    }
    while (micHead != micTail)
    {
        u32 ofs = micTail % MIC_RING_BYTES;
        u32 n = micHead - micTail;
        if (n > MIC_RING_BYTES - ofs)
            n = MIC_RING_BYTES - ofs;
        if (!aviWriteAudio(&recAvi, micRing + ofs, n))
            return false;
        micTail += n;
    }
    return true;
}

static void freeRecBuffers(void)
{
    for (int i = 0; i < REC_SLOTS; i++)
    {
        free(recSlot[i]);
        recSlot[i] = NULL;
    }
    free(recChunk);
    recChunk = NULL;
}

static void startRecording(void)
{
    if (!sdOk)
    {
        status("No SD card: can't record");
        return;
    }
    if (mediaCount >= MAX_MEDIA)
    {
        status("Video limit reached");
        return;
    }

    stopPreview();

    bool ok = true;
    for (int i = 0; i < REC_SLOTS; i++)
        ok = ok && (recSlot[i] = memalign(32, VIDEO_BYTES)) != NULL;
    ok = ok && (recChunk = memalign(32, AVI_CHUNK_HEADER + VIDEO_BYTES)) != NULL;
    if (!ok)
    {
        freeRecBuffers();
        status("Out of memory");
        return;
    }

    recMedia = (Media){ nextMediaNumber(), true };
    mediaName(recName, sizeof(recName), &recMedia);
    char path[64];
    mediaPath(path, sizeof(path), &recMedia);
    if (!aviOpen(&recAvi, path, VIDEO_W, VIDEO_H, VIDEO_FPS, MIC_RATE_REAL))
    {
        if (recAvi.f)
            fclose(recAvi.f);
        recAvi.f = NULL;
        freeRecBuffers();
        status("Couldn't create %s", recName);
        return;
    }
    addMedia(recMedia); // the file exists even if recording fails

    recHead = recTail = recCount = 0;
    recOverflows = recErrors = recStalls = recRepeats = 0;
    recHaveBase = false;
    recNextSlot = 0;
    recLastStatus = -1;

    clockStart();
    startMic();
    recLastFrame = clockNow();

    // Already in preview mode, so the interrupt can restart with register writes.
    irqSet(IRQ_NDMA(CAM_NDMA), recFrameDone);
    irqEnable(IRQ_NDMA(CAM_NDMA));
    recActive = true;
    recStartFrame(recSlot[recHead]);

    btnRec.label = "STOP";
    drawButton(&btnRec);
    printf("\x1b[%d;0H%-31s", STATUS_ROW + 1, "");
    status("REC %s", recName);
}

// Restarts a stalled capture. The interrupt only fires on completed frames.
static void recWatchdog(void)
{
    int ime = enterCriticalSection();
    bool stuck = cameraStalled()
              || (ndmaBusy(CAM_NDMA) && clockNow() - recLastFrame > CLOCK_HZ / 2);
    if (stuck)
    {
        abortCamera();
        recStalls++;
        recLastFrame = clockNow();
        recStartFrame(recSlot[recHead]);
    }
    leaveCriticalSection(ime);
}

// Writes frames on a fixed VIDEO_FPS timeline: late frames leave repeats, extra frames are skipped.
// Returns why recording has to stop, or NULL.
static const char *recService(void)
{
    if (recActive)
        recWatchdog();

    while (recCount > 0)
    {
        u16 *frame = recSlot[recTail];
        u32 t = recTime[recTail];
        if (!recHaveBase)
        {
            recBase = t;
            recHaveBase = true;
            u32 samples = (u64)(t - micStartTime) * MIC_RATE_REAL / CLOCK_HZ;
            micSkip = samples * 2;
        }
        u32 slot = ((u64)(t - recBase) * VIDEO_FPS + CLOCK_HZ / 2) / CLOCK_HZ;

        bool ok = true;
        if (slot >= recNextSlot)
        {
            while (ok && recNextSlot < slot)
            {
                ok = aviWriteRepeat(&recAvi);
                recNextSlot++;
                recRepeats++;
            }

            dmaCopy(frame, topFb, VIDEO_BYTES);
            toAviFrame(frame, (u32 *)(recChunk + AVI_CHUNK_HEADER));
            ok = ok && aviWriteFrame(&recAvi, recChunk);
            recNextSlot++;
        }

        recTail = (recTail + 1) % REC_SLOTS;
        int ime = enterCriticalSection();
        recCount--;
        leaveCriticalSection(ime);

        if (!ok)
            return "SD write failed (card full?)";
        if (recAvi.pos + (recAvi.indexCount + 2) * 16 + VIDEO_BYTES * 2 > REC_MAX_BYTES)
            return "Size limit reached";
    }

    if (recHaveBase && !micService())
        return "SD write failed (card full?)";

    int secs = recNextSlot / VIDEO_FPS;
    if (secs != recLastStatus)
    {
        recLastStatus = secs;
        status("REC %d:%02d %uMB rep %d", secs / 60, secs % 60,
               (unsigned)(recAvi.pos >> 20), recRepeats);
    }
    return NULL;
}

// reason replaces the summary if recording stopped on its own.
static void stopRecording(const char *reason)
{
    // The frame in flight is discarded.
    recActive = false;
    abortCamera();
    irqDisable(IRQ_NDMA(CAM_NDMA));
    micActive = false;
    soundMicOff();

    if (reason == NULL)
        reason = recService(); // flush the queue

    // Write the measured mic rate so sound stays in sync.
    u32 span = micLastTime - micStartTime;
    if (span >= CLOCK_HZ)
    {
        u32 rate = (u64)(micHead / 2) * CLOCK_HZ / span;
        if (rate > MIC_RATE_REAL * 98 / 100 && rate < MIC_RATE_REAL * 102 / 100)
            aviSetSampleRate(&recAvi, rate);
    }

    int frames = recAvi.frames;
    bool ok = aviClose(&recAvi);
    freeRecBuffers();
    clockStop();

    btnRec.label = "REC";
    drawButton(&btnRec);

    int secs = frames / VIDEO_FPS;
    if (!ok)
        status("Error saving %s", recName);
    else if (reason)
        status("%s", reason);
    else
        status("%s %d:%02d rep %d", recName, secs / 60, secs % 60, recRepeats);

    if (recErrors || recStalls || recOverflows || micOverflows)
        printf("\x1b[%d;0Htorn %d stall %d SD %d mic %d", STATUS_ROW + 1, recErrors,
               recStalls, recOverflows, micOverflows);
}

// ---------------------------------------------------------------------------
// Video playback

static void playerSoundStop(void)
{
    if (player.soundId >= 0)
        soundKill(player.soundId);
    player.soundId = -1;
}

static void playerClose(void)
{
    playerSoundStop();
    if (player.f)
        fclose(player.f);
    free(player.buf);
    clockStop();
    memset(&player, 0, sizeof(player));
    player.soundId = -1;
}

static u32 playerTimeUs(void)
{
    return player.offsetUs + (u32)((u64)clockNow() * 1000000 / CLOCK_HZ);
}

// Cut-off recordings have unpatched sizes, so fall back to the file size.
static bool playerOpen(const char *path)
{
    playerClose();

    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;
    player.f = f;
    // Small: the sound reader seeks, frames bypass it.
    setvbuf(f, NULL, _IOFBF, 4 * 1024);

    fseek(f, 0, SEEK_END);
    u32 fileSize = ftell(f);

    u8 c[12];
    fseek(f, 0, SEEK_SET);
    if (fread(c, 1, 12, f) != 12 || memcmp(c, "RIFF", 4) != 0 || memcmp(c + 8, "AVI ", 4) != 0)
        return false;
    u32 end = get32(c + 4) + 8;
    if (end <= 12 || end > fileSize)
        end = fileSize;

    int width = 0, height = 0, bpp = 0, compression = -1;
    for (u32 pos = 12; pos + 12 <= end;)
    {
        fseek(f, pos, SEEK_SET);
        if (fread(c, 1, 12, f) != 12)
            break;
        u32 size = get32(c + 4);

        if (memcmp(c, "LIST", 4) == 0 && memcmp(c + 8, "hdrl", 4) == 0)
        {
            u8 h[1024];
            u32 len = size < 4 ? 0 : size - 4 < sizeof(h) ? size - 4 : sizeof(h);
            if (fread(h, 1, len, f) != len)
                return false;

            bool videoStream = false, audioStream = false;
            int stream = -1;
            for (u32 q = 0; q + 8 <= len;)
            {
                const u8 *ck = h + q;
                u32 ckSize = get32(ck + 4);
                if (memcmp(ck, "LIST", 4) == 0)
                {
                    q += 12;
                    continue;
                }
                if (memcmp(ck, "avih", 4) == 0 && q + 8 + 20 <= len)
                {
                    player.usPerFrame = get32(ck + 8);
                    player.frames = get32(ck + 8 + 16);
                }
                else if (memcmp(ck, "strh", 4) == 0 && q + 12 <= len)
                {
                    stream++;
                    videoStream = memcmp(ck + 8, "vids", 4) == 0;
                    audioStream = memcmp(ck + 8, "auds", 4) == 0;
                }
                else if (memcmp(ck, "strf", 4) == 0 && audioStream && player.sampleRate == 0
                         && q + 8 + 16 <= len)
                {
                    // Only mono 16-bit PCM
                    if (get16(ck + 8) == 1 && get16(ck + 8 + 2) == 1
                        && get16(ck + 8 + 14) == 16 && stream < 100)
                    {
                        player.sampleRate = get32(ck + 8 + 4);
                        snprintf(player.audioId, sizeof(player.audioId), "%02dwb", stream);
                    }
                }
                else if (memcmp(ck, "strf", 4) == 0 && videoStream && compression < 0
                         && q + 8 + 20 <= len)
                {
                    width = (s32)get32(ck + 8 + 4);
                    height = (s32)get32(ck + 8 + 8);
                    bpp = get16(ck + 8 + 14);
                    compression = get32(ck + 8 + 16);
                }
                q += 8 + ((ckSize + 1) & ~1);
            }
        }
        else if (memcmp(c, "LIST", 4) == 0 && memcmp(c + 8, "movi", 4) == 0)
        {
            player.moviPos = pos + 12;
            player.moviEnd = (size > 4 && pos + 8 + size <= end) ? pos + 8 + size : end;
            break;
        }

        if (size == 0 && memcmp(c, "LIST", 4) != 0)
            return false; // can't step past this
        pos += 8 + ((size + 1) & ~1);
    }

    player.bottomUp = height > 0;
    if (height < 0)
        height = -height;
    if (width != VIDEO_W || height != VIDEO_H || bpp != 16 || compression != 0
        || player.moviPos == 0 || player.usPerFrame == 0)
        return false;

    // Cut-off recordings have no frame count.
    if (player.frames == 0)
        player.frames = (player.moviEnd - player.moviPos) / (AVI_CHUNK_HEADER + VIDEO_BYTES);

    player.buf = memalign(32, VIDEO_BYTES);
    if (player.buf == NULL)
        return false;

    player.pos = player.audioPos = player.moviPos;
    player.lastStatus = -1;

    if (player.sampleRate < 1000 || player.sampleRate > 65535)
        player.sampleRate = 0;
    // Same rounding as libnds SOUND_FREQ(), so playerSoundPos() doesn't drift.
    if (player.sampleRate)
        player.hwDivider = (SOUND_CLOCK + ((player.sampleRate + 1) >> 1)) / player.sampleRate;
    return true;
}

// X1R5G5B5 to screen (red in the low bits, alpha set).
static void showAviFrame(const u16 *src, bool bottomUp)
{
    const u32 *s = (const u32 *)src;
    for (int y = 0; y < VIDEO_H; y++)
    {
        u32 *dst = (u32 *)(topFb + (bottomUp ? VIDEO_H - 1 - y : y) * 256);
        for (int x = 0; x < VIDEO_W / 2; x++)
        {
            u32 p = *s++; // two pixels
            dst[x] = ((p & 0x001F001F) << 10) | ((p >> 10) & 0x001F001F) | (p & 0x03E003E0)
                   | 0x80008000;
        }
    }
}

// Steps one frame, drawing it if show. Empty chunks repeat the last frame. False at the end.
static bool playerStep(bool show)
{
    while (player.pos + AVI_CHUNK_HEADER <= player.moviEnd)
    {
        u8 c[AVI_CHUNK_HEADER];
        fseek(player.f, player.pos, SEEK_SET);
        if (fread(c, 1, sizeof(c), player.f) != sizeof(c))
            return false;
        u32 size = get32(c + 4);

        if (memcmp(c, "LIST", 4) == 0) // step into 'rec ' lists
        {
            player.pos += 12;
            continue;
        }
        player.pos += AVI_CHUNK_HEADER + ((size + 1) & ~1);

        bool frameChunk = memcmp(c, "00db", 4) == 0 || memcmp(c, "00dc", 4) == 0;
        if (!frameChunk)
            continue; // JUNK, other streams

        if (show && size == VIDEO_BYTES)
        {
            if (fread(player.buf, 1, VIDEO_BYTES, player.f) != VIDEO_BYTES)
                return false;
            showAviFrame(player.buf, player.bottomUp);
        }
        return true;
    }
    return false;
}

// Sound hardware read position (absolute sample).
static u32 playerSoundPos(void)
{
    if (player.soundId < 0)
        return player.ringBase;
    u32 us = playerTimeUs() - player.soundStartUs;
    return player.ringBase + (u64)us * SOUND_CLOCK / ((u64)player.hwDivider * 1000000);
}

// Appends count samples from the file, or silence.
static bool ringAppend(u32 count, bool fromFile)
{
    while (count > 0)
    {
        u32 ofs = (player.audioWritten - player.ringBase) % PLAY_RING;
        u32 n = PLAY_RING - ofs < count ? PLAY_RING - ofs : count;
        if (!fromFile)
            memset(playRing + ofs, 0, n * 2);
        else if (fread(playRing + ofs, 2, n, player.f) != n)
            return false;
        DC_FlushRange(playRing + ofs, n * 2); // the sound hardware reads RAM
        player.audioWritten += n;
        count -= n;
    }
    return true;
}

// Keeps the ring PLAY_AHEAD_MS ahead. Sound chunks have their own file position.
static void playerSoundFill(void)
{
    if (player.sampleRate == 0)
        return;

    u32 played = playerSoundPos();
    u32 target = played + player.sampleRate * PLAY_AHEAD_MS / 1000;

    while (player.audioWritten < target)
    {
        if (player.audioEnd || player.audioPos + AVI_CHUNK_HEADER > player.moviEnd)
        {
            player.audioEnd = true;
            ringAppend(target - player.audioWritten, false); // silence after the end
            return;
        }

        u8 c[AVI_CHUNK_HEADER];
        fseek(player.f, player.audioPos, SEEK_SET);
        if (fread(c, 1, sizeof(c), player.f) != sizeof(c))
        {
            player.audioEnd = true;
            continue;
        }
        u32 size = get32(c + 4);
        if (memcmp(c, "LIST", 4) == 0)
        {
            player.audioPos += 12;
            continue;
        }

        u32 n = size / 2;
        if (memcmp(c, player.audioId, 4) == 0 && n <= PLAY_RING / 2)
        {
            if (player.audioWritten + n - played > PLAY_RING)
                return; // no room yet
            if (!ringAppend(n, true))
            {
                player.audioEnd = true;
                continue;
            }
        }
        player.audioPos += AVI_CHUNK_HEADER + ((size + 1) & ~1);
    }
}

static void playerStatus(void)
{
    int secs = (u64)player.frame * player.usPerFrame / 1000000;
    int total = (u64)player.frames * player.usPerFrame / 1000000;
    if (secs == player.lastStatus)
        return;
    player.lastStatus = secs;
    status("%d:%02d / %d:%02d", secs / 60, secs % 60, total / 60, total % 60);
}

// Moves unplayed sound to the ring start, where a new channel starts.
static void playerSoundRebase(u32 from)
{
    u32 count = player.audioWritten > from ? player.audioWritten - from : 0;
    if (count > PLAY_RING)
        count = PLAY_RING;
    for (u32 i = 0; i < count; i++)
        playRingTmp[i] = playRing[(from + i - player.ringBase) % PLAY_RING];
    memcpy(playRing, playRingTmp, count * 2);
    DC_FlushRange(playRing, count * 2);
    player.ringBase = from;
    player.audioWritten = from + count;
}

static void playerSetPlaying(bool playing)
{
    if (playing)
    {
        // Start sound and clock together.
        playerSoundFill();
        clockStart();
        if (player.sampleRate)
        {
            player.soundStartUs = player.offsetUs;
            player.soundId = soundPlaySample(playRing, SoundFormat_16Bit, sizeof(playRing),
                                             player.sampleRate, 127, 64, true, 0);
        }
    }
    else
    {
        u32 played = playerSoundPos();
        player.offsetUs = playerTimeUs();
        playerSoundStop();
        if (player.sampleRate)
            playerSoundRebase(played);
        clockStop();
    }
    player.playing = playing;
    btnPlay.label = playing ? "PAUSE" : "PLAY";
    drawButton(&btnPlay);
}

static void playerToggle(void)
{
    if (player.f == NULL)
        return;

    if (!player.playing && player.frame >= player.frames)
    {
        // Restart
        player.pos = player.moviPos;
        player.frame = 0;
        player.offsetUs = 0;
        player.audioPos = player.moviPos;
        player.audioEnd = false;
        player.audioWritten = player.ringBase = 0;
    }
    playerSetPlaying(!player.playing);
}

// Shows the frame due now, skipping late ones without reading them.
static void playerTick(void)
{
    if (!player.playing)
        return;

    u32 now = playerTimeUs();
    while ((u64)player.frame * player.usPerFrame <= now)
    {
        bool newest = (u64)(player.frame + 1) * player.usPerFrame > now;
        if (!playerStep(newest))
        {
            player.frames = player.frame; // header was wrong
            break;
        }
        player.frame++;
    }

    playerSoundFill();
    playerStatus();
    if (player.frame >= player.frames)
        playerSetPlaying(false);
}

static void showCurrent(void)
{
    playerClose();
    btnPlay.label = "PLAY";

    if (mediaCount == 0)
    {
        memset(topFb, 0, 256 * SCREEN_HEIGHT * sizeof(u16));
        return;
    }

    char path[64];
    mediaPath(path, sizeof(path), &media[current]);
    status("Loading...");

    if (media[current].video)
    {
        memset(topFb, 0, 256 * SCREEN_HEIGHT * sizeof(u16));
        if (playerOpen(path) && playerStep(true))
        {
            // First frame stays as a poster.
            player.pos = player.moviPos;
            playerStatus();
        }
        else
        {
            playerClose();
            status("Couldn't open video");
        }
    }
    else if (showBmp(path))
    {
        status("");
    }
    else
    {
        status("Couldn't open photo");
    }
}

static void halt(const char *msg)
{
    printf("\n%s\n\nPress START to exit.\n", msg);
    while (true)
    {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_START)
            exit(1);
    }
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    consoleDemoInit(); // bottom screen text console

    vramSetBankA(VRAM_A_MAIN_BG);
    videoSetMode(MODE_5_2D);
    int bg = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    topFb = bgGetGfxPtr(bg);

    if (!isDSiMode())
        halt("This app needs a DSi in DSi mode.");

    sdOk = fatInitDefault();
    if (sdOk)
    {
        mkdir("/DCIM", 0777);
        mkdir(PHOTO_DIR, 0777);
        scanMedia();
    }

    if (!cameraInit())
        halt("Camera init failed.");
    if (!cameraSelect(camDevice))
        halt("Camera select failed.");
    soundEnable();
    player.soundId = -1;

    Mode mode = MODE_CAMERA;
    const Button *dragging = NULL; // slider being dragged
    drawUi(mode);

    while (true)
    {
        swiWaitForVBlank();
        scanKeys();
        u32 down = keysDown();
        u32 held = keysHeld();

        touchPosition touch = { 0 };
        bool touched = down & KEY_TOUCH;
        if (held & KEY_TOUCH)
            touchRead(&touch);
        else
            dragging = NULL;

        if (down & KEY_START)
            break;

        if (mode == MODE_CAMERA)
        {
            // While recording, recService() updates the screen.
            if (recActive)
            {
                const char *err = recService();
                if (err)
                    stopRecording(err);
            }
            else
            {
                if (cameraStalled())
                    abortCamera();
                if (!ndmaBusy(CAM_NDMA))
                    cameraStartTransfer(topFb, MCUREG_APT_SEQ_CMD_PREVIEW, CAM_NDMA);
            }

            bool rec = recActive;
            if ((down & KEY_B) || (touched && hit(&btnRec, &touch)))
            {
                if (rec)
                    stopRecording(NULL);
                else
                    startRecording();
            }
            else if (!rec && ((down & (KEY_A | KEY_L | KEY_R))
                              || (touched && hit(&btnSnap, &touch))))
            {
                takePhoto();
            }
            else if (!rec && ((down & KEY_X) || (touched && hit(&btnExpo, &touch))))
            {
                setManualExposure(!manualExp);
                drawUi(mode);
            }
            else if (!rec && ((down & KEY_SELECT) || (touched && hit(&btnCam, &touch))))
            {
                switchCamera();
                drawUi(mode);
            }
            else if (!rec && ((down & KEY_Y) || (touched && hit(&btnGallery, &touch))))
            {
                stopPreview();
                mode = MODE_GALLERY;
                current = mediaCount - 1; // start on the newest
                if (current < 0)
                    current = 0;
                drawUi(mode);
                showCurrent();
            }
            else if (manualExp && (dragging || touched))
            {
                if (touched)
                {
                    if (hit(&sldShutter, &touch))
                        dragging = &sldShutter;
                    else if (hit(&sldGain, &touch))
                        dragging = &sldGain;
                }

                if (dragging)
                {
                    int pos = sliderPosAt(dragging, touch.px);
                    if (dragging == &sldShutter && pos != shutterPos)
                    {
                        shutterPos = pos;
                        applyShutter();
                        drawShutterSlider();
                    }
                    else if (dragging == &sldGain && pos != gainPos)
                    {
                        gainPos = pos;
                        applyGain();
                        drawGainSlider();
                    }
                }
            }
        }
        else
        {
            playerTick();

            int step = 0;
            if ((down & KEY_LEFT) || (touched && hit(&btnPrev, &touch)))
                step = -1;
            else if ((down & KEY_RIGHT) || (touched && hit(&btnNext, &touch)))
                step = 1;

            if (step != 0 && mediaCount > 0)
            {
                current = (current + step + mediaCount) % mediaCount;
                drawUi(mode);
                showCurrent();
            }
            else if (player.f && ((down & KEY_A) || (touched && hit(&btnPlay, &touch))))
            {
                playerToggle();
            }
            else if ((down & KEY_B) || (touched && hit(&btnBack, &touch)))
            {
                playerClose();
                mode = MODE_CAMERA;
                drawUi(mode);
            }
        }
    }

    if (recActive)
        stopRecording(NULL);
    playerClose();
    stopPreview();
    cameraDeinit();
    return 0;
}
