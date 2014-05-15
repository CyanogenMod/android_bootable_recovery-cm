/*
 * Copyright (C) 2007 The Android Open Source Project
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
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>

#include <fs_mgr.h>
#include "mtdutils/mtdutils.h"
#include "mtdutils/mounts.h"
#include "roots.h"
#include "common.h"
#include "make_ext4fs.h"

#include "voldclient/voldclient.h"

static struct fstab *fstab = NULL;

extern struct selabel_handle *sehandle;

static int mkdir_p(const char* path, mode_t mode)
{
    char dir[PATH_MAX];
    char* p;
    strcpy(dir, path);
    for (p = strchr(&dir[1], '/'); p != NULL; p = strchr(p+1, '/')) {
        *p = '\0';
        if (mkdir(dir, mode) != 0 && errno != EEXIST) {
            return -1;
        }
        *p = '/';
    }
    if (mkdir(dir, mode) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static void write_fstab_entry(fstab_rec *v, FILE *file)
{
    if (NULL != v && strcmp(v->fs_type, "mtd") != 0 && strcmp(v->fs_type, "emmc") != 0
                  && strcmp(v->fs_type, "bml") != 0 && !fs_mgr_is_voldmanaged(v)
                  && strncmp(v->blk_device, "/", 1) == 0
                  && strncmp(v->mount_point, "/", 1) == 0) {

        fprintf(file, "%s ", v->blk_device);
        fprintf(file, "%s ", v->mount_point);
        // special case rfs cause auto will mount it as vfat on samsung.
        fprintf(file, "%s defaults\n", v->fs_type2 != NULL && strcmp(v->fs_type, "rfs") != 0 ? "auto" : v->fs_type);
    }
}

int get_num_volumes() {
    return fstab->num_entries;
}

fstab_rec* get_device_volumes() {
    return fstab->recs;
}

static int is_datamedia;
int is_data_media()
{
    return is_datamedia;
}

void load_volume_table()
{
    int i;
    int ret;

    fstab = fs_mgr_read_fstab("/etc/recovery.fstab");
    if (!fstab) {
        LOGE("failed to read /etc/recovery.fstab\n");
        return;
    }

    ret = fs_mgr_add_entry(fstab, "/tmp", "ramdisk", "ramdisk", 0);
    if (ret < 0 ) {
        LOGE("failed to add /tmp entry to fstab\n");
        fs_mgr_free_fstab(fstab);
        fstab = NULL;
        return;
    }

    // Create a boring /etc/fstab so tools like Busybox work
    FILE *file = fopen("/etc/fstab", "w");
    if (file == NULL) {
        LOGW("Unable to create /etc/fstab!\n");
        return;
    }

    is_datamedia = 1;

    printf("recovery filesystem table\n");
    printf("=========================\n");
    for (i = 0; i < fstab->num_entries; ++i) {
        fstab_rec* v = &fstab->recs[i];
        printf("  %d %s %s %s %lld\n", i, v->mount_point, v->fs_type,
               v->blk_device, v->length);

        write_fstab_entry(v, file);

        if (strcmp(v->mount_point, "/external_sd") == 0 ||
                strncmp(v->mount_point, "/sdcard", 7) == 0 ||
                strncmp(v->mount_point, "/mnt/media_rw/sdcard", 20) == 0 ||
                (fs_mgr_is_voldmanaged(v) && strncmp(v->label, "sdcard", 6) == 0)) {
            is_datamedia = 0;
        }
    }

    fclose(file);

    printf("\n");
}

storage_item* get_storage_items() {
    storage_item* items = (storage_item*)calloc(MAX_NUM_MANAGED_VOLUMES+1, sizeof(storage_item));
    int i;
    storage_item* item = items;

    if (is_data_media()) {
        item->vol = volume_for_path("/data");
        item->label = strdup("internal storage");
        item->path = strdup("/data/media");
        ++item;
    }
    for (i = 0; i < get_num_volumes(); i++) {
        fstab_rec* v = get_device_volumes() + i;
        if (strcmp(v->mount_point, "/external_sd") == 0 ||
                strncmp(v->mount_point, "/sdcard", 7) == 0) {
            item->vol = v;
            item->label = strdup(&v->mount_point[1]);
            item->path = strdup(v->mount_point);
            ++item;
        }
        else if (strncmp(v->mount_point, "/mnt/media_rw/sdcard", 20) == 0) {
            item->vol = v;
            item->label = strdup(&v->mount_point[14]);
            item->path = strdup(v->mount_point);
            ++item;
        }
        else if (fs_mgr_is_voldmanaged(v) && strncmp(v->label, "sdcard", 6) == 0) {
            char* path = (char*)malloc(9+strlen(v->label)+1);
            sprintf(path, "/storage/%s", v->label);
            if (vold_is_volume_available(path)) {
                item->vol = v;
                item->label = strdup(v->label);
                item->path = strdup(path);
                ++item;
            }
            free(path);
        }
    }

    return items;
}

void free_storage_items(storage_item* items) {
    storage_item* item = items;
    while (item->vol) {
        free(item->label);
        free(item->path);
        ++item;
    }
    free(items);
}

fstab_rec* volume_for_path(const char* path) {
    return fs_mgr_get_entry_for_mount_point(fstab, path);
}

fstab_rec* volume_for_label(const char* label) {
    int i;
    for (i = 0; i < get_num_volumes(); i++) {
        fstab_rec* v = get_device_volumes() + i;
        if (v->label && !strcmp(v->label, label)) {
            return v;
        }
    }
    return NULL;
}

int ensure_path_mounted(const char* path) {
    fstab_rec* v;
    if (memcmp(path, "/storage/", 9) == 0) {
        char label[PATH_MAX];
        const char* p = path+9;
        const char* q = strchr(p, '/');
        memset(label, 0, sizeof(label));
        if (q) {
            memcpy(label, p, q-p);
        }
        else {
            strcpy(label, p);
        }
        v = volume_for_label(label);
    }
    else {
        v = volume_for_path(path);
    }
    if (v == NULL) {
        LOGE("unknown volume for path [%s]\n", path);
        return -1;
    }
    return ensure_volume_mounted(v);
}

int ensure_volume_mounted(fstab_rec* v) {
    if (v == NULL) {
        LOGE("cannot mount unknown volume\n");
        return -1;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted.
        return 0;
    }

    int result;
    result = scan_mounted_volumes();
    if (result < 0) {
        LOGE("failed to scan mounted volumes\n");
        return -1;
    }

    if (!fs_mgr_is_voldmanaged(v)) {
        const MountedVolume* mv =
            find_mounted_volume_by_mount_point(v->mount_point);
        if (mv) {
            // volume is already mounted
            return 0;
        }
    }

    mkdir_p(v->mount_point, 0755);  // in case it doesn't already exist

    if (fs_mgr_is_voldmanaged(v)) {
        if (!strcmp(v->mount_point, "auto")) {
            return vold_mount_auto_volume(v->label, 1);
        }
        return vold_mount_volume(v->mount_point, 1);

    } else if (strcmp(v->fs_type, "yaffs2") == 0) {
        // mount an MTD partition as a YAFFS2 filesystem.
        mtd_scan_partitions();
        const MtdPartition* partition;
        partition = mtd_find_partition_by_name(v->blk_device);
        if (partition == NULL) {
            LOGE("failed to find \"%s\" partition to mount at \"%s\"\n",
                 v->blk_device, v->mount_point);
            return -1;
        }
        return mtd_mount_partition(partition, v->mount_point, v->fs_type, 0);
    } else if (strcmp(v->fs_type, "ext4") == 0 ||
#ifdef USE_F2FS
               strcmp(v->fs_type, "f2fs") == 0 ||
#endif
               strcmp(v->fs_type, "vfat") == 0) {
        result = mount(v->blk_device, v->mount_point, v->fs_type,
                       MS_NOATIME | MS_NODEV | MS_NODIRATIME, "");
        if (result == 0) return 0;

        LOGE("failed to mount %s (%s)\n", v->mount_point, strerror(errno));
        return -1;
    }

    LOGE("unknown fs_type \"%s\" for %s\n", v->fs_type, v->mount_point);
    return -1;
}

int ensure_path_unmounted(const char* path) {
    fstab_rec* v;
    if (memcmp(path, "/storage/", 9) == 0) {
        char label[PATH_MAX];
        const char* p = path+9;
        const char* q = strchr(p, '/');
        memset(label, 0, sizeof(label));
        if (q) {
            memcpy(label, p, q-p);
        }
        else {
            strcpy(label, p);
        }
        v = volume_for_label(label);
    }
    else {
        v = volume_for_path(path);
    }
    if (v == NULL) {
        LOGE("unknown volume for path [%s]\n", path);
        return -1;
    }
    return ensure_volume_unmounted(v);
}

int ensure_volume_unmounted(fstab_rec* v) {
    if (v == NULL) {
        LOGE("cannot unmount unknown volume\n");
        return -1;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted; you can't unmount it.
        return -1;
    }

    int result;
    result = scan_mounted_volumes();
    if (result < 0) {
        LOGE("failed to scan mounted volumes\n");
        return -1;
    }

    if (fs_mgr_is_voldmanaged(v)) {
        if (!strcmp(v->mount_point, "auto")) {
            return vold_unmount_auto_volume(v->label, 0, 1);
        }
        return vold_unmount_volume(v->mount_point, 0, 1);
    }

    const MountedVolume* mv =
        find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL) {
        // volume is already unmounted
        return 0;
    }

    return unmount_mounted_volume(mv);
}

static int rmtree_except(const char* path, const char* except)
{
    char pathbuf[PATH_MAX];
    int rc = 0;
    DIR* dp = opendir(path);
    if (dp == NULL) {
        return -1;
    }
    struct dirent* de;
    while ((de = readdir(dp)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (except && !strcmp(de->d_name, except))
            continue;
        struct stat st;
        snprintf(pathbuf, sizeof(pathbuf), "%s/%s", path, de->d_name);
        rc = lstat(pathbuf, &st);
        if (rc != 0) {
            LOGE("Failed to stat %s\n", pathbuf);
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            rc = rmtree_except(pathbuf, NULL);
            if (rc != 0)
                break;
            rc = rmdir(pathbuf);
        }
        else {
            rc = unlink(pathbuf);
        }
        if (rc != 0) {
            LOGI("Failed to remove %s: %s\n", pathbuf, strerror(errno));
            break;
        }
    }
    closedir(dp);
    return rc;
}

int format_volume(const char* volume, bool force) {
    if (strcmp(volume, "media") == 0) {
        if (!is_data_media()) {
            return 0;
        }
        if (ensure_path_mounted("/data") != 0) {
            LOGE("format_volume failed to mount /data\n");
            return -1;
        }
        int rc = 0;
        rc = rmtree_except("/data/media", NULL);
        ensure_path_unmounted("/data");
        return rc;
    }

    fstab_rec* v = volume_for_path(volume);
    if (v == NULL) {
        LOGE("unknown volume \"%s\"\n", volume);
        return -1;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // you can't format the ramdisk.
        LOGE("can't format_volume \"%s\"", volume);
        return -1;
    }
    if (strcmp(v->mount_point, volume) != 0) {
        LOGE("can't give path \"%s\" to format_volume\n", volume);
        return -1;
    }

    if (!force && strcmp(volume, "/data") == 0) {
        if (ensure_path_mounted("/data") == 0) {
            int rc = rmtree_except("/data", "media");
            ensure_path_unmounted(volume);
            return rc;
        }
        LOGE("format_volume failed to mount /data, formatting instead\n");
    }

    if (ensure_path_unmounted(volume) != 0) {
        LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
        return -1;
    }

    // Only use vold format for exact matches otherwise /sdcard will be
    // formatted instead of /storage/sdcard0/.android_secure
    if (fs_mgr_is_voldmanaged(v) && strcmp(volume, v->mount_point) == 0) {
        if (ensure_path_unmounted(volume) != 0) {
            LOGE("format_volume failed to unmount %s", v->mount_point);
        }
        return vold_format_volume(v->mount_point, 1);
    }

    if (strcmp(v->fs_type, "yaffs2") == 0 || strcmp(v->fs_type, "mtd") == 0) {
        mtd_scan_partitions();
        const MtdPartition* partition = mtd_find_partition_by_name(v->blk_device);
        if (partition == NULL) {
            LOGE("format_volume: no MTD partition \"%s\"\n", v->blk_device);
            return -1;
        }

        MtdWriteContext *write = mtd_write_partition(partition);
        if (write == NULL) {
            LOGW("format_volume: can't open MTD \"%s\"\n", v->blk_device);
            return -1;
        } else if (mtd_erase_blocks(write, -1) == (off_t) -1) {
            LOGW("format_volume: can't erase MTD \"%s\"\n", v->blk_device);
            mtd_write_close(write);
            return -1;
        } else if (mtd_write_close(write)) {
            LOGW("format_volume: can't close MTD \"%s\"\n", v->blk_device);
            return -1;
        }
        return 0;
    }

    if (strcmp(v->fs_type, "ext4") == 0) {
        int result = make_ext4fs(v->blk_device, v->length, volume, sehandle);
        if (result != 0) {
            LOGE("format_volume: make_ext4fs failed on %s\n", v->blk_device);
            return -1;
        }
        return 0;
    }

#ifdef USE_F2FS
    if (strcmp(v->fs_type, "f2fs") == 0) {
        const char* args[] = { "mkfs.f2fs", v->blk_device };
        int result = make_f2fs_main(2, (char**)args);
        if (result != 0) {
            LOGE("format_volume: mkfs.f2fs failed on %s\n", v->blk_device);
            return -1;
        }
        return 0;
    }
#endif

    LOGE("format_volume: fs_type \"%s\" unsupported\n", v->fs_type);
    return -1;
}

int setup_install_mounts() {
    if (fstab == NULL) {
        LOGE("can't set up install mounts: no fstab loaded\n");
        return -1;
    }
    for (int i = 0; i < fstab->num_entries; ++i) {
        fstab_rec* v = fstab->recs + i;

        if (strcmp(v->mount_point, "/tmp") == 0 ||
            strcmp(v->mount_point, "/cache") == 0) {
            if (ensure_path_mounted(v->mount_point) != 0) return -1;

        } else {
            if (ensure_path_unmounted(v->mount_point) != 0) return -1;
        }
    }
    return 0;
}
