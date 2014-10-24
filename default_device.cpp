/*
 * Copyright (C) 2009 The Android Open Source Project
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

#include <linux/input.h>

#include "common.h"
#include "device.h"
#include "screen_ui.h"

#include "roots.h"

static const char* HEADERS[] = { "Swipe up/down to change selections;",
                                 "swipe right to select, or left to go back.",
                                 "",
                                 NULL };

static const char* ITEMS[] = { "Reboot system now",
                               "Apply update",
                               "Wipe data/factory reset",
                               "Wipe cache partition",
                               "Wipe media",
                               NULL };

class DefaultUI : public ScreenRecoveryUI {
  public:
    virtual KeyAction CheckKey(int key) {
        if (key == KEY_HOME) {
            return TOGGLE;
        }
        return ENQUEUE;
    }
};

extern int ui_root_menu;

class DefaultDevice : public Device {
  public:
    DefaultDevice() :
        ui(new DefaultUI) {
        // Remove "wipe media" option for non-datamedia devices
        if (!is_data_media()) {
            ITEMS[4] = NULL;
        }
    }

    RecoveryUI* GetUI() { return ui; }

    int HandleMenuKey(int key, int visible) {
        if (visible) {
            if (key & KEY_FLAG_ABS) {
                return key;
            }
            switch (key) {
              case KEY_RIGHTSHIFT:
              case KEY_DOWN:
              case KEY_VOLUMEDOWN:
              case KEY_MENU:
                return kHighlightDown;

              case KEY_LEFTSHIFT:
              case KEY_UP:
              case KEY_VOLUMEUP:
              case KEY_SEARCH:
                return kHighlightUp;

              case KEY_ENTER:
              case KEY_POWER:
              case BTN_MOUSE:
              case KEY_HOME:
              case KEY_HOMEPAGE:
              case KEY_SEND:
                return kInvokeItem;

              case KEY_BACKSPACE:
              case KEY_BACK:
                if (!ui_root_menu)
                  return kGoBack;
            }
        }

        return kNoAction;
    }

    BuiltinAction InvokeMenuItem(int menu_position) {
        if (menu_position < 0 ||
                menu_position >= (int)(sizeof(ITEMS)/sizeof(ITEMS[0])) ||
                ITEMS[menu_position] == NULL) {
            return NO_ACTION;
        }
        switch (menu_position) {
          case 0: return REBOOT;
          case 1: return APPLY_UPDATE;
          case 2: return WIPE_DATA;
          case 3: return WIPE_CACHE;
          case 4: return WIPE_MEDIA;
	  default: return NO_ACTION;
        }
    }

    const char* const* GetMenuHeaders() { return HEADERS; }
    const char* const* GetMenuItems() { return ITEMS; }

  private:
    RecoveryUI* ui;
};

Device* make_device() {
    return new DefaultDevice();
}
