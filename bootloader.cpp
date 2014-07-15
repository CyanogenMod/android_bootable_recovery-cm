/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include <fs_mgr.h>
#include "bootloader.h"
#include "common.h"
#include "mtdutils/mtdutils.h"
#include "roots.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int get_bootloader_message_mtd(struct bootloader_message *out, const fstab_rec* v);
static int set_bootloader_message_mtd(const struct bootloader_message *in, const fstab_rec* v);
static int get_bootloader_message_block(struct bootloader_message *out, const fstab_rec* v);
static int set_bootloader_message_block(const struct bootloader_message *in, const fstab_rec* v);

#ifdef RECOVERY_CUSTOM_BCB
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <stdlib.h>
#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef max
#define max(a,b) ((a)<(b)?(b):(a))
#endif
#define PIPE_R 0
#define PIPE_W 1
struct buf
{
    size_t len;
    unsigned char* data;
};
static int childpid;
static void sig_child(int sig)
{
    pid_t pid;
    int status;
    pid = waitpid(childpid, &status, WNOHANG);
    if (pid == childpid) {
        childpid = -1;
    }
}
static int exec_child(const char** argv, struct buf* ibuf, struct buf* obuf)
{
    int rc = -1;
    int childin[2];
    int childout[2];

    childin[0] = childin[1] = -1;
    childout[0] = childout[1] = -1;
    if (pipe(childin) != 0) {
        goto bail;
    }
    if (pipe(childout) != 0) {
        goto bail;
    }
    signal(SIGCHLD, sig_child);
    childpid = fork();
    if (childpid < 0) {
        goto bail;
    }
    if (childpid == 0) {
        close(childin[PIPE_W]);
        close(childout[PIPE_R]);
        close(STDIN_FILENO);
        dup2(childin[PIPE_R], STDIN_FILENO);
        close(STDOUT_FILENO);
        dup2(childout[PIPE_W], STDOUT_FILENO);
        execve(argv[0], (char * const *)argv, NULL);
        exit(-1); /* NOTREACHED */
    }
    close(childin[PIPE_R]);
    childin[PIPE_R] = -1;
    close(childout[PIPE_W]);
    childout[PIPE_W] = -1;

    while (childpid != -1) {
        fd_set fdsr, fdsw;
        int fdmax = 0;
        FD_ZERO(&fdsr);
        if (ibuf) {
            FD_SET(childout[PIPE_R], &fdsr);
            fdmax = max(fdmax, childout[PIPE_R]);
        }
        FD_ZERO(&fdsw);
        if (obuf) {
            FD_SET(childin[PIPE_W], &fdsw);
            fdmax = max(fdmax, childin[PIPE_W]);
        }
        rc = select(fdmax+1, &fdsr, &fdsw, NULL, NULL);
        if (rc > 0) {
            if (FD_ISSET(childin[PIPE_W], &fdsw)) {
                rc = write(childin[PIPE_W], obuf->data, obuf->len);
                if (rc > 0) {
                    obuf->len -= rc;
                }
                if (rc <= 0 || obuf->len == 0) {
                    obuf = NULL;
                    close(childin[PIPE_W]);
                    childin[PIPE_W] = -1;
                }
            }
            if (FD_ISSET(childout[PIPE_R], &fdsr)) {
                rc = read(childout[PIPE_R], ibuf->data, ibuf->len);
                if (rc > 0) {
                    ibuf->len += rc;
                    //XXX: dont overflow ibuf->data
                }
                if (rc <= 0) {
                    ibuf = NULL;
                    close(childout[PIPE_R]);
                    childout[PIPE_R] = -1;
                }
            }
        }
    }
    rc = 0;

bail:
    signal(SIGCHLD, SIG_DFL);
    close(childout[PIPE_W]);
    close(childout[PIPE_R]);
    close(childin[PIPE_W]);
    close(childin[PIPE_R]);
    return rc;
}
#endif

int get_bootloader_message(struct bootloader_message *out) {
#ifdef RECOVERY_CUSTOM_BCB
    const char* argv[3];
    int argc = 0;
    struct buf buf;
    int rc;
    argv[argc++] = "/sbin/bcb";
    argv[argc++] = "--get";
    argv[argc++] = NULL;
    buf.len = 1024;
    buf.data = (unsigned char*)malloc(buf.len);
    memset(buf.data, 0, buf.len);
    rc = exec_child(argv, &buf, NULL);
    memcpy(out->recovery, buf.data, buf.len);
    free(buf.data);
    return rc;
#else
    fstab_rec* v = volume_for_path("/misc");
    if (v == NULL) {
      return -1;
    }
    if (strcmp(v->fs_type, "mtd") == 0) {
        return get_bootloader_message_mtd(out, v);
    } else if (strcmp(v->fs_type, "emmc") == 0) {
        return get_bootloader_message_block(out, v);
    }
    LOGE("unknown misc partition fs_type \"%s\"\n", v->fs_type);
    return -1;
#endif
}

int set_bootloader_message(const struct bootloader_message *in) {
#ifdef RECOVERY_CUSTOM_BCB
    const char* argv[3];
    int argc = 0;
    struct buf buf;
    int rc;
    argv[argc++] = "/sbin/bcb";
    argv[argc++] = "--set";
    argv[argc++] = NULL;
    buf.len = sizeof(in->recovery);
    buf.data = (unsigned char*)malloc(buf.len);
    memset(buf.data, 0, buf.len);
    memcpy(buf.data, in->recovery, sizeof(in->recovery));
    rc = exec_child(argv, NULL, &buf);
    free(buf.data);
    return rc;
#else
    fstab_rec* v = volume_for_path("/misc");
    if (v == NULL) {
      return -1;
    }
    if (strcmp(v->fs_type, "mtd") == 0) {
        return set_bootloader_message_mtd(in, v);
    } else if (strcmp(v->fs_type, "emmc") == 0) {
        return set_bootloader_message_block(in, v);
    }
    LOGE("unknown misc partition fs_type \"%s\"\n", v->fs_type);
    return -1;
#endif
}

// ------------------------------
// for misc partitions on MTD
// ------------------------------

static const int MISC_PAGES = 3;         // number of pages to save
static const int MISC_COMMAND_PAGE = 1;  // bootloader command is this page

static int get_bootloader_message_mtd(struct bootloader_message *out,
                                      const fstab_rec* v) {
    size_t write_size;
    mtd_scan_partitions();
    const MtdPartition *part = mtd_find_partition_by_name(v->blk_device);
    if (part == NULL || mtd_partition_info(part, NULL, NULL, &write_size)) {
        LOGE("Can't find %s\n", v->blk_device);
        return -1;
    }

    MtdReadContext *read = mtd_read_partition(part);
    if (read == NULL) {
        LOGE("Can't open %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }

    const ssize_t size = write_size * MISC_PAGES;
    char data[size];
    ssize_t r = mtd_read_data(read, data, size);
    if (r != size) LOGE("Can't read %s\n(%s)\n", v->blk_device, strerror(errno));
    mtd_read_close(read);
    if (r != size) return -1;

    memcpy(out, &data[write_size * MISC_COMMAND_PAGE], sizeof(*out));
    return 0;
}
static int set_bootloader_message_mtd(const struct bootloader_message *in,
                                      const fstab_rec* v) {
    size_t write_size;
    mtd_scan_partitions();
    const MtdPartition *part = mtd_find_partition_by_name(v->blk_device);
    if (part == NULL || mtd_partition_info(part, NULL, NULL, &write_size)) {
        LOGE("Can't find %s\n", v->blk_device);
        return -1;
    }

    MtdReadContext *read = mtd_read_partition(part);
    if (read == NULL) {
        LOGE("Can't open %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }

    ssize_t size = write_size * MISC_PAGES;
    char data[size];
    ssize_t r = mtd_read_data(read, data, size);
    if (r != size) LOGE("Can't read %s\n(%s)\n", v->blk_device, strerror(errno));
    mtd_read_close(read);
    if (r != size) return -1;

    memcpy(&data[write_size * MISC_COMMAND_PAGE], in, sizeof(*in));

    MtdWriteContext *write = mtd_write_partition(part);
    if (write == NULL) {
        LOGE("Can't open %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }
    if (mtd_write_data(write, data, size) != size) {
        LOGE("Can't write %s\n(%s)\n", v->blk_device, strerror(errno));
        mtd_write_close(write);
        return -1;
    }
    if (mtd_write_close(write)) {
        LOGE("Can't finish %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }

    LOGI("Set boot command \"%s\"\n", in->command[0] != 255 ? in->command : "");
    return 0;
}


// ------------------------------------
// for misc partitions on block devices
// ------------------------------------

static void wait_for_device(const char* fn) {
    int tries = 0;
    int ret;
    struct stat buf;
    do {
        ++tries;
        ret = stat(fn, &buf);
        if (ret) {
            printf("stat %s try %d: %s\n", fn, tries, strerror(errno));
            sleep(1);
        }
    } while (ret && tries < 10);
    if (ret) {
        printf("failed to stat %s\n", fn);
    }
}

static int get_bootloader_message_block(struct bootloader_message *out,
                                        const fstab_rec* v) {
    wait_for_device(v->blk_device);
    FILE* f = fopen(v->blk_device, "rb");
    if (f == NULL) {
        LOGE("Can't open %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }
#ifdef BOARD_RECOVERY_BLDRMSG_OFFSET
    fseek(f, BOARD_RECOVERY_BLDRMSG_OFFSET, SEEK_SET);
#endif
    struct bootloader_message temp;
    int count = fread(&temp, sizeof(temp), 1, f);
    if (count != 1) {
        LOGE("Failed reading %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }
    if (fclose(f) != 0) {
        LOGE("Failed closing %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }
    memcpy(out, &temp, sizeof(temp));
    return 0;
}

static int set_bootloader_message_block(const struct bootloader_message *in,
                                        const fstab_rec* v) {
    wait_for_device(v->blk_device);
    FILE* f = fopen(v->blk_device, "rb+");
    if (f == NULL) {
        LOGE("Can't open %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }
#ifdef BOARD_RECOVERY_BLDRMSG_OFFSET
    fseek(f, BOARD_RECOVERY_BLDRMSG_OFFSET, SEEK_SET);
#endif
    int count = fwrite(in, sizeof(*in), 1, f);
    if (count != 1) {
        LOGE("Failed writing %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }
    if (fclose(f) != 0) {
        LOGE("Failed closing %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }
    return 0;
}
