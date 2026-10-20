// DSi Camera: a barebones photo app.
//
// Top screen:    live camera preview / gallery viewer
// Bottom screen: touch buttons
//
// Photos are saved as 640x480 24-bit BMPs in /DCIM/100DSI00 on the SD card.
// Must be launched in DSi mode (e.g. from TWiLight Menu++ or Unlaunch).

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

#define CAM_NDMA    1
#define CAM_DEVICE  CAMERA_OUTER
#define PHOTO_DIR   "/DCIM/100DSI00"
#define PHOTO_W     640
#define PHOTO_H     480
#define MAX_PHOTOS  1000

// Manual exposure ranges. The sensor frame is 704 rows long (set by the
// libnds driver), so exposure can't be longer than about one frame.
#define SHUTTER_MIN_ROWS  1
#define SHUTTER_MAX_ROWS  700
// Gain is stored in units of 1/32 (32 = 1.0x).
#define GAIN_MIN_32       32
#define GAIN_MAX_32       256
#define SLIDER_MAX        255

typedef struct {
    int col, row, w, h; // in console cells (8x8 px)
    const char *label;
} Button;

// Camera screen
static const Button btnSnap    = { 2,  1, 28, 5, "TAKE PHOTO" };
static const Button btnGallery = { 2,  7, 13, 3, "GALLERY" };
static Button       btnExpo    = { 17, 7, 13, 3, "EXP: AUTO" };
static const Button sldShutter = { 2, 12, 28, 3, "" };
static const Button sldGain    = { 2, 17, 28, 3, "" };
// Gallery screen
static const Button btnPrev    = { 2,  8, 13,  5, "< PREV" };
static const Button btnNext    = { 17, 8, 13,  5, "NEXT >" };
static const Button btnBack    = { 2, 15, 28,  4, "BACK TO CAMERA" };

#define STATUS_ROW 21

typedef enum { MODE_CAMERA, MODE_GALLERY } Mode;

static u16 *topFb;              // 256x192 16-bit framebuffer on the top screen
static int photos[MAX_PHOTOS];  // photo numbers found on SD, ascending
static int photoCount;
static int current;             // index into photos[] while in gallery
static bool sdOk;

static bool manualExp;
static int shutterPos;          // slider positions, 0..SLIDER_MAX
static int gainPos;

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

static void photoPath(char *buf, size_t size, int num)
{
    snprintf(buf, size, PHOTO_DIR "/IMG_%04d.BMP", num);
}

// ---------------------------------------------------------------------------
// Exposure
//
// Both sliders are logarithmic, so each step is the same number of "stops".

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

// Gain register layout: bits 0-6 initial gain (1/32 units), bits 7-8 analog
// multiplier (1x/2x/4x/8x). Keep the initial gain in 1.0-2.0x and push the
// rest into the multiplier.
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

// The camera's MCU keeps its own copy of the exposure (ae.R9 = integration
// rows, VirtGain = gain in 1/16 units) and pushes it to the sensor every frame,
// so write the MCU variables as well as the sensor registers.

static void applyShutter(void)
{
    int rows = shutterRows();
    cameraMcuWriteTWL(CAM_DEVICE, MCUREG_APT_AE_R9, rows);
    cameraI2cWriteTWL(CAM_DEVICE, I2CREG_APT_COARSE_INTEGRATION_TIME, rows);
}

static void applyGain(void)
{
    int g = gain32();
    cameraMcuWriteTWL(CAM_DEVICE, MCUREG_APT_AE_CURRENT_VIRTUAL_GAIN, g / 2);
    cameraI2cWriteTWL(CAM_DEVICE, I2CREG_APT_ANALOG_GAIN_CODE_GLOBAL, encodeGain(g));
}

static void stopPreview(void);

// Per-state AE settings: 0 = off, 1 = fast settling, 2 = manual,
// 3 = continuous, 4 = fast settling + metering.
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

    u16 seq = cameraMcuReadTWL(CAM_DEVICE, MCUREG_APT_SEQ_MODE);
    if (manual)
        seq &= ~MCUREG_APT_SEQ_MODE_AE_ENABLE;
    else
        seq |= MCUREG_APT_SEQ_MODE_AE_ENABLE;
    cameraMcuWriteTWL(CAM_DEVICE, MCUREG_APT_SEQ_MODE, seq);

    // AE is also configured per sequencer state; turn it off in all of them
    // (so capture doesn't re-meter either), or restore what the driver set.
    for (int i = 0; i < 4; i++)
    {
        if (manual)
        {
            savedSeqAe[i] = cameraMcuReadTWL(CAM_DEVICE, seqAeRegs[i]);
            cameraMcuWriteTWL(CAM_DEVICE, seqAeRegs[i], 0);
        }
        else
        {
            cameraMcuWriteTWL(CAM_DEVICE, seqAeRegs[i], savedSeqAe[i]);
        }
    }

    // Ask the camera's sequencer to pick up the change, and wait for it.
    cameraMcuWriteTWL(CAM_DEVICE, MCUREG_APT_SEQ_CMD, MCUREG_APT_SEQ_CMD_REFRESH);
    for (int i = 0; i < 60 && cameraMcuReadTWL(CAM_DEVICE, MCUREG_APT_SEQ_CMD) != 0; i++)
        swiWaitForVBlank();

    if (manual)
    {
        // Start the sliders where auto-exposure left off.
        u16 rows = cameraMcuReadTWL(CAM_DEVICE, MCUREG_APT_AE_R9);
        u16 virtGain = cameraMcuReadTWL(CAM_DEVICE, MCUREG_APT_AE_CURRENT_VIRTUAL_GAIN);
        shutterPos = toSliderPos(rows, SHUTTER_MIN_ROWS, SHUTTER_MAX_ROWS);
        gainPos = toSliderPos(virtGain * 2, GAIN_MIN_32, GAIN_MAX_32);
        applyShutter();
        applyGain();
    }

    manualExp = manual;
    btnExpo.label = manual ? "EXP: MANUAL" : "EXP: AUTO";
}

static void drawSlider(const Button *s, const char *name, int pos, const char *value)
{
    printf("\x1b[%d;%dH%-10s%18s", s->row - 1, s->col, name, value);
    drawButton(s);

    // Track runs between the box's borders.
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

// Maps a touch x coordinate to a slider position.
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
        drawButton(&btnGallery);
        drawButton(&btnExpo);
        if (manualExp)
        {
            drawShutterSlider();
            drawGainSlider();
        }
        else
        {
            printf("\x1b[14;2HAuto exposure");
        }
        printf("\x1b[23;0HA/L/R:photo Y:gallery X:exp");
        if (!sdOk)
            status("No SD card: can't save photos");
    }
    else
    {
        printf("\x1b[1;1HGALLERY");
        if (photoCount > 0)
        {
            printf("\x1b[3;1H%d / %d", current + 1, photoCount);
            printf("\x1b[4;1HIMG_%04d.BMP", photos[current]);
        }
        else
        {
            printf("\x1b[3;1HNo photos yet");
        }
        drawButton(&btnPrev);
        drawButton(&btnNext);
        drawButton(&btnBack);
        printf("\x1b[23;0HLeft/Right: browse  B: back");
    }
}

// ---------------------------------------------------------------------------
// BMP saving / loading

static inline int clamp255(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

// Converts one row of YUV422 (Y1 Cb Y2 Cr) to BGR24, the BMP pixel order.
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
    const u32 rowBytes = PHOTO_W * 3; // 1920, already 4-byte aligned
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

    // BMP rows are stored bottom-up
    for (int y = PHOTO_H - 1; ok && y >= 0; y--)
    {
        yuvRowToBgr(yuv + y * PHOTO_W * 2, row, PHOTO_W);
        ok = fwrite(row, 1, rowBytes, f) == rowBytes;
    }

    return (fclose(f) == 0) && ok;
}

// Draws a 24-bit BMP scaled to fill the top screen.
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

    // Visit screen rows in file order so we always seek forwards.
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
// Photos on SD

static int cmpInt(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static void scanPhotos(void)
{
    photoCount = 0;

    DIR *d = opendir(PHOTO_DIR);
    if (d == NULL)
        return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL && photoCount < MAX_PHOTOS)
    {
        size_t len = strlen(e->d_name);
        if (len > 8 && strncasecmp(e->d_name, "IMG_", 4) == 0
            && strcasecmp(e->d_name + len - 4, ".BMP") == 0)
        {
            photos[photoCount++] = atoi(e->d_name + 4);
        }
    }
    closedir(d);

    qsort(photos, photoCount, sizeof(int), cmpInt);
}

// ---------------------------------------------------------------------------
// Camera

static void stopPreview(void)
{
    while (ndmaBusy(CAM_NDMA))
        swiWaitForVBlank();
    cameraStopTransfer();
}

static void takePhoto(void)
{
    if (!sdOk)
    {
        status("No SD card: can't save photos");
        return;
    }
    if (photoCount >= MAX_PHOTOS)
    {
        status("Photo limit reached");
        return;
    }

    status("Capturing...");
    stopPreview(); // leaves the last preview frame frozen on screen

    const size_t size = PHOTO_W * PHOTO_H * sizeof(u16);
    u16 *yuv = memalign(32, size);
    if (yuv == NULL)
    {
        status("Out of memory");
        return;
    }

    // NDMA writes straight to RAM, so keep the data cache out of the way.
    DC_InvalidateRange(yuv, size);
    cameraStartTransfer(yuv, MCUREG_APT_SEQ_CMD_CAPTURE, CAM_NDMA);
    while (ndmaBusy(CAM_NDMA))
        swiWaitForVBlank();
    cameraStopTransfer();
    DC_InvalidateRange(yuv, size);

    // Going back to preview makes the camera restore its own exposure, so
    // switch now and re-apply ours on top.
    if (manualExp)
    {
        cameraSetCaptureMode(MCUREG_APT_SEQ_CMD_PREVIEW);
        applyShutter();
        applyGain();
    }

    int num = photoCount > 0 ? photos[photoCount - 1] + 1 : 0;
    char path[64];
    photoPath(path, sizeof(path), num);

    status("Saving...");
    bool ok = saveBmp(path, (const u8 *)yuv);
    free(yuv);

    if (ok)
    {
        photos[photoCount++] = num;
        status("Saved IMG_%04d.BMP", num);
    }
    else
    {
        remove(path);
        status("Save failed!");
    }
}

static void showCurrent(void)
{
    if (photoCount == 0)
    {
        memset(topFb, 0, 256 * SCREEN_HEIGHT * sizeof(u16));
        return;
    }

    char path[64];
    photoPath(path, sizeof(path), photos[current]);
    status("Loading...");
    if (showBmp(path))
        status("");
    else
        status("Couldn't open photo");
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
        scanPhotos();
    }

    if (!cameraInit())
        halt("Camera init failed.");
    if (!cameraSelect(CAM_DEVICE))
        halt("Camera select failed.");

    Mode mode = MODE_CAMERA;
    const Button *dragging = NULL; // slider being dragged, if any
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
            // Keep a preview frame streaming into the top screen.
            if (!ndmaBusy(CAM_NDMA) || !cameraTransferActive())
                cameraStartTransfer(topFb, MCUREG_APT_SEQ_CMD_PREVIEW, CAM_NDMA);

            if ((down & (KEY_A | KEY_L | KEY_R)) || (touched && hit(&btnSnap, &touch)))
            {
                takePhoto();
            }
            else if ((down & KEY_X) || (touched && hit(&btnExpo, &touch)))
            {
                setManualExposure(!manualExp);
                drawUi(mode);
            }
            else if ((down & KEY_Y) || (touched && hit(&btnGallery, &touch)))
            {
                stopPreview();
                mode = MODE_GALLERY;
                current = photoCount - 1; // start on the newest photo
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
            int step = 0;
            if ((down & KEY_LEFT) || (touched && hit(&btnPrev, &touch)))
                step = -1;
            else if ((down & KEY_RIGHT) || (touched && hit(&btnNext, &touch)))
                step = 1;

            if (step != 0 && photoCount > 0)
            {
                current = (current + step + photoCount) % photoCount;
                drawUi(mode);
                showCurrent();
            }
            else if ((down & KEY_B) || (touched && hit(&btnBack, &touch)))
            {
                mode = MODE_CAMERA;
                drawUi(mode);
            }
        }
    }

    stopPreview();
    cameraDeinit();
    return 0;
}
