// semihost_vfs.cpp - QEMU only: a VFS backed by Xtensa ISS "simcall"
// semihosting, so the firmware reads the prepared game tree straight from a
// host directory.
//
// ESP-IDF's own semihosting VFS speaks the OpenOCD protocol (BREAK 1,14),
// which Espressif's QEMU does not implement; QEMU's xtensa target does
// implement the ISS simcall ABI (enable with -semihosting):
//   a2 = call, a3..a5 = args; a2 = result, a3 = errno
//   3 read(fd, buf, n)  4 write(fd, buf, n)  5 open(path, flags, mode)
//   6 close(fd)  19 lseek(fd, off, whence)
// Open flags use the GDB File-I/O values, which equal newlib's.
// There is no stat/mkdir/readdir: stat is emulated with open + lseek, a
// directory is told apart by read() failing with EISDIR, and directories must
// already exist on the host (re1tool.py creates save/).
#include "board.h"

#if CONFIG_RE1_TARGET_QEMU

#include "esp_vfs.h"
#include "esp_log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

namespace {

char s_hostDir[128];

long Simcall(long nr, long a, long b, long c, long* err)
{
    register long a2 asm("a2") = nr;
    register long a3 asm("a3") = a;
    register long a4 asm("a4") = b;
    register long a5 asm("a5") = c;
    __asm__ __volatile__("simcall" : "+r"(a2), "+r"(a3) : "r"(a4), "r"(a5) : "memory");
    if (err) *err = a3;
    return a2;
}

void HostPath(const char* path, char* out, size_t size)
{
    snprintf(out, size, "%s%s", s_hostDir, path);
}

int Fail(long err, int fallback)
{
    errno = (err > 0 && err < 200) ? (int)err : fallback;
    return -1;
}

int VfsOpen(const char* path, int flags, int mode)
{
    char p[300];
    HostPath(path, p, sizeof(p));
    long err = 0;
    long fd = Simcall(5, (long)p, flags, mode ? mode : 0644, &err);
    return fd < 0 ? Fail(err, ENOENT) : (int)fd;
}

ssize_t VfsRead(int fd, void* dst, size_t size)
{
    long err = 0;
    long n = Simcall(3, fd, (long)dst, (long)size, &err);
    return n < 0 ? Fail(err, EIO) : (ssize_t)n;
}

ssize_t VfsWrite(int fd, const void* data, size_t size)
{
    long err = 0;
    long n = Simcall(4, fd, (long)data, (long)size, &err);
    return n < 0 ? Fail(err, EIO) : (ssize_t)n;
}

off_t VfsLseek(int fd, off_t off, int whence)
{
    long err = 0;
    long r = Simcall(19, fd, (long)off, whence, &err);
    return r < 0 ? (off_t)Fail(err, EINVAL) : (off_t)r;
}

int VfsClose(int fd)
{
    long err = 0;
    long r = Simcall(6, fd, 0, 0, &err);
    return r < 0 ? Fail(err, EBADF) : 0;
}

bool IsDirFd(long fd)
{
    char c;
    long err = 0;
    long n = Simcall(3, fd, (long)&c, 1, &err);
    return n < 0 && err == EISDIR;
}

int StatFd(long fd, struct stat* st)
{
    memset(st, 0, sizeof(*st));
    long err = 0;
    // Save the position first: the directory probe reads a byte, and newlib's
    // fopen fstat()s the descriptor before the first read.
    long cur = Simcall(19, fd, 0, SEEK_CUR, &err);
    if (IsDirFd(fd)) {
        st->st_mode = S_IFDIR | 0755;
        return 0;
    }
    long end = Simcall(19, fd, 0, SEEK_END, &err);
    Simcall(19, fd, cur < 0 ? 0 : cur, SEEK_SET, &err);
    st->st_mode = S_IFREG | 0644;
    st->st_size = end < 0 ? 0 : end;
    st->st_blksize = 4096;
    return 0;
}

int VfsFstat(int fd, struct stat* st)
{
    return StatFd(fd, st);
}

int VfsStat(const char* path, struct stat* st)
{
    char p[300];
    HostPath(path, p, sizeof(p));
    long err = 0;
    long fd = Simcall(5, (long)p, O_RDONLY, 0, &err);
    if (fd < 0) return Fail(err, ENOENT);
    StatFd(fd, st);
    Simcall(6, fd, 0, 0, &err);
    return 0;
}

int VfsMkdir(const char* path, mode_t)
{
    struct stat st;
    if (VfsStat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        errno = EEXIST;
        return -1;
    }
    errno = ENOSYS;   // create directories on the host side
    return -1;
}

}  // namespace

int board_semihost_vfs_register(const char* mount, const char* hostDir)
{
    strncpy(s_hostDir, hostDir, sizeof(s_hostDir) - 1);
    size_t n = strlen(s_hostDir);
    while (n > 0 && s_hostDir[n - 1] == '/') s_hostDir[--n] = '\0';

    esp_vfs_t vfs = {};
    vfs.flags = ESP_VFS_FLAG_DEFAULT;
    vfs.open = VfsOpen;
    vfs.read = VfsRead;
    vfs.write = VfsWrite;
    vfs.lseek = VfsLseek;
    vfs.close = VfsClose;
    vfs.fstat = VfsFstat;
    vfs.stat = VfsStat;
    vfs.mkdir = VfsMkdir;
    if (esp_vfs_register(mount, &vfs, nullptr) != ESP_OK) return 0;
    ESP_LOGI("semihost", "%s -> host directory '%s'", mount, s_hostDir);
    return 1;
}

#else

int board_semihost_vfs_register(const char*, const char*) { return 0; }

#endif
