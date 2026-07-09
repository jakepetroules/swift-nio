//===----------------------------------------------------------------------===//
//
// This source file is part of the SwiftNIO open source project
//
// Copyright (c) 2020 Apple Inc. and the SwiftNIO project authors
// Licensed under Apache License v2.0
//
// See LICENSE.txt for license information
// See CONTRIBUTORS.txt for the list of SwiftNIO project authors
//
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//

#ifndef C_NIO_WINDOWS_H
#define C_NIO_WINDOWS_H

#if defined(_WIN32)

#include <WinSock2.h>
#include <time.h>
#include <stdint.h>
#include <basetsd.h>

#define NIO(name) CNIOWindows_ ## name

// This is a DDK type which is not available in the WinSDK as it is not part of
// the shared, usermode (um), or ucrt portions of the code.  We must replicate
// this datastructure manually from the MSDN references or the DDK.
// https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_reparse_data_buffer
typedef struct NIO(_REPARSE_DATA_BUFFER) {
  ULONG   ReparseTag;
  USHORT  ReparseDataLength;
  USHORT  Reserved;
  union {
    struct {
      USHORT  SubstituteNameOffset;
      USHORT  SubstituteNameLength;
      USHORT  PrintNameOffset;
      USHORT  PrintNameLength;
      ULONG   Flags;
      WCHAR   PathBuffer[1];
    } SymbolicLinkReparseBuffer;
    struct {
      USHORT  SubstituteNameOffset;
      USHORT  SubstituteNameLength;
      USHORT  PrintNameOffset;
      USHORT  PrintNameLength;
      WCHAR   PathBuffer[1];
    } MountPointReparseBuffer;
    struct {
      UCHAR   DataBuffer[1];
    } GenericReparsaeBuffer;
  } DUMMYUNIONNAME;
} NIO(REPARSE_DATA_BUFFER), *NIO(PREPARSE_DATA_BUFFER);

typedef struct {
  WSAMSG msg_hdr;
  unsigned int msg_len;
} NIO(mmsghdr);

static inline __attribute__((__always_inline__)) int
NIO(getsockopt)(SOCKET s, int level, int optname, void *optval, int *optlen) {
  return getsockopt(s, level, optname, optval, optlen);
}

static inline __attribute__((__always_inline__)) int
NIO(recv)(SOCKET s, void *buf, int len, int flags) {
  return recv(s, buf, len, flags);
}

static inline __attribute__((__always_inline__)) int
NIO(recvfrom)(SOCKET s, void *buf, int len, int flags, SOCKADDR *from,
              int *fromlen) {
  return recvfrom(s, buf, len, flags, from, fromlen);
}

static inline __attribute__((__always_inline__)) int
NIO(send)(SOCKET s, const void *buf, int len, int flags) {
  return send(s, buf, len, flags);
}

static inline __attribute__((__always_inline__)) int
NIO(setsockopt)(SOCKET s, int level, int optname, const void *optval,
                int optlen) {
  return setsockopt(s, level, optname, optval, optlen);
}

static inline __attribute__((__always_inline__)) int
NIO(sendto)(SOCKET s, const void *buf, int len, int flags, const SOCKADDR *to,
            int tolen) {
  return sendto(s, buf, len, flags, to, tolen);
}

int NIO(sendmmsg)(SOCKET s, NIO(mmsghdr) *msgvec, unsigned int vlen, int flags);

int NIO(recvmmsg)(SOCKET s, NIO(mmsghdr) *msgvec, unsigned int vlen, int flags,
                  struct timespec *timeout);


const void *NIO(CMSG_DATA)(const WSACMSGHDR *);
void *NIO(CMSG_DATA_MUTABLE)(LPWSACMSGHDR);

WSACMSGHDR *NIO(CMSG_FIRSTHDR)(const WSAMSG *);
WSACMSGHDR *NIO(CMSG_NXTHDR)(const WSAMSG *, LPWSACMSGHDR);

size_t NIO(CMSG_LEN)(size_t);
size_t NIO(CMSG_SPACE)(size_t);

int NIO(errno)(void);

// Sets `errno`. `errno` is a macro on the Windows CRT (expanding to a
// dereference of `_errno()`) that Swift cannot assign through directly, so we
// wrap the write in C where the macro is expanded.
void NIO(set_errno)(int value);

DWORD NIO(FormatGetLastError)(DWORD errorCode, LPSTR errorMsg);

// Disables buffering on the process's standard output stream. `stdout` is a
// macro on the Windows CRT (currently `__acrt_iob_func(1)`) that Swift cannot
// reference directly; wrapping it here lets the C compiler expand the macro, so
// we remain correct if its definition ever changes.
void NIO(setStdoutUnbuffered)(void);

// Copies the regular file at `source` to `destination` using `CopyFile2`,
// which transparently uses ReFS block cloning (copy-on-write) where the volume
// supports it and falls back to a full copy otherwise. `source` and
// `destination` are wide (UTF-16) paths, matching swift-system's
// `CInterop.PlatformChar` on Windows.
//
// If `failIfExists` is non-zero the copy fails (setting `errno` to `EEXIST`)
// when `destination` already exists; otherwise an existing destination is
// overwritten in place. Returns 0 on success, or -1 with `errno` set.
int NIO(copyfile)(const wchar_t *source, const wchar_t *destination, int failIfExists);

// As `copyfile`, but copies the symbolic link at `source` itself (via
// `COPY_FILE_COPY_SYMLINK`) rather than following it, recreating a link to the
// same target at `destination`. Creating the link requires
// `SeCreateSymbolicLinkPrivilege`; without it the copy fails and `errno` is set
// to `EACCES`.
int NIO(copysymlink)(const wchar_t *source, const wchar_t *destination, int failIfExists);

// Renames (moves) `source` to `destination` using `MoveFileExW`. If
// `replaceExisting` is non-zero any existing destination is atomically
// replaced; otherwise an existing destination fails with `errno` set to
// `EEXIST`. `MOVEFILE_COPY_ALLOWED` permits a copy+delete fallback across
// volumes. Returns 0 on success, or -1 with `errno` set.
int NIO(rename)(const wchar_t *source, const wchar_t *destination, int replaceExisting);

// Extended-attribute helpers backing the POSIX-shaped f*xattr wrappers, built
// on NTFS Extended Attributes via the ntdll native calls NtQueryEaFile /
// NtSetEaFile (resolved dynamically). `fd` is a CRT file descriptor. Attribute
// names are passed as wide (UTF-16) strings and transcoded to the narrow,
// case-insensitive names NTFS EAs use.
//
// Semantics mirror the Linux f*xattr family:
//  - `fgetxattr`: with `value == NULL` (or `size == 0`) returns the size needed
//    to hold the value; otherwise copies the value and returns its length, or
//    fails with `ERANGE` if it doesn't fit. Fails with `ENODATA` if the named
//    attribute doesn't exist. Returns the value length, or -1 with `errno` set.
//  - `fsetxattr`: creates or replaces the named attribute. Returns 0, or -1.
//  - `fremovexattr`: deletes the named attribute (fails with `ENODATA` if
//    absent). Returns 0, or -1.
//  - `flistxattr`: with `namebuf == NULL` (or `size == 0`) returns the size
//    needed; otherwise writes the NUL-terminated attribute names with no
//    padding and returns their total length, or fails with `ERANGE` if they
//    don't fit. Returns the list length, or -1 with `errno` set.
intptr_t NIO(fgetxattr)(int fd, const wchar_t *name, void *value, intptr_t size);
int NIO(fsetxattr)(int fd, const wchar_t *name, const void *value, intptr_t size);
int NIO(fremovexattr)(int fd, const wchar_t *name);
intptr_t NIO(flistxattr)(int fd, char *namebuf, intptr_t size);

// MARK: - Scalar filesystem / CRT helpers
//
// Thin wrappers over CRT / Win32 primitives backing the POSIX-shaped syscall
// stubs used by the NIOFileSystem family. Each returns 0 / a valid pointer on
// success, or -1 / NULL with `errno` set, matching its POSIX counterpart.

// `fsync(2)` -> `FlushFileBuffers`.
int NIO(fsync)(int fd);
// `getenv(3)`; returns a pointer owned by the CRT environment (or NULL).
char *NIO(getenv)(const char *name);
// `getcwd(3)` over wide paths -> `_wgetcwd`.
wchar_t *NIO(getcwd)(wchar_t *buffer, int size);
// `strerror(3)`.
char *NIO(strerror)(int code);
// `mkdir(2)` -> `CreateDirectoryW` (mode is not representable, ignored).
int NIO(mkdir)(const wchar_t *path);
// `unlink(2)` -> `DeleteFileW`.
int NIO(unlink)(const wchar_t *path);
// `remove(3)` -> `DeleteFileW`, falling back to `RemoveDirectoryW` for a directory.
int NIO(remove)(const wchar_t *path);
// `link(2)` -> `CreateHardLinkW`.
int NIO(link)(const wchar_t *existing, const wchar_t *newLink);

// Thread-local storage via Fiber Local Storage, backing the pthread_key_* API.
// FLS (rather than TLS) is used because its destructor callback runs on thread
// exit, matching pthread key destructor semantics. Keys are `uint32_t` to match
// the Swift `pthread_key_t` typealias.
int NIO(fls_key_create)(uint32_t *key, void (*destructor)(void *));
int NIO(fls_set)(uint32_t key, const void *value);
void *NIO(fls_get)(uint32_t key);

// MARK: - stat family

// POSIX-shaped file status. A flat C struct the Swift layer translates into its
// own `CInterop.Stat`; timestamps are whole seconds + nanoseconds since the
// Unix epoch. Mode uses the standard `S_IF*` bits.
typedef struct {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint64_t st_size;
  int64_t st_atim_sec;
  int64_t st_atim_nsec;
  int64_t st_mtim_sec;
  int64_t st_mtim_nsec;
  int64_t st_ctim_sec;
  int64_t st_ctim_nsec;
} CNIOWindows_stat_t;

// `fstat(2)` from a CRT file descriptor.
int NIO(fstat)(int fd, CNIOWindows_stat_t *out);
// `stat(2)` / `lstat(2)`; `followSymlinks` selects whether reparse points are
// followed (stat) or reported as links (lstat).
int NIO(stat)(const wchar_t *path, int followSymlinks, CNIOWindows_stat_t *out);
// `fchmod(2)`: only the writable bit is representable; sets/clears
// `FILE_ATTRIBUTE_READONLY` accordingly.
int NIO(fchmod)(int fd, uint32_t mode);
// `futimens(2)`: set access/modification times from nanoseconds since the Unix
// epoch. A `*_sec` of -1 leaves that timestamp unchanged (UTIME_OMIT) and -2
// sets it to the current time (UTIME_NOW); the Swift layer maps the POSIX
// sentinels onto this convention.
int NIO(futimens)(int fd, int64_t atimeSec, int64_t atimeNsec, int64_t mtimeSec, int64_t mtimeNsec);

// MARK: - openat / *at family
//
// NIOFS always passes a real, open directory descriptor to these calls on
// Windows (AT_FDCWD is not exposed there), and file-system paths are made
// absolute before use. Each shim recovers the directory's path from `dirfd`
// via GetFinalPathNameByHandleW and joins the relative child name onto it.
//
// Behaviour gap: resolving `dirfd` to a path and then operating on that path is
// not atomic with respect to the directory being renamed concurrently (unlike
// POSIX *at calls, which pin the directory). Callers needing atomicity must
// serialise externally.

// `openat(2)`. `oflag` carries the CRT `_O_*` access/creation bits OR'd with the
// swift-system-gap OpenOptions bits the Swift layer owns; `windowsFlags`
// separately carries those gap bits (noFollow / directory / closeOnExec) already
// decoded so the C side need not know their numeric values. On success returns a
// CRT file descriptor; on failure returns -1 with `errno` set.
int NIO(openat)(int dirfd, const wchar_t *name, int oflag, int windowsFlags, uint32_t mode);

// Decoded `windowsFlags` bits for `openat`, set by the Swift layer.
#define CNIO_O_NOFOLLOW   0x1
#define CNIO_O_DIRECTORY  0x2
#define CNIO_O_CLOEXEC    0x4

// `unlinkat(2)`: removes a directory when `removeDir` is non-zero (AT_REMOVEDIR),
// otherwise a file.
int NIO(unlinkat)(int dirfd, const wchar_t *name, int removeDir);

// `symlink(2)` / `symlinkat(2)`: create a symbolic link at `linkPath` pointing at
// `target`. Requires SeCreateSymbolicLinkPrivilege; without it fails with EACCES.
int NIO(symlink)(const wchar_t *target, const wchar_t *linkPath);
int NIO(symlinkat)(const wchar_t *target, int dirfd, const wchar_t *linkPath);

// `readlink(2)`: reads a reparse point's target into `buffer` (wide chars, not
// NUL-terminated), returning the number of wchars written, or -1 with `errno`.
intptr_t NIO(readlink)(const wchar_t *path, wchar_t *buffer, intptr_t size);

// MARK: - Directory iteration primitives
//
// Raw FindFirstFileExW / FindNextFileW iteration. The stateful directory-stream
// and FTS logic lives in Swift; these primitives just surface one entry at a
// time so the Swift side needn't call the wide Win32 APIs directly.

// Open a directory iterator from an open directory descriptor, or by path (the
// latter is used by the FTS walk to descend into subdirectories). Returns an
// opaque handle, or NULL with `errno` set.
void *NIO(dir_open_fd)(int fd);
void *NIO(dir_open_path)(const wchar_t *path);

// Fetch the next entry. Writes the entry's (wide, NUL-terminated) name into
// `nameOut` (capacity `nameCap` wchars) and its type — one of the CNIO_DT_*
// values below — into `*typeOut`. Returns 1 for an entry, 0 at end of
// directory, or -1 with `errno` set.
int NIO(dir_next)(void *dir, wchar_t *nameOut, int nameCap, uint8_t *typeOut);

// Close an iterator opened by dir_open_fd / dir_open_path.
void NIO(dir_close)(void *dir);

// Entry-type values reported by dir_next (mirroring the DT_* constants the Swift
// layer uses).
#define CNIO_DT_UNKNOWN 0
#define CNIO_DT_DIR     4
#define CNIO_DT_REG     8
#define CNIO_DT_LNK     10

#undef NIO

#endif

#endif
