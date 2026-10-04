/*
 * Gamepads of SailOver's input service
 *
 * The service (sailover-input, a native macOS process) reads the Mac's gamepads with SDL3 and serves
 * them on a UNIX socket, WINE_SAILOVER_INPUT_SOCKET or /run/sailover/input.sock by default. Each
 * gamepad becomes a HID gamepad with the layout of the SDL bus's mapped controllers, which
 * winexinput.sys offers through XInput; rumble goes back to the service. Wire format:
 * sailover_input_protocol.h. Without a service the bus does not start.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "ddk/hidtypes.h"
#include "hidusage.h"

#include "wine/debug.h"
#include "wine/hid.h"
#include "wine/unixlib.h"

#include "unix_private.h"
#include "sailover_input_protocol.h"

WINE_DEFAULT_DEBUG_CHANNEL(hid);

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* guards the device list and the writes to the service */
static pthread_mutex_t sailover_cs = PTHREAD_MUTEX_INITIALIZER;
static struct list event_queue = LIST_INIT(event_queue);
static struct list device_list = LIST_INIT(device_list);

static int service_fd = -1;
static int control_pipe[2] = {-1, -1};
static BYTE message[sizeof(struct si_hdr) + SI_MAX_PAYLOAD];
static size_t message_len;

struct sailover_device
{
    struct unix_device unix_device;
    UINT gamepad_id;
    BOOL started;
};

static inline struct sailover_device *impl_from_unix_device(struct unix_device *iface)
{
    return CONTAINING_RECORD(iface, struct sailover_device, unix_device);
}

/* sailover_cs must be held */
static struct sailover_device *find_device(UINT gamepad_id)
{
    struct sailover_device *impl;

    LIST_FOR_EACH_ENTRY(impl, &device_list, struct sailover_device, unix_device.entry)
        if (impl->gamepad_id == gamepad_id) return impl;

    return NULL;
}

/* sailover_cs must be held */
static BOOL service_send(UINT type, const void *payload, UINT len)
{
    struct si_hdr hdr = {type, len};
    struct iovec iov[2] = {{&hdr, sizeof(hdr)}, {(void *)payload, len}};
    struct msghdr msg = {.msg_iov = iov, .msg_iovlen = 2};
    ssize_t ret;

    if (service_fd < 0) return FALSE;
    do ret = sendmsg(service_fd, &msg, MSG_NOSIGNAL);
    while (ret < 0 && errno == EINTR);
    if (ret == sizeof(hdr) + len) return TRUE;
    WARN("failed to send message %u to the input service: %s\n", type, ret < 0 ? strerror(errno) : "short write");
    return FALSE;
}

static void sailover_device_destroy(struct unix_device *iface)
{
}

static NTSTATUS sailover_device_start(struct unix_device *iface)
{
    struct sailover_device *impl = impl_from_unix_device(iface);

    pthread_mutex_lock(&sailover_cs);
    impl->started = TRUE;
    pthread_mutex_unlock(&sailover_cs);

    return STATUS_SUCCESS;
}

static void sailover_device_stop(struct unix_device *iface)
{
    struct sailover_device *impl = impl_from_unix_device(iface);

    pthread_mutex_lock(&sailover_cs);
    impl->started = FALSE;
    list_remove(&impl->unix_device.entry);
    pthread_mutex_unlock(&sailover_cs);
}

static NTSTATUS sailover_device_haptics_start(struct unix_device *iface, UINT duration_ms,
                                              USHORT rumble_intensity, USHORT buzz_intensity,
                                              USHORT left_intensity, USHORT right_intensity)
{
    struct sailover_device *impl = impl_from_unix_device(iface);
    struct si_rumble rumble =
    {
        .gamepad_id = impl->gamepad_id,
        .low_frequency = rumble_intensity,
        .high_frequency = buzz_intensity,
        .left_trigger = left_intensity,
        .right_trigger = right_intensity,
        .duration_ms = duration_ms,
    };
    BOOL sent;

    TRACE("iface %p, duration_ms %u, rumble_intensity %u, buzz_intensity %u, left_intensity %u, right_intensity %u.\n",
          iface, duration_ms, rumble_intensity, buzz_intensity, left_intensity, right_intensity);

    pthread_mutex_lock(&sailover_cs);
    sent = service_send(SI_RUMBLE, &rumble, sizeof(rumble));
    pthread_mutex_unlock(&sailover_cs);

    return sent ? STATUS_SUCCESS : STATUS_DEVICE_NOT_CONNECTED;
}

static NTSTATUS sailover_device_haptics_stop(struct unix_device *iface)
{
    return sailover_device_haptics_start(iface, 0, 0, 0, 0, 0);
}

static NTSTATUS sailover_device_physical_device_control(struct unix_device *iface, USAGE control)
{
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS sailover_device_physical_device_set_gain(struct unix_device *iface, BYTE percent)
{
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS sailover_device_physical_effect_control(struct unix_device *iface, BYTE index,
                                                        USAGE control, BYTE iterations)
{
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS sailover_device_physical_effect_update(struct unix_device *iface, BYTE index,
                                                       struct effect_params *params)
{
    return STATUS_NOT_SUPPORTED;
}

static const struct hid_device_vtbl sailover_device_vtbl =
{
    sailover_device_destroy,
    sailover_device_start,
    sailover_device_stop,
    sailover_device_haptics_start,
    sailover_device_haptics_stop,
    sailover_device_physical_device_control,
    sailover_device_physical_device_set_gain,
    sailover_device_physical_effect_control,
    sailover_device_physical_effect_update,
};

/* The SDL bus's button numbers for a mapped controller, by SDL gamepad button; -1 has none. */
static const int button_numbers[SI_BUTTON_COUNT] =
{
    [SI_BUTTON_SOUTH] = 0, [SI_BUTTON_EAST] = 1, [SI_BUTTON_WEST] = 2, [SI_BUTTON_NORTH] = 3,
    [SI_BUTTON_LEFT_SHOULDER] = 4, [SI_BUTTON_RIGHT_SHOULDER] = 5, [SI_BUTTON_BACK] = 6, [SI_BUTTON_START] = 7,
    [SI_BUTTON_LEFT_STICK] = 8, [SI_BUTTON_RIGHT_STICK] = 9,
    [SI_BUTTON_DPAD_UP] = 10, [SI_BUTTON_DPAD_RIGHT] = 11, [SI_BUTTON_DPAD_DOWN] = 12, [SI_BUTTON_DPAD_LEFT] = 13,
    [SI_BUTTON_GUIDE] = 16,
    [SI_BUTTON_MISC1] = -1, [SI_BUTTON_RIGHT_PADDLE1] = -1, [SI_BUTTON_LEFT_PADDLE1] = -1,
    [SI_BUTTON_RIGHT_PADDLE2] = -1, [SI_BUTTON_LEFT_PADDLE2] = -1, [SI_BUTTON_TOUCHPAD] = -1,
    [SI_BUTTON_MISC2] = -1, [SI_BUTTON_MISC3] = -1, [SI_BUTTON_MISC4] = -1, [SI_BUTTON_MISC5] = -1,
    [SI_BUTTON_MISC6] = -1,
};

static void set_report_from_state(struct unix_device *iface, const struct si_ev_state *state)
{
    BOOL up = !!(state->buttons & (1u << SI_BUTTON_DPAD_UP)), down = !!(state->buttons & (1u << SI_BUTTON_DPAD_DOWN));
    BOOL left = !!(state->buttons & (1u << SI_BUTTON_DPAD_LEFT)), right = !!(state->buttons & (1u << SI_BUTTON_DPAD_RIGHT));
    int i;

    for (i = 0; i < SI_AXIS_COUNT; i++)
    {
        LONG value = state->axes[i];
        if (i == SI_AXIS_LEFTY || i == SI_AXIS_RIGHTY) value = -value - 1; /* match XUSB / GIP protocol */
        hid_device_set_abs_axis(iface, i, value);
    }
    hid_device_set_hatswitch_x(iface, 0, right - left);
    hid_device_set_hatswitch_y(iface, 0, down - up);
    for (i = 0; i < SI_BUTTON_COUNT; i++)
    {
        if (button_numbers[i] < 0) continue;
        hid_device_set_button(iface, button_numbers[i], !!(state->buttons & (1u << i)));
    }
}

static NTSTATUS build_report_descriptor(struct unix_device *iface, BOOL rumble)
{
    static const USAGE_AND_PAGE device_usage = {.UsagePage = HID_USAGE_PAGE_GENERIC, .Usage = HID_USAGE_GENERIC_GAMEPAD};

    if (!hid_device_begin_report_descriptor(iface, &device_usage)) return STATUS_NO_MEMORY;
    if (!hid_device_add_gamepad(iface)) return STATUS_NO_MEMORY;
    if (rumble && !hid_device_add_haptics(iface)) return STATUS_NO_MEMORY;
    if (!hid_device_end_report_descriptor(iface)) return STATUS_NO_MEMORY;
    return STATUS_SUCCESS;
}

/* sailover_cs must be held */
static void sailover_add_device(const struct si_ev_added *added)
{
    struct device_desc desc =
    {
        .vid = added->vendor,
        .pid = added->product,
        .version = added->version,
        .input = -1,
        .uid = added->gamepad_id,
        .is_gamepad = TRUE,
        .manufacturer = {'S','a','i','l','O','v','e','r',0},
    };
    char name[sizeof(added->name)], serial[sizeof(added->serial)];
    struct sailover_device *impl;

    if (find_device(added->gamepad_id)) return;

    if (added->bus == SI_BUS_USB) desc.bus_type = BUS_TYPE_USB;
    else if (added->bus == SI_BUS_BLUETOOTH) desc.bus_type = BUS_TYPE_BLUETOOTH;
    memcpy(name, added->name, sizeof(name));
    name[sizeof(name) - 1] = 0;
    memcpy(serial, added->serial, sizeof(serial));
    serial[sizeof(serial) - 1] = 0;
    if (!serial[0]) snprintf(serial, sizeof(serial), "%08x", added->gamepad_id);
    ntdll_umbstowcs(name, strlen(name) + 1, desc.product, ARRAY_SIZE(desc.product));
    ntdll_umbstowcs(serial, strlen(serial) + 1, desc.serialnumber, ARRAY_SIZE(desc.serialnumber));

    TRACE("gamepad %u, desc %s.\n", added->gamepad_id, debugstr_device_desc(&desc));

    if (!(impl = hid_device_create(&sailover_device_vtbl, sizeof(struct sailover_device)))) return;
    impl->gamepad_id = added->gamepad_id;
    if (build_report_descriptor(&impl->unix_device, !!(added->caps & SI_CAP_RUMBLE)))
    {
        impl->unix_device.vtbl->destroy(&impl->unix_device);
        free(impl);
        return;
    }
    list_add_tail(&device_list, &impl->unix_device.entry);
    bus_event_queue_device_created(&event_queue, &impl->unix_device, &desc);
}

/* sailover_cs must be held */
static void process_message(const struct si_hdr *hdr, const BYTE *payload)
{
    struct sailover_device *impl;

    switch (hdr->type)
    {
    case SI_EV_HELLO:
    {
        const struct si_hello *hello = (const struct si_hello *)payload;
        if (hdr->len < sizeof(*hello)) break;
        if (hello->version == SI_PROTOCOL_VERSION) TRACE("input service pid %u\n", hello->pid);
        else
        {
            ERR("input service speaks version %u, this bus %u\n", hello->version, SI_PROTOCOL_VERSION);
            close(service_fd);
            service_fd = -1;
        }
        break;
    }
    case SI_EV_ADDED:
        if (hdr->len >= sizeof(struct si_ev_added)) sailover_add_device((const struct si_ev_added *)payload);
        break;
    case SI_EV_REMOVED:
    {
        const struct si_gamepad *gamepad = (const struct si_gamepad *)payload;
        if (hdr->len < sizeof(*gamepad)) break;
        if ((impl = find_device(gamepad->gamepad_id))) bus_event_queue_device_removed(&event_queue, &impl->unix_device);
        break;
    }
    case SI_EV_STATE:
    {
        const struct si_ev_state *state = (const struct si_ev_state *)payload;
        struct hid_device_state *report;
        if (hdr->len < sizeof(*state) || !(impl = find_device(state->gamepad_id))) break;
        /* a gamepad not yet started keeps its state for its first report */
        set_report_from_state(&impl->unix_device, state);
        if (!impl->started) break;
        report = &impl->unix_device.hid_device_state;
        bus_event_queue_input_report(&event_queue, &impl->unix_device, report->report_buf, report->report_len);
        break;
    }
    }
}

/* Reads what the service sent; when it is gone, its gamepads go with it. */
static void read_service(void)
{
    struct sailover_device *impl;
    struct si_hdr hdr;
    ssize_t ret;

    do ret = read(service_fd, message + message_len, sizeof(message) - message_len);
    while (ret < 0 && errno == EINTR);

    pthread_mutex_lock(&sailover_cs);
    if (ret <= 0)
    {
        WARN("lost the input service: %s\n", ret < 0 ? strerror(errno) : "closed");
        close(service_fd);
        service_fd = -1;
        LIST_FOR_EACH_ENTRY(impl, &device_list, struct sailover_device, unix_device.entry)
            bus_event_queue_device_removed(&event_queue, &impl->unix_device);
        pthread_mutex_unlock(&sailover_cs);
        return;
    }

    message_len += ret;
    while (service_fd >= 0 && message_len >= sizeof(hdr))
    {
        BYTE payload[SI_MAX_PAYLOAD] __attribute__((aligned(8)));
        size_t total;

        memcpy(&hdr, message, sizeof(hdr));
        if (hdr.len > SI_MAX_PAYLOAD)
        {
            ERR("input service sent a message of %u bytes\n", hdr.len);
            close(service_fd);
            service_fd = -1;
            break;
        }
        total = sizeof(hdr) + hdr.len;
        if (message_len < total) break;
        memcpy(payload, message + sizeof(hdr), hdr.len);
        memmove(message, message + total, message_len - total);
        message_len -= total;
        process_message(&hdr, payload);
    }
    pthread_mutex_unlock(&sailover_cs);
}

NTSTATUS sailover_bus_init(void *args)
{
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    struct si_hello hello = {SI_PROTOCOL_VERSION, getpid()};
    const char *path;
    BOOL sent;
    int fd;

    if (!(path = getenv("WINE_SAILOVER_INPUT_SOCKET"))) path = "/run/sailover/input.sock";
    if (strlen(path) >= sizeof(addr.sun_path)) return STATUS_NOT_SUPPORTED;
    strcpy(addr.sun_path, path);

    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) return STATUS_UNSUCCESSFUL;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        TRACE("no input service at %s: %s\n", path, strerror(errno));
        close(fd);
        return STATUS_NOT_SUPPORTED;
    }
#ifdef SO_NOSIGPIPE
    {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif
    if (pipe(control_pipe) < 0)
    {
        close(fd);
        return STATUS_UNSUCCESSFUL;
    }
    fcntl(control_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(control_pipe[1], F_SETFD, FD_CLOEXEC);

    pthread_mutex_lock(&sailover_cs);
    service_fd = fd;
    message_len = 0;
    sent = service_send(SI_HELLO, &hello, sizeof(hello));
    pthread_mutex_unlock(&sailover_cs);
    if (!sent)
    {
        close(service_fd);
        service_fd = -1;
        close(control_pipe[0]);
        close(control_pipe[1]);
        control_pipe[0] = control_pipe[1] = -1;
        return STATUS_UNSUCCESSFUL;
    }

    TRACE("connected to the input service at %s\n", path);
    return STATUS_SUCCESS;
}

NTSTATUS sailover_bus_wait(void *args)
{
    struct bus_event *result = args;
    struct pollfd fds[2];

    /* cleanup previously returned event */
    bus_event_cleanup(result);

    for (;;)
    {
        if (bus_event_queue_pop(&event_queue, result)) return STATUS_PENDING;

        fds[0].fd = control_pipe[0];
        fds[0].events = POLLIN;
        fds[1].fd = service_fd; /* ignored once the service is gone */
        fds[1].events = POLLIN;
        if (poll(fds, 2, -1) < 0)
        {
            if (errno == EINTR) continue;
            ERR("poll failed: %s\n", strerror(errno));
            break;
        }
        if (fds[0].revents) break;
        if (fds[1].revents) read_service();
    }

    TRACE("SailOver main loop exiting\n");
    bus_event_queue_destroy(&event_queue);
    pthread_mutex_lock(&sailover_cs);
    if (service_fd >= 0) close(service_fd);
    service_fd = -1;
    pthread_mutex_unlock(&sailover_cs);
    close(control_pipe[0]);
    close(control_pipe[1]);
    control_pipe[0] = control_pipe[1] = -1;
    return STATUS_SUCCESS;
}

NTSTATUS sailover_bus_stop(void *args)
{
    if (control_pipe[1] < 0) return STATUS_SUCCESS;
    if (write(control_pipe[1], "q", 1) != 1) WARN("failed to stop the SailOver bus: %s\n", strerror(errno));
    return STATUS_SUCCESS;
}
