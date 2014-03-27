/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

#include <cutils/android_reboot.h>
#include <cutils/properties.h>

#include "common.h"
#include "roots.h"
#include "device.h"
#include "minui/minui.h"
#include "screen_ui.h"
#include "ui.h"

#include "voldclient/voldclient.h"

#include "messagesocket.h"

#define UI_WAIT_KEY_TIMEOUT_SEC    120

/* Some extra input defines */
#ifndef ABS_MT_ANGLE
#define ABS_MT_ANGLE 0x38
#endif

#define DEBUG_TOUCH_EVENTS

static void show_event(struct input_event *ev)
{
#ifdef DEBUG_TOUCH_EVENTS
    char typebuf[40];
    char codebuf[40];
    const char *evtypestr = NULL;
    const char *evcodestr = NULL;

    sprintf(typebuf, "0x%04x", ev->type);
    evtypestr = typebuf;

    sprintf(codebuf, "0x%04x", ev->code);
    evcodestr = codebuf;

    switch (ev->type) {
    case EV_SYN:
        evtypestr = "EV_SYN";
        switch (ev->code) {
        case SYN_REPORT:
            evcodestr = "SYN_REPORT";
            break;
        case SYN_MT_REPORT:
            evcodestr = "SYN_MT_REPORT";
            break;
        }
        break;
    case EV_KEY:
        evtypestr = "EV_KEY";
        switch (ev->code) {
        case BTN_TOOL_FINGER:
            evcodestr = "BTN_TOOL_FINGER";
            break;
        case BTN_TOUCH:
            evcodestr = "BTN_TOUCH";
            break;
        }
        break;
    case EV_REL:
        evtypestr = "EV_REL";
        switch (ev->code) {
        case REL_X:
            evcodestr = "REL_X";
            break;
        case REL_Y:
            evcodestr = "REL_Y";
            break;
        case REL_Z:
            evcodestr = "REL_Z";
            break;
        }
        break;
    case EV_ABS:
        evtypestr = "EV_ABS";
        switch (ev->code) {
        case ABS_MT_TOUCH_MAJOR:
            evcodestr = "ABS_MT_TOUCH_MAJOR";
            break;
        case ABS_MT_TOUCH_MINOR:
            evcodestr = "ABS_MT_TOUCH_MINOR";
            break;
        case ABS_MT_WIDTH_MAJOR:
            evcodestr = "ABS_MT_WIDTH_MAJOR";
            break;
        case ABS_MT_WIDTH_MINOR:
            evcodestr = "ABS_MT_WIDTH_MINOR";
            break;
        case ABS_MT_ORIENTATION:
            evcodestr = "ABS_MT_ORIGENTATION";
            break;
        case ABS_MT_POSITION_X:
            evcodestr = "ABS_MT_POSITION_X";
            break;
        case ABS_MT_POSITION_Y:
            evcodestr = "ABS_MT_POSITION_Y";
            break;
        case ABS_MT_TRACKING_ID:
            evcodestr = "ABS_MT_TRACKING_ID";
            break;
        case ABS_MT_PRESSURE:
            evcodestr = "ABS_MT_PRESSURE";
            break;
        case ABS_MT_ANGLE:
            evcodestr = "ABS_MT_ANGLE";
            break;
        }
        break;
    }
    LOGI("show_event: type=%s, code=%s, val=%d\n", evtypestr, evcodestr, ev->value);
#endif
}

// There's only (at most) one of these objects, and global callbacks
// (for pthread_create, and the input event system) need to find it,
// so use a global variable.
static RecoveryUI* self = NULL;

static int string_split(char* s, char** fields, int maxfields)
{
    int n = 0;
    while (n+1 < maxfields) {
        char* p = strchr(s, ' ');
        if (!p)
            break;
        *p = '\0';
        printf("string_split: field[%d]=%s\n", n, s);
        fields[n++] = s;
        s = p+1;
    }
    fields[n] = s;
    printf("string_split: last field[%d]=%s\n", n, s);
    return n+1;
}

static int message_socket_client_event(int fd, short revents, void *data)
{
    MessageSocket* client = (MessageSocket*)data;

    printf("message_socket client event\n");
    if (!(revents & POLLIN)) {
        return 0;
    }

    char buf[256];
    ssize_t nread;
    nread = client->Read(buf, sizeof(buf));
    if (nread <= 0) {
        ev_del_fd(fd);
        self->DialogDismiss();
        client->Close();
        delete client;
        return 0;
    }

    printf("message_socket client message <%s>\n", buf);

    // Parse the message.  Right now we support:
    //   dialog show <string>
    //   dialog dismiss
    char* fields[3];
    int nfields;
    nfields = string_split(buf, fields, 3);
    printf("fields=%d\n", nfields);
    if (nfields < 2)
        return 0;
    printf("field[0]=%s, field[1]=%s\n", fields[0], fields[1]);
    if (strcmp(fields[0], "dialog") == 0) {
        if (strcmp(fields[1], "show") == 0 && nfields > 2) {
            self->DialogShowInfo(fields[2]);
        }
        if (strcmp(fields[1], "dismiss") == 0) {
            self->DialogDismiss();
        }
    }

    return 0;
}

static int message_socket_listen_event(int fd, short revents, void *data)
{
    MessageSocket* ms = (MessageSocket*)data;
    MessageSocket* client = ms->Accept();
    printf("message_socket_listen_event: event on %d\n", fd);
    if (client) {
        printf("message_socket client connected\n");
        ev_add_fd(client->fd(), message_socket_client_event, client);
    }
    return 0;
}

RecoveryUI::RecoveryUI() :
    key_queue_len(0),
    key_last_down(-1),
    key_long_press(false),
    key_down_count(0),
    consecutive_power_keys(0),
    consecutive_alternate_keys(0),
    last_key(-1),
    in_touch(0),
    in_swipe(0) {
    pthread_mutex_init(&key_queue_mutex, NULL);
    pthread_cond_init(&key_queue_cond, NULL);

    touch_start.x = touch_last.x = touch_end.x = -1;
    touch_start.y = touch_last.y = touch_end.y = -1;

    self = this;
}

void RecoveryUI::Init() {
    calibrate_swipe();
    ev_init(input_callback, NULL);
    message_socket.ServerInit();
    ev_add_fd(message_socket.fd(), message_socket_listen_event, &message_socket);
    pthread_create(&input_t, NULL, input_thread, NULL);
}


int RecoveryUI::input_callback(int fd, short revents, void* data)
{
    struct input_event ev;
    int ret;

    ret = ev_get_input(fd, revents, &ev);
    if (ret)
        return -1;

    self->process_touch(fd, &ev);

    if (ev.type == EV_SYN) {
        return 0;
    } else if (ev.type == EV_REL) {
        if (ev.code == REL_Y) {
            // accumulate the up or down motion reported by
            // the trackball.  When it exceeds a threshold
            // (positive or negative), fake an up/down
            // key event.
            self->rel_sum += ev.value;
            if (self->rel_sum > 3) {
                self->process_key(KEY_DOWN, 1);   // press down key
                self->process_key(KEY_DOWN, 0);   // and release it
                self->rel_sum = 0;
            } else if (self->rel_sum < -3) {
                self->process_key(KEY_UP, 1);     // press up key
                self->process_key(KEY_UP, 0);     // and release it
                self->rel_sum = 0;
            }
        }
    } else {
        self->rel_sum = 0;
    }

    if (ev.type == EV_KEY && ev.code <= KEY_MAX)
        self->process_key(ev.code, ev.value);

    return 0;
}

// Process a key-up or -down event.  A key is "registered" when it is
// pressed and then released, with no other keypresses or releases in
// between.  Registered keys are passed to CheckKey() to see if it
// should trigger a visibility toggle, an immediate reboot, or be
// queued to be processed next time the foreground thread wants a key
// (eg, for the menu).
//
// We also keep track of which keys are currently down so that
// CheckKey can call IsKeyPressed to see what other keys are held when
// a key is registered.
//
// updown == 1 for key down events; 0 for key up events
void RecoveryUI::process_key(int key_code, int updown) {
    bool register_key = false;
    bool long_press = false;

    pthread_mutex_lock(&key_queue_mutex);
    key_pressed[key_code] = updown;
    if (updown) {
        ++key_down_count;
        key_last_down = key_code;
        key_long_press = false;
        pthread_t th;
        key_timer_t* info = new key_timer_t;
        info->ui = this;
        info->key_code = key_code;
        info->count = key_down_count;
        pthread_create(&th, NULL, &RecoveryUI::time_key_helper, info);
        pthread_detach(th);
    } else {
        if (key_last_down == key_code) {
            long_press = key_long_press;
            register_key = true;
        }
        key_last_down = -1;
    }
    pthread_mutex_unlock(&key_queue_mutex);

    if (register_key) {
        NextCheckKeyIsLong(long_press);
        switch (CheckKey(key_code)) {
          case RecoveryUI::IGNORE:
            break;

          case RecoveryUI::TOGGLE:
            ShowText(!IsTextVisible());
            break;

          case RecoveryUI::REBOOT:
            vold_unmount_all();
            android_reboot(ANDROID_RB_RESTART, 0, 0);
            break;

          case RecoveryUI::ENQUEUE:
            EnqueueKey(key_code);
            break;

          case RecoveryUI::MOUNT_SYSTEM:
#ifndef NO_RECOVERY_MOUNT
            ensure_path_mounted("/system");
            Print("Mounted /system.");
#endif
            break;
        }
    }
}

void* RecoveryUI::time_key_helper(void* cookie) {
    key_timer_t* info = (key_timer_t*) cookie;
    info->ui->time_key(info->key_code, info->count);
    delete info;
    return NULL;
}

void RecoveryUI::time_key(int key_code, int count) {
    usleep(750000);  // 750 ms == "long"
    bool long_press = false;
    pthread_mutex_lock(&key_queue_mutex);
    if (key_last_down == key_code && key_down_count == count) {
        long_press = key_long_press = true;
    }
    pthread_mutex_unlock(&key_queue_mutex);
    if (long_press) KeyLongPress(key_code);
}

void RecoveryUI::calibrate_touch(int fd) {
    fb_dimensions.x = gr_fb_width();
    fb_dimensions.y = gr_fb_height();

    struct input_absinfo info;
    memset(&info, 0, sizeof(info));
    ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &info);
    touch_min.x = info.minimum;
    touch_max.x = info.maximum;
    memset(&info, 0, sizeof(info));
    ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &info);
    touch_min.y = info.minimum;
    touch_max.y = info.maximum;
    printf("touch_min=(%d,%d), touch_max=(%d,%d)\n", touch_min.x, touch_min.y, touch_max.x, touch_max.y);
}

void RecoveryUI::calibrate_swipe() {
    char strvalue[PROPERTY_VALUE_MAX];
    int  intvalue;
    property_get("ro.sf.lcd_density", strvalue, "160");
    intvalue = atoi(strvalue);
    int screen_density = (intvalue >= 160 ? intvalue : 160);
    min_swipe_px.x = screen_density * 50 / 100; // Roughly 0.5in
    min_swipe_px.y = screen_density * 30 / 100; // Roughly 0.3in
    printf("density=%d, min_swipe_x=%d, min_swipe_y=%d\n", screen_density, min_swipe_px.x, min_swipe_px.y);
}

int RecoveryUI::touch_scale_x(int val) {
    int scaled = val * fb_dimensions.x / (touch_max.x - touch_min.x);
    return scaled;
}

int RecoveryUI::touch_scale_y(int val) {
    int scaled = val * fb_dimensions.y / (touch_max.y - touch_min.y);
    return scaled;
}

void RecoveryUI::handle_press() {
printf("handle_press: (%d,%d) -> (%d,%d)\n",
        touch_start.x, touch_start.y,
        touch_end.x, touch_end.y);
}

void RecoveryUI::handle_release() {
    struct point diff;
    diff.x = touch_end.x - touch_start.x;
    diff.y = touch_end.y - touch_start.y;
printf("handle_release: (%d,%d) -> (%d,%d) d=(%d,%d)\n",
        touch_start.x, touch_start.y,
        touch_end.x, touch_end.y,
        diff.x, diff.y);

    printf("handle_release: showing=%d\n", DialogShowing());
    if (DialogShowing()) {
        if (DialogDismissable() && !in_swipe) {
            DialogDismiss();
        }
        return;
    }

    if (in_swipe) {
        if (abs(diff.x) > abs(diff.y)) {
            if (abs(diff.x) > min_swipe_px.x) {
                int key = (diff.x > 0 ? KEY_ENTER : KEY_BACK);
                process_key(key, 1);
                process_key(key, 0);
            }
        }
        else {
            /* Vertical swipe, handled realtime */
        }
    }
    else {
        int sel;
        sel = (touch_end.y - MenuItemStart())/MenuItemHeight();
        printf("sel: y=%d mis=%d mih=%d => %d\n", touch_end.y, MenuItemStart(), MenuItemHeight(), sel);
        SelectMenu(sel);
        usleep(50*1000);
        EnqueueKey(KEY_ABS_START + sel);
    }
}

void RecoveryUI::handle_gestures() {
    struct point diff;
    diff.x = touch_end.x - touch_start.x;
    diff.y = touch_end.y - touch_start.y;
printf("handle_gestures: (%d,%d) -> (%d,%d) d=(%d,%d)\n",
        touch_start.x, touch_start.y,
        touch_end.x, touch_end.y,
        diff.x, diff.y);

    if (touch_end.x == -1 || touch_end.y == -1) {
        return;
    }
    if (abs(diff.x) > abs(diff.y)) {
        if (abs(diff.x) > gr_fb_width()/4) {
            /* Horizontal swipe, handle it on release */
            in_swipe = 1;
        }
    }
    else {
        if (touch_last.y == -1) {
            touch_last.y = touch_end.y;
        }
        diff.y = touch_end.y - touch_last.y;
        if (abs(diff.y) > MenuItemHeight()) {
            in_swipe = 1;
            if (!DialogShowing()) {
                touch_last.y = touch_end.y;
                int key = (diff.y < 0) ? KEY_VOLUMEUP : KEY_VOLUMEDOWN;
                process_key(key, 1);
                process_key(key, 0);
            }
        }
    }
}

static int  touch_active_slot_count = 0;
static int  touch_first_slot = 0;
static int  touch_current_slot = 0;
static int  touch_tracking_id = -1;
static bool touch_saw_x = false;
static bool touch_saw_y = false;

void RecoveryUI::process_touch(int fd, struct input_event *ev) {

    show_event(ev);

    if (touch_max.x == 0 || touch_max.y == 0) {
        calibrate_touch(fd);
    }

    /*
     * Type A device release:
     *   1. Lack of position update
     *   2. BTN_TOUCH | ABS_PRESSURE | SYN_MT_REPORT
     *   3. SYN_REPORT
     *
     * Type B device release:
     *   1. ABS_MT_TRACKING_ID == -1 for "first" slot
     *   2. SYN_REPORT
     */

    if (ev->type == EV_SYN) {
printf("process_touch: in_touch=%d, in_swipe=%d\n", in_touch, in_swipe);
        if (ev->code == SYN_REPORT) {
            if (in_touch) {
                printf(" .. in_touch\n");
                /* Detect release */
                if (touch_active_slot_count == 0 && !touch_saw_x && !touch_saw_y) {
                    /* type A release */
                    printf("  type a release\n");
                    handle_release();
                    in_touch = 0;
                    in_swipe = 0;
                    touch_start.x = touch_last.x = touch_end.x = -1;
                    touch_start.y = touch_last.y = touch_end.y = -1;
                    touch_current_slot = touch_first_slot = 0;
                }
                else if (touch_current_slot == touch_first_slot && touch_tracking_id == -1) {
                    /* type B release */
                    printf("  type b release\n");
                    handle_release();
                    in_touch = 0;
                    in_swipe = 0;
                    touch_start.x = touch_last.x = touch_end.x = -1;
                    touch_start.y = touch_last.y = touch_end.y = -1;
                    touch_current_slot = touch_first_slot = 0;
                }
            }
            else {
                printf(" .. not in_touch\n");
                if (touch_saw_x && touch_saw_y) {
                    handle_press();
                    in_touch = 1;
                }
            }

            if (in_touch) {
                handle_gestures();
            }
        }
    }
    else if (ev->type == EV_ABS) {
        if (ev->code == ABS_MT_SLOT) {
            touch_current_slot = ev->value;
            if (touch_first_slot == -1) {
                touch_first_slot = touch_current_slot;
            }
            return;
        }
        if (ev->code == ABS_MT_TRACKING_ID) {
            touch_tracking_id = ev->value;
            if (touch_tracking_id == -1) {
                touch_active_slot_count--;
            }
            else {
                touch_active_slot_count++;
            }
            printf("tracking id %d, active %d\n", touch_tracking_id, touch_active_slot_count);
            return;
        }
        /*
         * For type A devices, we "lock" onto the first coordinates by ignoring
         * position updates from the time we see a SYN_MT_REPORT until the next
         * SYN_REPORT
         *
         * For type B devices, we "lock" onto the first slot seen until all slots
         * are released
         */
        if (touch_active_slot_count == 0) {
            /* type A */
            if (touch_saw_x && touch_saw_y) {
                return;
            }
        }
        else {
            if (touch_current_slot != touch_first_slot) {
                return;
            }
        }
        if (ev->code == ABS_MT_POSITION_X) {
            touch_saw_x = true;
            touch_end.x = touch_scale_x(ev->value);
            if (touch_start.x == -1)
                touch_start.x = touch_last.x = touch_end.x;
        }
        else if (ev->code == ABS_MT_POSITION_Y) {
            touch_saw_y = true;
            touch_end.y = touch_scale_y(ev->value);
            if (touch_start.y == -1)
                touch_start.y = touch_last.y = touch_end.y;
        }
    }
}

void RecoveryUI::EnqueueKey(int key_code) {
    if (DialogShowing()) {
        if (DialogDismissable()) {
            DialogDismiss();
        }
        return;
    }
    pthread_mutex_lock(&key_queue_mutex);
    const int queue_max = sizeof(key_queue) / sizeof(key_queue[0]);
    if (key_queue_len < queue_max) {
        key_queue[key_queue_len++] = key_code;
        pthread_cond_signal(&key_queue_cond);
    }
    pthread_mutex_unlock(&key_queue_mutex);
}


// Reads input events, handles special hot keys, and adds to the key queue.
void* RecoveryUI::input_thread(void *cookie)
{
    for (;;) {
        if (!ev_wait(-1))
            ev_dispatch();
    }
    return NULL;
}

void RecoveryUI::CancelWaitKey()
{
    pthread_mutex_lock(&key_queue_mutex);
    key_queue[key_queue_len] = -2;
    key_queue_len++;
    pthread_cond_signal(&key_queue_cond);
    pthread_mutex_unlock(&key_queue_mutex);
}

int RecoveryUI::WaitKey()
{
    pthread_mutex_lock(&key_queue_mutex);
    int timeouts = UI_WAIT_KEY_TIMEOUT_SEC;

    // Time out after UI_WAIT_KEY_TIMEOUT_SEC, unless a USB cable is
    // plugged in.
    do {
        struct timeval now;
        struct timespec timeout;
        gettimeofday(&now, NULL);
        timeout.tv_sec = now.tv_sec;
        timeout.tv_nsec = now.tv_usec * 1000;
        timeout.tv_sec += 1;

        int rc = 0;
        while (key_queue_len == 0 && rc != ETIMEDOUT) {
            rc = pthread_cond_timedwait(&key_queue_cond, &key_queue_mutex,
                                        &timeout);
            if (VolumesChanged()) {
                pthread_mutex_unlock(&key_queue_mutex);
                return Device::kRefresh;
            }
        }
        timeouts--;
    } while ((timeouts || usb_connected()) && key_queue_len == 0);

    int key = -1;
    if (key_queue_len > 0) {
        key = key_queue[0];
        memcpy(&key_queue[0], &key_queue[1], sizeof(int) * --key_queue_len);
    }
    pthread_mutex_unlock(&key_queue_mutex);
    return key;
}

// Return true if USB is connected.
bool RecoveryUI::usb_connected() {
    int fd = open("/sys/class/android_usb/android0/state", O_RDONLY);
    if (fd < 0) {
        printf("failed to open /sys/class/android_usb/android0/state: %s\n",
               strerror(errno));
        return 0;
    }

    char buf;
    /* USB is connected if android_usb state is CONNECTED or CONFIGURED */
    int connected = (read(fd, &buf, 1) == 1) && (buf == 'C');
    if (close(fd) < 0) {
        printf("failed to close /sys/class/android_usb/android0/state: %s\n",
               strerror(errno));
    }
    return connected;
}

bool RecoveryUI::IsKeyPressed(int key)
{
    pthread_mutex_lock(&key_queue_mutex);
    int pressed = key_pressed[key];
    pthread_mutex_unlock(&key_queue_mutex);
    return pressed;
}

void RecoveryUI::FlushKeys() {
    pthread_mutex_lock(&key_queue_mutex);
    key_queue_len = 0;
    pthread_mutex_unlock(&key_queue_mutex);
}

// The default CheckKey implementation assumes the device has power,
// volume up, and volume down keys.
//
// - Hold power and press vol-up to toggle display.
// - Press power seven times in a row to reboot.
// - Alternate vol-up and vol-down seven times to mount /system.
RecoveryUI::KeyAction RecoveryUI::CheckKey(int key) {
    if (IsKeyPressed(KEY_POWER) && key == KEY_VOLUMEUP) {
        return TOGGLE;
    }

    if (key == KEY_POWER) {
        ++consecutive_power_keys;
        if (consecutive_power_keys >= 7) {
            return REBOOT;
        }
    } else {
        consecutive_power_keys = 0;
    }

    if ((key == KEY_VOLUMEUP &&
         (last_key == KEY_VOLUMEDOWN || last_key == -1)) ||
        (key == KEY_VOLUMEDOWN &&
         (last_key == KEY_VOLUMEUP || last_key == -1))) {
        ++consecutive_alternate_keys;
        if (consecutive_alternate_keys >= 7) {
            consecutive_alternate_keys = 0;
            return MOUNT_SYSTEM;
        }
    } else {
        consecutive_alternate_keys = 0;
    }
    last_key = key;

    return ENQUEUE;
}

void RecoveryUI::NextCheckKeyIsLong(bool is_long_press) {
}

void RecoveryUI::KeyLongPress(int key) {
}

void RecoveryUI::NotifyVolumesChanged() {
    v_changed = 1;
}

bool RecoveryUI::VolumesChanged() {
    int ret = v_changed;
    if (v_changed > 0)
        v_changed = 0;
    return ret == 1;
}
