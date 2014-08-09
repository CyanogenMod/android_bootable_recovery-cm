#include <utils/String8.h>

#include <lib/libtar.h>
#include <zlib.h>

extern "C" {
#include <sha1.h>
#include <md5.h>
// Add some compatibility stuff for bionic md5
#ifndef MD5_DIGEST_LENGTH
#define MD5_DIGEST_LENGTH 16
#endif
#ifndef MD5_DIGEST_STRING_LENGTH
#define MD5_DIGEST_STRING_LENGTH 33
#endif
typedef struct md5 MD5_CTX;
}

#define HASH_MAX_LENGTH SHA1_DIGEST_LENGTH
#define HASH_MAX_STRING_LENGTH SHA1_DIGEST_STRING_LENGTH

#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef max
#define max(a,b) ((a)<(b)?(b):(a))
#endif

#define PROP_LINE_LEN (PROPERTY_KEY_MAX+1+PROPERTY_VALUE_MAX+1+1)

extern int sockfd;
extern TAR* tar;
extern gzFile gzf;

extern char* hash_name;
extern size_t hash_datalen;
extern SHA1_CTX sha1_ctx;
extern MD5_CTX md5_ctx;

struct partspec {
    char*       name;
    char*       path;
    fstab_rec*  vol;
    uint64_t    size;
    uint64_t    used;
    uint64_t    off;
};
#define MAX_PART 8

extern void logmsg(const char* fmt, ...);

extern int part_add(const char* name);
extern partspec* part_get(int i);
extern partspec* part_find(const char* name);
extern void part_set(partspec* part);

extern int update_progress(uint64_t off);

extern int create_tar(const char* compress, const char* mode);

extern int do_backup(int argc, char** argv);
extern int do_restore(int argc, char** argv);
