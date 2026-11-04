/*
 * BananaOS / Anbernic RG DS: the second-screen panel on the bottom display (DSI-1).
 *
 * Included from port_second_screen.c in place of the Android surface plumbing (build with -DTMC_BANANA_PANEL).
 * The game keeps its normal SDL3 KMSDRM window on the top panel. SDL holds the DRM device and its master, so this
 * borrows that fd (found through /proc/self/fd, the same trick as the Lighthouse and UZDoom companion screens),
 * gives the bottom panel's connector its own free CRTC and a dumb buffer, and paints the existing compositor
 * (Port_SecondScreen_PaintInto) into it from a thread of its own at ~20 Hz. Touches are read straight from the
 * Goodix touchscreen (evdev) and fed to Port_SecondScreen_OnTap. Off unless BANANA_PANEL=1.
 *
 * Environment:  BANANA_PANEL=1          enable
 *               BANANA_PANEL_ID=1       DSI connector id of the bottom panel (default 1 = DSI-1)
 *               BANANA_TOUCH=/dev/input/eventN   touchscreen (default: first device named "*TouchScreen*")
 *               BANANA_TOUCH_SWAP=1     swap X/Y      BANANA_TOUCH_FLIPX=1 / BANANA_TOUCH_FLIPY=1   mirror an axis
 */

#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>
extern long syscall(long number, ...);
#include <xf86drm.h>
#include <xf86drmMode.h>

/* port_second_screen.c is compiled in strict C mode: declare the few POSIX calls the headers hide */
extern ssize_t readlink(const char* path, char* buf, size_t len);
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
extern int clock_gettime(clockid_t, struct timespec*);
extern int nanosleep(const struct timespec*, struct timespec*);

#define BP_LOG(...) do { fprintf(stderr, "[banana_panel] " __VA_ARGS__); fputc('\n', stderr); } while (0)

static struct {
    int fd;
    uint32_t crtc, conn, fb;
    drmModeModeInfo mode;
    int w, h;
    uint32_t pitch; /* bytes */
    size_t size;
    uint8_t* front; /* mapped scanout buffer */
    uint32_t* back; /* RGBA8888 painted by the compositor, w*h */
    volatile int up;
} sBp;

static pthread_t sBpThread;
static int sBpStarted;

static int BpFindDrmFd(void) {
    char path[64], buf[256];
    for (int fd = 3; fd < 256; fd++) {
        snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(path, buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            if (!strncmp(buf, "/dev/dri/card", 13)) {
                return fd;
            }
        }
    }
    return -1;
}

static int BpSetup(void) {
    int fd = BpFindDrmFd();
    if (fd < 0) {
        return 0; /* SDL has not opened the display yet: retry */
    }
    drmModeRes* res = drmModeGetResources(fd);
    if (!res) {
        BP_LOG("drmModeGetResources failed");
        return 0;
    }
    const char* e = getenv("BANANA_PANEL_ID");
    int want = e ? atoi(e) : 1;
    drmModeConnector* conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) {
            continue;
        }
        if (c->connector_type == DRM_MODE_CONNECTOR_DSI && (int)c->connector_type_id == want &&
            c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            conn = c;
        } else {
            drmModeFreeConnector(c);
        }
    }
    if (!conn) {
        BP_LOG("DSI-%d not found", want);
        drmModeFreeResources(res);
        return 0;
    }
    sBp.fd = fd;
    sBp.conn = conn->connector_id;
    sBp.mode = conn->modes[0];
    sBp.w = sBp.mode.hdisplay;
    sBp.h = sBp.mode.vdisplay;

    /* CRTCs used by the other connectors (SDL's top panel) are left alone */
    uint32_t busy = 0, possible = 0, cur = 0;
    for (int i = 0; i < res->count_connectors; i++) {
        if (res->connectors[i] == sBp.conn) {
            continue;
        }
        drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) {
            continue;
        }
        if (c->encoder_id) {
            drmModeEncoder* en = drmModeGetEncoder(fd, c->encoder_id);
            if (en) {
                for (int k = 0; k < res->count_crtcs; k++) {
                    if (res->crtcs[k] == en->crtc_id) {
                        busy |= 1u << k;
                    }
                }
                drmModeFreeEncoder(en);
            }
        }
        drmModeFreeConnector(c);
    }
    for (int i = 0; i < conn->count_encoders; i++) {
        drmModeEncoder* en = drmModeGetEncoder(fd, conn->encoders[i]);
        if (!en) {
            continue;
        }
        possible |= en->possible_crtcs;
        if (conn->encoder_id == en->encoder_id) {
            cur = en->crtc_id;
        }
        drmModeFreeEncoder(en);
    }
    sBp.crtc = 0;
    for (int k = 0; k < res->count_crtcs && !sBp.crtc; k++) {
        if ((possible & (1u << k)) && !(busy & (1u << k)) && (!cur || res->crtcs[k] == cur)) {
            sBp.crtc = res->crtcs[k];
        }
    }
    for (int k = 0; k < res->count_crtcs && !sBp.crtc; k++) {
        if ((possible & (1u << k)) && !(busy & (1u << k))) {
            sBp.crtc = res->crtcs[k];
        }
    }
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    if (!sBp.crtc) {
        BP_LOG("no free CRTC (possible 0x%x busy 0x%x)", possible, busy);
        return 0;
    }

    struct drm_mode_create_dumb cd;
    memset(&cd, 0, sizeof cd);
    cd.width = sBp.w;
    cd.height = sBp.h;
    cd.bpp = 32;
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) || drmModeAddFB(fd, sBp.w, sBp.h, 24, 32, cd.pitch, cd.handle, &sBp.fb)) {
        BP_LOG("dumb buffer / addfb failed");
        return 0;
    }
    struct drm_mode_map_dumb md;
    memset(&md, 0, sizeof md);
    md.handle = cd.handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) {
        BP_LOG("map dumb failed");
        return 0;
    }
    void* m = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (m == MAP_FAILED) {
        BP_LOG("mmap failed");
        return 0;
    }
    sBp.front = (uint8_t*)m;
    sBp.pitch = cd.pitch;
    sBp.size = cd.size;
    sBp.back = (uint32_t*)calloc((size_t)sBp.w * sBp.h, 4);
    memset(m, 0, cd.size);
    if (!sBp.back || drmModeSetCrtc(fd, sBp.crtc, sBp.fb, 0, 0, &sBp.conn, 1, &sBp.mode)) {
        BP_LOG("setcrtc failed");
        return 0;
    }
    BP_LOG("DSI-%d %dx%d on crtc %u", want, sBp.w, sBp.h, sBp.crtc);
    return 1;
}

/* ---- touch ---------------------------------------------------------------------------------------------------- */

static int BpOpenTouch(void) {
    const char* forced = getenv("BANANA_TOUCH");
    if (forced && *forced) {
        return open(forced, O_RDONLY | O_NONBLOCK);
    }
    for (int i = 0; i < 16; i++) {
        char path[64], name[128] = "";
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        if (strstr(name, "ouch")) {
            BP_LOG("touchscreen %s (%s)", path, name);
            return fd;
        }
        close(fd);
    }
    return -1;
}

typedef struct {
    int fd, minX, maxX, minY, maxY;
    int x, y, down, wasDown;
    struct timespec t0;
} BpTouch;

static void BpTouchInit(BpTouch* t) {
    memset(t, 0, sizeof *t);
    t->fd = BpOpenTouch();
    struct input_absinfo ai;
    if (t->fd >= 0 && !ioctl(t->fd, EVIOCGABS(ABS_MT_POSITION_X), &ai)) {
        t->minX = ai.minimum;
        t->maxX = ai.maximum;
    }
    if (t->fd >= 0 && !ioctl(t->fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai)) {
        t->minY = ai.minimum;
        t->maxY = ai.maximum;
    }
    if (t->fd >= 0) {
        BP_LOG("touch range x %d..%d y %d..%d", t->minX, t->maxX, t->minY, t->maxY);
    }
}

static void BpTouchPoll(BpTouch* t) {
    if (t->fd < 0) {
        return;
    }
    struct input_event ev;
    while (read(t->fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) {
            t->x = ev.value;
        } else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) {
            t->y = ev.value;
        } else if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID) {
            t->down = ev.value >= 0;
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            t->down = ev.value != 0;
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            if (t->down && !t->wasDown) {
                clock_gettime(CLOCK_MONOTONIC, &t->t0);
            }
            if (!t->down && t->wasDown) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long ms = (now.tv_sec - t->t0.tv_sec) * 1000 + (now.tv_nsec - t->t0.tv_nsec) / 1000000;
                double fx = t->maxX > t->minX ? (double)(t->x - t->minX) / (t->maxX - t->minX) : 0.0;
                double fy = t->maxY > t->minY ? (double)(t->y - t->minY) / (t->maxY - t->minY) : 0.0;
                if (getenv("BANANA_TOUCH_SWAP")) {
                    double s = fx;
                    fx = fy;
                    fy = s;
                }
                if (getenv("BANANA_TOUCH_FLIPX")) {
                    fx = 1.0 - fx;
                }
                if (getenv("BANANA_TOUCH_FLIPY")) {
                    fy = 1.0 - fy;
                }
                int px = (int)(fx * sBp.w), py = (int)(fy * sBp.h);
                BP_LOG("tap %d,%d (%ld ms)", px, py, ms);
                Port_SecondScreen_OnTap(px, py, ms >= 500);
            }
            t->wasDown = t->down;
        }
    }
}

/* ---- thread --------------------------------------------------------------------------------------------------- */

static void BpPresent(void) {
    SecondScreenSnapshot snap;
    static uint32_t sTick = 0;
    Port_SecondScreenState_Read(&snap);
    Port_SecondScreen_PaintInto(sBp.back, sBp.w, sBp.h, sBp.w, &snap, ++sTick);
    /* compositor: R,G,B,A bytes (u32 = A<<24|B<<16|G<<8|R); scanout XRGB8888 wants R and B swapped */
    for (int y = 0; y < sBp.h; y++) {
        const uint32_t* s = sBp.back + (size_t)y * sBp.w;
        uint32_t* d = (uint32_t*)(sBp.front + (size_t)y * sBp.pitch);
        for (int x = 0; x < sBp.w; x++) {
            uint32_t p = s[x];
            d[x] = ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
        }
    }
}

extern int Port_CaptureBaseFramebufferPNG(const char* path);

/* `touch /tmp/banana-shot` while the game runs: writes /tmp/banana-top.png (the game frame) and
 * /tmp/banana-bottom.ppm (this panel, RGB). Used for screenshots; costs one access() per frame otherwise. */
static void BpMaybeScreenshot(void) {
    if (access("/tmp/banana-shot", F_OK) != 0) {
        return;
    }
    unlink("/tmp/banana-shot");
    Port_CaptureBaseFramebufferPNG("/tmp/banana-top.png");
    FILE* f = fopen("/tmp/banana-bottom.ppm", "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", sBp.w, sBp.h);
    for (int y = 0; y < sBp.h; y++) {
        for (int x = 0; x < sBp.w; x++) {
            uint32_t p = sBp.back[(size_t)y * sBp.w + x];
            unsigned char rgb[3] = { (unsigned char)(p & 0xFF), (unsigned char)((p >> 8) & 0xFF),
                                     (unsigned char)((p >> 16) & 0xFF) };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    BP_LOG("screenshot written");
}

static void* BpThreadMain(void* arg) {
    (void)arg;
    while (!sBp.up) {
        if (BpSetup()) {
            sBp.up = 1;
        } else {
            struct timespec w = { 0, 500 * 1000 * 1000 };
            nanosleep(&w, NULL);
        }
    }
    BpTouch touch;
    BpTouchInit(&touch);
    /* The game thread (software PPU + audio mixing) is the one that must never stall: run the panel at low
     * priority and at ~12 Hz, and log its own cost so a slowdown can be told apart from the game's. */
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 15);
    double sumMs = 0, maxMs = 0;
    int frames = 0;
    for (;;) {
        BpTouchPoll(&touch);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        BpPresent();
        BpMaybeScreenshot();
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        sumMs += ms;
        if (ms > maxMs) maxMs = ms;
        if (++frames == 120) {
            BP_LOG("paint avg %.1f ms, max %.1f ms over %d frames", sumMs / frames, maxMs, frames);
            sumMs = maxMs = 0;
            frames = 0;
        }
        struct timespec w = { 0, 80 * 1000 * 1000 };
        nanosleep(&w, NULL);
    }
    return NULL;
}

void Port_SecondScreen_Init(void) {
    const char* on = getenv("BANANA_PANEL");
    if (!on || *on != '1' || sBpStarted) {
        return;
    }
    sBpStarted = 1;
    sBp.fd = -1;
    pthread_create(&sBpThread, NULL, BpThreadMain, NULL);
    pthread_detach(sBpThread);
}

void Port_SecondScreen_OnSurfaceReady(void* window, int width, int height) {
    (void)window;
    (void)width;
    (void)height;
}
void Port_SecondScreen_OnSurfaceLost(void) {}
int Port_SecondScreen_HasSurface(void) {
    return sBp.up;
}
