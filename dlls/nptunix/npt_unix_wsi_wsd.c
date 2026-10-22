#if 0
#pragma makedep unix
#endif
/*
 * Copyright 2026 Turing Software LLC
 * Copyright 2026 Obelisk
 * SPDX-License-Identifier: MIT
 *
 * wsd WSI for nptunix.so: replaces the X11 DRI3/Present WSI (npt_unix_wsi.c). Each swapchain image is a host
 * blob (POSIX shm created by the macOS renderer, received through vtest); its fd is attached once to the wsd
 * window of the HWND with BUFFER_ATTACH, so wsd samples the pages DXMT rendered into (no copy). The wsd
 * connection is the one of wineobelisk.drv in the same process, reached through its exported functions.
 * The x11_window field of npt_unix_wsi_init_params carries the wsd window id (HWND property
 * __wine_obelisk_window, set by the display driver).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "npt_unix_internal.h"
#include "npt_unixlib.h"
#include "wsd_protocol.h"

typedef int BOOL32;
static BOOL32 (*p_send)(unsigned int type, const void *payload, unsigned int len, int fd);
static void (*p_set_release_listener)(void (*)(unsigned int, unsigned int, uint64_t));
static void (*p_set_external)(unsigned int window_id, BOOL32 external);
static uint64_t (*p_next_serial)(void);
static void (*p_set_presented_listener)(void (*)(unsigned int, uint64_t, uint64_t));
static BOOL32 (*p_client_rect)(unsigned int window_id, int *x, int *y, unsigned int *w, unsigned int *h);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static unsigned int g_generation;

static struct {
   bool active;
   uint32_t window_id;
   uint32_t num_images;
   uint32_t width, height;
   uint32_t buffer_ids[NPT_WSI_MAX_IMAGES];
   uint32_t released[NPT_WSI_MAX_IMAGES];   /* EV_RELEASE count not yet reported by drain */
   uint64_t presents;
   uint64_t first_present_ns;
   uint32_t in_flight;          /* PRESENT_REGION sent, EV_PRESENTED not yet received */
   uint64_t latch_serial;       /* serial of the frame sent last, until wsd releases it (0: none) */
   /* statistics, logged every 300 presents (and the first one) */
   uint64_t stat_fence_timeouts, stat_fence_ns, stat_pace_ns, stat_last_ns, stat_interval_ns, stat_pace_timeouts, stat_no_fence;
   uint64_t stat_presented;     /* EV_PRESENTED received */
   int rx, ry; uint32_t rw, rh; /* last overlay rectangle */
} g_wsi;

static uint64_t now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static bool bind_driver(void)
{
   void *h;
   if (p_send) return true;
   if (!(h = dlopen("wineobelisk.so", RTLD_NOW | RTLD_NOLOAD))) {
      fprintf(stderr, "neptune unixlib wsi (wsd): wineobelisk.so is not loaded (%s)\n", dlerror());
      return false;
   }
   p_set_release_listener = dlsym(h, "obelisk_wsd_set_release_listener");
   p_set_external = dlsym(h, "obelisk_wsd_set_external");
   p_next_serial = dlsym(h, "obelisk_wsd_next_serial");
   p_set_presented_listener = dlsym(h, "obelisk_wsd_set_presented_listener");
   p_client_rect = dlsym(h, "obelisk_wsd_client_rect");
   p_send = dlsym(h, "obelisk_wsd_send");
   if (!p_send || !p_set_release_listener || !p_set_external || !p_next_serial || !p_set_presented_listener || !p_client_rect) {
      fprintf(stderr, "neptune unixlib wsi (wsd): wineobelisk.so lacks the wsd API\n");
      p_send = NULL;
      return false;
   }
   return true;
}

/* called on the display driver's event thread */
static void on_release(unsigned int buffer_id, unsigned int shown, uint64_t serial)
{
   (void)shown;
   pthread_mutex_lock(&g_lock);
   if (g_wsi.latch_serial && serial == g_wsi.latch_serial) {
      g_wsi.latch_serial = 0;
      pthread_cond_broadcast(&g_cond);
   }
   for (uint32_t i = 0; g_wsi.active && i < g_wsi.num_images; i++) {
      if (g_wsi.buffer_ids[i] == buffer_id) {
         g_wsi.released[i]++;
         pthread_cond_broadcast(&g_cond);
      }
   }
   pthread_mutex_unlock(&g_lock);
}

/* called on the display driver's event thread: one frame left the queue (shown or not) */
static void on_presented(unsigned int buffer_id, uint64_t serial, uint64_t presented_ns)
{
   (void)serial; (void)presented_ns;
   pthread_mutex_lock(&g_lock);
   for (uint32_t i = 0; g_wsi.active && i < g_wsi.num_images; i++) {
      if (g_wsi.buffer_ids[i] == buffer_id) g_wsi.stat_presented++;
      if (g_wsi.buffer_ids[i] == buffer_id && g_wsi.in_flight) {
         g_wsi.in_flight--;
         pthread_cond_broadcast(&g_cond);
      }
   }
   pthread_mutex_unlock(&g_lock);
}

static uint32_t wsd_format(uint32_t virgl_format)
{
   switch (virgl_format) {
   case 67: return WSD_FMT_RGBA8;    /* R8G8B8A8_UNORM */
   case 8: return WSD_FMT_RGB10A2;   /* R10G10B10A2_UNORM */
   case 1:                          /* B8G8R8A8_UNORM */
   case 2:                           /* B8G8R8X8_UNORM: wsd presents opaque */
   default: return WSD_FMT_BGRA8;
   }
}

static void detach_all(void)
{
   for (uint32_t i = 0; i < g_wsi.num_images; i++) {
      if (!g_wsi.buffer_ids[i]) continue;
      struct wsd_buffer b = { g_wsi.buffer_ids[i] };
      p_send(WSD_BUFFER_DETACH, &b, sizeof(b), -1);
   }
   if (g_wsi.window_id) {
      struct wsd_present_region r;
      memset(&r, 0, sizeof(r));
      r.present.window_id = g_wsi.window_id;
      p_send(WSD_PRESENT_REGION, &r, sizeof(r), -1);  /* 0x0: remove the overlay plane */
   }
}

NTSTATUS npt_do_wsi_init(void *args)
{
   struct npt_unix_wsi_init_params *p = args;
   p->result = -1;

   if (!p->num_images || p->num_images > NPT_WSI_MAX_IMAGES) return STATUS_SUCCESS;
   if (!bind_driver()) return STATUS_SUCCESS;
   if (!p->x11_window) {
      fprintf(stderr, "neptune unixlib wsi (wsd): no wsd window supplied\n");
      return STATUS_SUCCESS;
   }

   pthread_mutex_lock(&g_lock);
   if (g_wsi.active) detach_all();
   memset(&g_wsi, 0, sizeof(g_wsi));
   g_generation = (g_generation + 1) & 0x7ffffff;
   g_wsi.window_id = p->x11_window;
   g_wsi.num_images = p->num_images;
   g_wsi.width = p->width;
   g_wsi.height = p->height;
   pthread_mutex_unlock(&g_lock);
   p_set_release_listener(on_release);
   p_set_presented_listener(on_presented);

   for (uint32_t i = 0; i < p->num_images; i++) {
      struct wsd_buffer_attach a;
      if (p->fds[i] < 0) continue;
      memset(&a, 0, sizeof(a));
      a.buffer_id = 0x80000000u | (g_generation << 4) | i;
      a.width = p->width;
      a.height = p->height;
      a.stride = p->strides[i];
      a.format = wsd_format(p->virgl_format);
      a.offset = 0;
      a.size = (uint64_t)p->strides[i] * p->height;
      if (!p_send(WSD_BUFFER_ATTACH, &a, sizeof(a), p->fds[i])) {
         fprintf(stderr, "neptune unixlib wsi (wsd): BUFFER_ATTACH failed for image %u\n", i);
         continue;
      }
      g_wsi.buffer_ids[i] = a.buffer_id;
      fprintf(stderr, "neptune unixlib wsi (wsd): image %u -> buffer %#x (%ux%u stride %u format %u)\n",
              i, a.buffer_id, a.width, a.height, a.stride, a.format);
   }
   pthread_mutex_lock(&g_lock);
   g_wsi.active = true;
   pthread_mutex_unlock(&g_lock);
   fprintf(stderr, "neptune unixlib wsi (wsd): initialized for wsd window %u (%u images, %ux%u)\n",
           g_wsi.window_id, p->num_images, p->width, p->height);
   p->result = 0;
   return STATUS_SUCCESS;
}

/* Pacing follows the game's vsync: a Present with a SyncInterval uses "latch", one without uses "off".
 * "latch" sends each frame as soon as its fence signals, then waits until wsd has taken it at a display-link tick
 * (EV_RELEASE of that frame: copied into a drawable, or replaced), bounded by 100 ms. The game is held to the display
 * rate with one frame in the queue, while the macOS composition that follows (two or three refreshes) overlaps the
 * next frames. "off" never waits: wsd keeps only the newest unshown frame, as no tearing is possible.
 * NPT_WSD_PACING forces one mode for every frame: "latch", "off", "after" (waits until wsd has shown the frame,
 * EV_PRESENTED: the composition is then part of every frame, which caps a 120 Hz display at about 30 frames per
 * second) or "before" (a ready frame waits for the previous one to be shown, then is queued in wsd for the next
 * refresh: one more frame of input latency). */
static int forced_pacing_mode(void)
{
   static int mode = -2;
   if (mode == -2) {
      const char *e = getenv("NPT_WSD_PACING");
      mode = !e ? -1 : !strcmp(e, "latch") ? 3 : !strcmp(e, "after") ? 2 : !strcmp(e, "before") ? 1 : !strcmp(e, "off") ? 0 : -1;
      fprintf(stderr, "neptune unixlib wsi (wsd): pacing %s\n", mode == 3 ? "latch" : mode == 2 ? "after send" :
              mode == 1 ? "before send" : mode == 0 ? "off" : "from the game's vsync");
   }
   return mode;
}

static int pacing_mode(const struct npt_unix_wsi_present_params *p)
{
   int mode = forced_pacing_mode();
   return mode >= 0 ? mode : p->sync_interval ? 3 : 0;
}

static void deadline_in_ms(struct timespec *deadline, long ms)
{
   clock_gettime(CLOCK_REALTIME, deadline);
   deadline->tv_nsec += ms * 1000000;
   if (deadline->tv_nsec >= 1000000000) { deadline->tv_sec++; deadline->tv_nsec -= 1000000000; }
}

/* "latch": until wsd releases the frame sent last */
static void latch_wait(void)
{
   struct timespec deadline;
   int timedout = 0;
   uint64_t w0;
   deadline_in_ms(&deadline, 100);
   pthread_mutex_lock(&g_lock);
   w0 = now_ns();
   while (g_wsi.latch_serial && !timedout)
      timedout = pthread_cond_timedwait(&g_cond, &g_lock, &deadline) == ETIMEDOUT;
   if (timedout) g_wsi.stat_pace_timeouts++;
   g_wsi.stat_pace_ns += now_ns() - w0;
   g_wsi.latch_serial = 0;
   pthread_mutex_unlock(&g_lock);
}

static void pace_wait(void)
{
   {
      struct timespec deadline;
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_nsec += 100 * 1000000;
      if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
      pthread_mutex_lock(&g_lock);
      uint64_t w0 = now_ns();
      int timedout = 0;
      while (g_wsi.in_flight && !timedout)
         timedout = pthread_cond_timedwait(&g_cond, &g_lock, &deadline) == ETIMEDOUT;
      if (timedout) g_wsi.stat_pace_timeouts++;
      g_wsi.stat_pace_ns += now_ns() - w0;
      g_wsi.in_flight = 0;
      pthread_mutex_unlock(&g_lock);
   }

}

NTSTATUS npt_do_wsi_present(void *args)
{
   struct npt_unix_wsi_present_params *p = args;
   struct wsd_present pr;
   uint64_t t0 = now_ns();
   int mode;

   p->result = -1;
   p->wait_fence_consumed = 0;

   /* wsd has no fenced present yet: wait here for the renderer's fence (end of the frame on the host). */
   if (p->wait_fence_fd >= 0) {
      struct pollfd pfd = { .fd = p->wait_fence_fd, .events = POLLIN };
      int rc;
      /* Bounded: a fence that stays parked on the host must not freeze the swapchain (the frame is shown anyway,
       * possibly before the GPU finished it; counted in the statistics). */
      do rc = poll(&pfd, 1, 1000); while (rc < 0 && errno == EINTR);
      if (rc == 0) {
         g_wsi.stat_fence_timeouts++;
         fprintf(stderr, "neptune unixlib wsi (wsd): fence of present %llu not signaled after 1 s (image %u)\n",
                 (unsigned long long)g_wsi.presents + 1, p->image_index);
      }
      close(p->wait_fence_fd);
      p->wait_fence_consumed = 1;
   }
   else g_wsi.stat_no_fence++;
   g_wsi.stat_fence_ns += now_ns() - t0;
   if (!g_wsi.active || p->image_index >= g_wsi.num_images || !g_wsi.buffer_ids[p->image_index]) {
      fprintf(stderr, "neptune unixlib wsi (wsd): present of image %u ignored (active %d, %u images)\n",
              p->image_index, g_wsi.active, g_wsi.num_images);
      return STATUS_SUCCESS;
   }

   mode = pacing_mode(p);
   if (mode == 1) pace_wait();

   /* the swapchain covers the client area of the window (Wine draws the frame): present into an overlay plane */
   {
      struct wsd_present_region r;
      int x = 0, y = 0; unsigned int w = g_wsi.width, h = g_wsi.height;
      if (!p_client_rect(g_wsi.window_id, &x, &y, &w, &h)) { x = y = 0; w = g_wsi.width; h = g_wsi.height; }
      memset(&r, 0, sizeof(r));
      r.present.window_id = g_wsi.window_id;
      r.present.buffer_id = g_wsi.buffer_ids[p->image_index];
      r.present.serial = p_next_serial();
      r.present.client_time_ns = now_ns();
      r.x = x; r.y = y; r.width = w; r.height = h;
      pr = r.present;
      pthread_mutex_lock(&g_lock);
      g_wsi.in_flight++;
      if (mode == 3) g_wsi.latch_serial = r.present.serial;
      pthread_mutex_unlock(&g_lock);
      if (!p_send(WSD_PRESENT_REGION, &r, sizeof(r), -1)) {
         pthread_mutex_lock(&g_lock);
         g_wsi.latch_serial = 0;
         pthread_mutex_unlock(&g_lock);
         return STATUS_SUCCESS;
      }
   }
   if (mode == 3) latch_wait();
   else if (mode == 2) pace_wait();

   if (!g_wsi.presents++) g_wsi.first_present_ns = pr.client_time_ns;
   if (g_wsi.stat_last_ns) g_wsi.stat_interval_ns += pr.client_time_ns - g_wsi.stat_last_ns;
   g_wsi.stat_last_ns = pr.client_time_ns;
   if (g_wsi.presents <= 3 || g_wsi.presents % 60 == 0) {
      uint64_t n = g_wsi.presents <= 3 ? 1 : (g_wsi.presents == 60 ? 57 : 60);
      fprintf(stderr, "neptune unixlib wsi (wsd): %llu presents (%llu shown events), last %llu: interval avg %.3f ms, "
              "fence wait avg %.3f ms, pacing wait avg %.3f ms, pacing timeouts %llu, presents without fence %llu, fence timeouts %llu\n",
              (unsigned long long)g_wsi.presents, (unsigned long long)g_wsi.stat_presented, (unsigned long long)n,
              g_wsi.stat_interval_ns / (double)n / 1e6, g_wsi.stat_fence_ns / (double)n / 1e6,
              g_wsi.stat_pace_ns / (double)n / 1e6, (unsigned long long)g_wsi.stat_pace_timeouts,
              (unsigned long long)g_wsi.stat_no_fence, (unsigned long long)g_wsi.stat_fence_timeouts);
      g_wsi.stat_interval_ns = g_wsi.stat_fence_ns = g_wsi.stat_pace_ns = 0;
      g_wsi.stat_pace_timeouts = g_wsi.stat_no_fence = 0;
   }
   p->result = 0;
   return STATUS_SUCCESS;
}

NTSTATUS npt_do_wsi_drain(void *args)
{
   static uint64_t drains, last_log_ns;
   struct npt_unix_wsi_drain_params *p = args;
   struct timespec deadline;
   bool any = false;

   for (uint32_t i = 0; i < NPT_WSI_MAX_IMAGES; i++) p->released_count[i] = 0;
   p->result = -1;
   if (!g_wsi.active) return STATUS_SUCCESS;

   clock_gettime(CLOCK_REALTIME, &deadline);
   deadline.tv_sec += p->timeout_ms / 1000;
   deadline.tv_nsec += (long)(p->timeout_ms % 1000) * 1000000;
   if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }

   pthread_mutex_lock(&g_lock);
   for (;;) {
      for (uint32_t i = 0; i < g_wsi.num_images; i++) if (g_wsi.released[i]) any = true;
      if (any || !p->timeout_ms) break;
      if (pthread_cond_timedwait(&g_cond, &g_lock, &deadline) == ETIMEDOUT) break;
   }
   for (uint32_t i = 0; i < g_wsi.num_images; i++) {
      p->released_count[i] = g_wsi.released[i];
      g_wsi.released[i] = 0;
   }
   drains++;
   if (now_ns() - last_log_ns > 10000000000ull) {
      /* once every 10 s: shows whether the swapchain still presents at all */
      last_log_ns = now_ns();
      fprintf(stderr, "neptune unixlib wsi (wsd): alive: %llu presents, %llu shown events, %llu drains, in flight %u\n",
              (unsigned long long)g_wsi.presents, (unsigned long long)g_wsi.stat_presented,
              (unsigned long long)drains, g_wsi.in_flight);
   }
   pthread_mutex_unlock(&g_lock);
   p->result = 0;
   return STATUS_SUCCESS;
}

NTSTATUS npt_do_wsi_destroy(void *args)
{
   (void)args;
   if (!g_wsi.active) return STATUS_SUCCESS;
   detach_all();
   pthread_mutex_lock(&g_lock);
   memset(&g_wsi, 0, sizeof(g_wsi));
   pthread_mutex_unlock(&g_lock);
   return STATUS_SUCCESS;
}
