/*
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <linux/fb.h>
#include <linux/kd.h>

#include <linux/msm_mdp.h>
#include <linux/msm_ion.h>

#include "minui.h"
#include "graphics.h"

#define MDP_V4_0 400
#define PIXEL_SIZE 4
#define ALIGN(x, align) (((x) + ((align)-1)) & ~((align)-1))
#define FB_PATH "/sys/class/graphics/fb0/name"

static gr_surface overlay_init(minui_backend*);
static gr_surface overlay_flip(minui_backend*);
static void overlay_blank(minui_backend*, bool);
static void overlay_exit(minui_backend*);

static GRSurface gr_draw;
static struct fb_var_screeninfo vi;
static int fb_fd = -1;

static minui_backend overlay_backend = {
    .init = overlay_init,
    .flip = overlay_flip,
    .blank = overlay_blank,
    .exit = overlay_exit,
};

typedef struct {
    int size;
    int ion_fd;
    int mem_fd;
    struct ion_handle_data handle_data;
} ion_mem_info;

static int overlay_id = MSMFB_NEW_REQUEST;
static ion_mem_info mem_info;

static int map_mdp_pixel_format()
{
    int format = MDP_RGB_565;
#if defined(RECOVERY_BGRA)
    format = MDP_BGRA_8888;
#elif defined(RECOVERY_RGBX)
    format = MDP_RGBA_8888;
#endif
    return format;
}

bool target_has_overlay()
{
    bool ret = false;
    char version[32];
    char str_ver[4];
    int len = 0;
    int fd = open(FB_PATH, O_RDONLY);

    if (fd < 0)
        return false;

    if ((len = read(fd, version, 31)) >= 0) {
        version[len] = '\0';
    }
    close(fd);

    if (len >= 8) {
        if(!strncmp(version, "msmfb", strlen("msmfb"))) {
            memcpy(str_ver, version + strlen("msmfb"), 3);
            str_ver[3] = '\0';
            if (atoi(str_ver) >= MDP_V4_0) {
                ret = true;
            }
        } else if (!strncmp(version, "mdssfb", strlen("mdssfb"))) {
            ret = true;
        }
    }

    return ret;
}

minui_backend* open_overlay() {
    return &overlay_backend;
}

static int free_ion_mem(void) {
    int ret = 0;

    if (gr_draw.data)
        munmap(gr_draw.data, mem_info.size);

    if (mem_info.ion_fd >= 0) {
        ret = ioctl(mem_info.ion_fd, ION_IOC_FREE, &mem_info.handle_data);
        if (ret < 0)
            perror("free_mem failed ");
    }

    if (mem_info.mem_fd >= 0)
        close(mem_info.mem_fd);
    if (mem_info.ion_fd >= 0)
        close(mem_info.ion_fd);

    memset(&mem_info, 0, sizeof(mem_info));
    mem_info.mem_fd = -1;
    mem_info.ion_fd = -1;
    return 0;
}

static int alloc_ion_mem(unsigned int size)
{
    int result;
    struct ion_fd_data fd_data;
    struct ion_allocation_data ionAllocData;

    mem_info.ion_fd = open("/dev/ion", O_RDWR|O_DSYNC);
    if (mem_info.ion_fd < 0) {
        perror("ERROR: Can't open ion ");
        return -errno;
    }

    ionAllocData.flags = 0;
    ionAllocData.len = size;
    ionAllocData.align = sysconf(_SC_PAGESIZE);
    ionAllocData.heap_mask =
            ION_HEAP(ION_IOMMU_HEAP_ID) |
            ION_HEAP(ION_SYSTEM_CONTIG_HEAP_ID);

    result = ioctl(mem_info.ion_fd, ION_IOC_ALLOC,  &ionAllocData);
    if(result){
        perror("ION_IOC_ALLOC Failed ");
        close(mem_info.ion_fd);
        return result;
    }

    fd_data.handle = ionAllocData.handle;
    mem_info.handle_data.handle = ionAllocData.handle;
    result = ioctl(mem_info.ion_fd, ION_IOC_MAP, &fd_data);
    if (result) {
        perror("ION_IOC_MAP Failed ");
        free_ion_mem();
        return result;
    }
    gr_draw.data = (unsigned char *)mmap(NULL, size, PROT_READ |
                PROT_WRITE, MAP_SHARED, fd_data.fd, 0);
    mem_info.mem_fd = fd_data.fd;


    if (!gr_draw.data) {
        perror("ERROR: ION MAP_FAILED ");
        free_ion_mem();
        return -ENOMEM;
    }

    return 0;
}

static int allocate_overlay(int fd)
{
    // Check if overlay is already allocated
    if (MSMFB_NEW_REQUEST == overlay_id) {
        struct mdp_overlay overlay;
        int ret = 0;

        memset(&overlay, 0 , sizeof (struct mdp_overlay));

        /* Fill Overlay Data */

        overlay.src.width  = ALIGN(gr_draw.width, 32);
        overlay.src.height = gr_draw.height;
        overlay.src.format = map_mdp_pixel_format();
        overlay.src_rect.w = gr_draw.width;
        overlay.src_rect.h = gr_draw.height;
        overlay.dst_rect.w = gr_draw.width;
        overlay.dst_rect.h = gr_draw.height;
        overlay.alpha = 0xFF;
        overlay.transp_mask = MDP_TRANSP_NOP;
        overlay.id = MSMFB_NEW_REQUEST;
        ret = ioctl(fd, MSMFB_OVERLAY_SET, &overlay);
        if (ret < 0) {
            perror("Overlay Set Failed");
            return ret;
        }

        overlay_id = overlay.id;
    }

    return 0;
}

static int free_overlay(int fd)
{
    int ret = 0;
    struct mdp_display_commit ext_commit;

    if (overlay_id != MSMFB_NEW_REQUEST) {
        ret = ioctl(fd, MSMFB_OVERLAY_UNSET, &overlay_id);
        if (ret) {
            perror("Overlay Unset Failed");
            overlay_id = MSMFB_NEW_REQUEST;
            return ret;
        }

        memset(&ext_commit, 0, sizeof(struct mdp_display_commit));
        ext_commit.flags = MDP_DISPLAY_COMMIT_OVERLAY;
        ext_commit.wait_for_finish = 1;
        ret = ioctl(fd, MSMFB_DISPLAY_COMMIT, &ext_commit);
        if (ret < 0) {
            perror("ERROR: Clear MSMFB_DISPLAY_COMMIT failed!");
            overlay_id = MSMFB_NEW_REQUEST;
            return ret;
        }

        overlay_id = MSMFB_NEW_REQUEST;
    }
    return 0;
}

static int overlay_display_frame(int fd, size_t size)
{
    int ret = 0;
    struct msmfb_overlay_data ovdata;
    struct mdp_display_commit ext_commit;

    if (overlay_id == MSMFB_NEW_REQUEST) {
        perror("display_frame failed, no overlay\n");
        return -EINVAL;
    }

    memset(&ovdata, 0, sizeof(struct msmfb_overlay_data));

    ovdata.id = overlay_id;
    ovdata.data.flags = 0;
    ovdata.data.offset = 0;
    ovdata.data.memory_id = mem_info.mem_fd;
    ret = ioctl(fd, MSMFB_OVERLAY_PLAY, &ovdata);
    if (ret < 0) {
        perror("overlay_display_frame failed, overlay play Failed\n");
    } else {
        memset(&ext_commit, 0, sizeof(struct mdp_display_commit));
        ext_commit.flags = MDP_DISPLAY_COMMIT_OVERLAY;
        ext_commit.wait_for_finish = 1;
        ret = ioctl(fd, MSMFB_DISPLAY_COMMIT, &ext_commit);
        if (ret < 0) {
            perror("overlay_display_frame failed, overlay commit Failed\n!");
        }
    }

    return ret;
}

static gr_surface overlay_init(minui_backend* backend)
{
    int fd;
    void *bits = NULL;

    struct fb_fix_screeninfo fi;

    fd = open("/dev/graphics/fb0", O_RDWR);
    if (fd < 0) {
        perror("cannot open fb0");
        return NULL;
    }

    if (ioctl(fd, FBIOGET_FSCREENINFO, &fi) < 0) {
        perror("failed to get fb0 info");
        close(fd);
        return NULL;
    }

    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0) {
        perror("failed to get fb0 info");
        close(fd);
        return NULL;
    }

    printf("fb0 reports (possibly inaccurate):\n"
           "  vi.bits_per_pixel = %d\n"
           "  vi.red.offset   = %3d   .length = %3d\n"
           "  vi.green.offset = %3d   .length = %3d\n"
           "  vi.blue.offset  = %3d   .length = %3d\n",
           vi.bits_per_pixel,
           vi.red.offset, vi.red.length,
           vi.green.offset, vi.green.length,
           vi.blue.offset, vi.blue.length);

    fi.line_length = ALIGN(vi.xres, 32) * PIXEL_SIZE;

    gr_draw.width = vi.xres;
    gr_draw.height = vi.yres;
    gr_draw.row_bytes = fi.line_length;
    gr_draw.pixel_bytes = vi.bits_per_pixel / 8;

    fb_fd = fd;

    printf("overlay: %d (%d x %d)\n", fb_fd, gr_draw.width, gr_draw.height);

    overlay_blank(backend, true);
    overlay_blank(backend, false);

    if (alloc_ion_mem(fi.line_length * vi.yres) || allocate_overlay(fb_fd))
        free_ion_mem();

    return &gr_draw;
}

static gr_surface overlay_flip(minui_backend* backend __unused)
{
    if (overlay_display_frame(fb_fd, (gr_draw.row_bytes * gr_draw.height)) < 0) {
        // Free and allocate overlay in failure case
        // so that next cycle can be retried
        free_overlay(fb_fd);
        allocate_overlay(fb_fd);
    }
    return &gr_draw;
}

static void overlay_blank(minui_backend* backend __unused, bool blank) {
    if (blank)
        free_overlay(fb_fd);

    ioctl(fb_fd, FBIOBLANK, blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK);

    if (!blank)
        allocate_overlay(fb_fd);
}

static void overlay_exit(minui_backend* backend __unused) {
    free_overlay(fb_fd);
    free_ion_mem();
    close(fb_fd);
    fb_fd = -1;
}
