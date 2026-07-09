//===----------------------------------------------------------------------===//
//
// This source file is part of the SwiftNIO open source project
//
// Copyright (c) 2026 Apple Inc. and the SwiftNIO project authors
// Licensed under Apache License v2.0
//
// See LICENSE.txt for license information
// See CONTRIBUTORS.txt for the list of SwiftNIO project authors
//
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//

#if os(Windows)

// Windows-only compatibility shim for the NIOFileSystem family.
//
// This port is *compile-only*: it exists so the file-system targets build on
// Windows. Every POSIX primitive that has no trivial Win32 equivalent is
// stubbed with `fatalError(...)` (or a trivial value). Real Win32
// implementations are a follow-up, tracked by the NIOFS Windows-port design
// doc.
//
// Everything here is defined at module scope so, thanks to same-module
// visibility, the rest of the target can use these types, constants and
// free-functions without importing anything.

import CNIOWindows
import SystemPackage

// MARK: - Scalar types

/// POSIX `off_t`.
typealias off_t = Int64

/// POSIX `timespec`. Defined locally (rather than imported from ucrt) to avoid
/// colliding with the CRT declaration in files that reference it.
public struct timespec: Sendable {
    public var tv_sec: Int
    public var tv_nsec: Int

    public init(tv_sec: Int = 0, tv_nsec: Int = 0) {
        self.tv_sec = tv_sec
        self.tv_nsec = tv_nsec
    }
}

/// POSIX `pthread_key_t`.
typealias pthread_key_t = UInt32

// MARK: - `CInterop.Stat` and friends

extension CInterop {
    /// Windows stand-in for `struct stat`. Only carries the fields NIOFS reads.
    public struct WindowsStat: Sendable {
        var st_dev: UInt64 = 0
        var st_ino: UInt64 = 0
        var st_mode: UInt32 = 0
        var st_nlink: UInt32 = 0
        var st_uid: UInt32 = 0
        var st_gid: UInt32 = 0
        var st_rdev: UInt64 = 0
        var st_size: Int64 = 0
        var st_blocks: Int64 = 0
        var st_blksize: Int64 = 0
        var st_atim: timespec = timespec()
        var st_mtim: timespec = timespec()
        var st_ctim: timespec = timespec()

        init() {}

        /// Translates the flat C `CNIOWindows_stat_t` bridge struct produced by
        /// the stat shims into the fields NIOFS reads.
        init(_ raw: CNIOWindows_stat_t) {
            self.st_dev = raw.st_dev
            self.st_ino = raw.st_ino
            self.st_mode = raw.st_mode
            self.st_nlink = raw.st_nlink
            self.st_size = Int64(bitPattern: raw.st_size)
            self.st_atim = timespec(tv_sec: Int(raw.st_atim_sec), tv_nsec: Int(raw.st_atim_nsec))
            self.st_mtim = timespec(tv_sec: Int(raw.st_mtim_sec), tv_nsec: Int(raw.st_mtim_nsec))
            self.st_ctim = timespec(tv_sec: Int(raw.st_ctim_sec), tv_nsec: Int(raw.st_ctim_nsec))
        }
    }

    public typealias Stat = WindowsStat

    @_spi(Testing)
    public static let maxPathLength: Int32 = 260

    /// Directory-stream handle. Matches the Linux representation (opaque).
    typealias DirPointer = OpaquePointer

    /// Windows stand-in for `struct dirent`. `d_name` is a fixed-size tuple of
    /// `CChar` so the existing `.0/.1/.2` "is this '.' or '..'" check compiles.
    struct WindowsDirEnt {
        var d_type: UInt8 = 0
        var d_name:
            (
                CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar,
                CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar
            ) = (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)

        init() {}
    }

    typealias DirEnt = WindowsDirEnt

    /// Opaque FTS handle.
    typealias FTS = OpaquePointer

    /// Windows stand-in for `FTSENT`. Only the fields NIOFS reads.
    struct WindowsFTSEnt {
        var fts_info: UInt16 = 0
        var fts_errno: CInt = 0
        var fts_path: UnsafeMutablePointer<CInterop.PlatformChar>? = nil

        init() {}
    }

    typealias FTSEnt = WindowsFTSEnt
}

// MARK: - swift-system gap extensions

extension FileDescriptor.OpenOptions {
    // Distinct high bits we own. These are placeholders for a real Win32
    // mapping in the future implementation.
    static var noFollow: FileDescriptor.OpenOptions {
        FileDescriptor.OpenOptions(rawValue: 0x4000_0000)
    }
    static var closeOnExec: FileDescriptor.OpenOptions {
        FileDescriptor.OpenOptions(rawValue: 0x2000_0000)
    }
    static var nonBlocking: FileDescriptor.OpenOptions {
        FileDescriptor.OpenOptions(rawValue: 0x1000_0000)
    }
    static var directory: FileDescriptor.OpenOptions {
        FileDescriptor.OpenOptions(rawValue: 0x0800_0000)
    }
}

extension Errno {
    /// No direct equivalent on Windows; alias to `EINVAL` so switches stay
    /// exhaustive.
    static var noData: Errno { Errno.invalidArgument }
}

// MARK: - `stat` mode constants (typed to `CInterop.Mode`)

let S_IFMT: CInterop.Mode = 0o170000
let S_IFSOCK: CInterop.Mode = 0o140000
let S_IFLNK: CInterop.Mode = 0o120000
let S_IFREG: CInterop.Mode = 0o100000
let S_IFBLK: CInterop.Mode = 0o060000
let S_IFDIR: CInterop.Mode = 0o040000
let S_IFCHR: CInterop.Mode = 0o020000
let S_IFIFO: CInterop.Mode = 0o010000

// MARK: - `dirent` `d_type` constants (typed to `CInt`)

let DT_UNKNOWN: CInt = 0
let DT_FIFO: CInt = 1
let DT_CHR: CInt = 2
let DT_DIR: CInt = 4
let DT_BLK: CInt = 6
let DT_REG: CInt = 8
let DT_LNK: CInt = 10
let DT_SOCK: CInt = 12

// MARK: - FTS constants (typed to `CInt`)

let FTS_D: CInt = 1
let FTS_DC: CInt = 2
let FTS_DEFAULT: CInt = 3
let FTS_DNR: CInt = 4
let FTS_DOT: CInt = 5
let FTS_DP: CInt = 6
let FTS_ERR: CInt = 7
let FTS_F: CInt = 8
let FTS_NS: CInt = 10
let FTS_NSOK: CInt = 11
let FTS_SL: CInt = 12
let FTS_SLNONE: CInt = 13

let FTS_PHYSICAL: CInt = 0x0010
let FTS_LOGICAL: CInt = 0x0002
let FTS_NOCHDIR: CInt = 0x0004

// MARK: - `UTIME_*` sentinels

let UTIME_NOW: CInt = (1 << 30) - 1
let UTIME_OMIT: CInt = (1 << 30) - 2

// MARK: - errno access

// `errno` is a macro on the Windows CRT that Swift cannot read or assign
// directly, so bridge through the `CNIOWindows` C shim, which expands the macro
// in C. This is the real thread-local `errno` backing the CRT calls the higher
// level file-system code inspects.
var _nio_fs_errno: CInt {
    get { CNIOWindows_errno() }
    set { CNIOWindows_set_errno(newValue) }
}

// MARK: - libc string helpers (stubs / trivial)

func strlen(_ s: UnsafePointer<CChar>) -> Int {
    var length = 0
    while s[length] != 0 { length += 1 }
    return length
}

func strlen(_ s: UnsafeMutablePointer<CChar>) -> Int {
    strlen(UnsafePointer(s))
}

func strlen(_ s: UnsafePointer<CInterop.PlatformChar>) -> Int {
    var length = 0
    while s[length] != 0 { length += 1 }
    return length
}

func strerror(_ code: CInt) -> UnsafeMutablePointer<CChar>? {
    CNIOWindows_strerror(code)
}

func memset(_ b: UnsafeMutableRawPointer, _ c: CInt, _ len: Int) -> UnsafeMutableRawPointer {
    b.initializeMemory(as: UInt8.self, repeating: UInt8(truncatingIfNeeded: c), count: len)
    return b
}

func getenv(_ name: UnsafePointer<CChar>) -> UnsafeMutablePointer<CChar>? {
    CNIOWindows_getenv(name)
}

// MARK: - POSIX file-system syscall stubs
//
// These match the bare-libc call signatures used by the `system_*` / `libc_*`
// wrappers, so those wrappers need no edits. All are stubs.

func openat(
    _ fd: FileDescriptor.RawValue,
    _ path: UnsafePointer<CInterop.PlatformChar>,
    _ oflag: CInt
) -> CInt {
    fatalError("openat is unavailable on Windows")
}

func openat(
    _ fd: FileDescriptor.RawValue,
    _ path: UnsafePointer<CInterop.PlatformChar>,
    _ oflag: CInt,
    _ mode: CInterop.Mode
) -> CInt {
    fatalError("openat is unavailable on Windows")
}

func stat(
    _ path: UnsafePointer<CInterop.PlatformChar>,
    _ info: UnsafeMutablePointer<CInterop.Stat>
) -> CInt {
    _nio_fs_stat(path, followSymlinks: true, info)
}

func lstat(
    _ path: UnsafePointer<CInterop.PlatformChar>,
    _ info: UnsafeMutablePointer<CInterop.Stat>
) -> CInt {
    _nio_fs_stat(path, followSymlinks: false, info)
}

func fstat(
    _ fd: FileDescriptor.RawValue,
    _ info: UnsafeMutablePointer<CInterop.Stat>
) -> CInt {
    var raw = CNIOWindows_stat_t()
    let result = CNIOWindows_fstat(fd, &raw)
    if result == 0 {
        info.pointee = CInterop.Stat(raw)
    }
    return result
}

private func _nio_fs_stat(
    _ path: UnsafePointer<CInterop.PlatformChar>,
    followSymlinks: Bool,
    _ info: UnsafeMutablePointer<CInterop.Stat>
) -> CInt {
    var raw = CNIOWindows_stat_t()
    let result = CNIOWindows_stat(path, followSymlinks ? 1 : 0, &raw)
    if result == 0 {
        info.pointee = CInterop.Stat(raw)
    }
    return result
}

func fchmod(_ fd: FileDescriptor.RawValue, _ mode: CInterop.Mode) -> CInt {
    CNIOWindows_fchmod(fd, UInt32(mode))
}

func fsync(_ fd: FileDescriptor.RawValue) -> CInt {
    CNIOWindows_fsync(fd)
}

func mkdir(_ path: UnsafePointer<CInterop.PlatformChar>, _ mode: CInterop.Mode) -> CInt {
    // Windows directories inherit their ACL; `mode` has no representable analogue.
    CNIOWindows_mkdir(path)
}

func symlink(
    _ destination: UnsafePointer<CInterop.PlatformChar>,
    _ source: UnsafePointer<CInterop.PlatformChar>
) -> CInt {
    fatalError("symlink is unavailable on Windows")
}

func symlinkat(
    _ destination: UnsafePointer<CInterop.PlatformChar>,
    _ dirfd: FileDescriptor.RawValue,
    _ source: UnsafePointer<CInterop.PlatformChar>
) -> CInt {
    fatalError("symlinkat is unavailable on Windows")
}

func readlink(
    _ path: UnsafePointer<CInterop.PlatformChar>,
    _ buffer: UnsafeMutablePointer<CInterop.PlatformChar>,
    _ size: Int
) -> Int {
    fatalError("readlink is unavailable on Windows")
}

func rename(
    _ old: UnsafePointer<CInterop.PlatformChar>,
    _ new: UnsafePointer<CInterop.PlatformChar>
) -> CInt {
    // Plain rename overwrites any existing destination, like POSIX `rename`.
    CNIOWindows_rename(old, new, 1)
}

func link(
    _ old: UnsafePointer<CInterop.PlatformChar>,
    _ new: UnsafePointer<CInterop.PlatformChar>
) -> CInt {
    CNIOWindows_link(old, new)
}

func unlink(_ path: UnsafePointer<CInterop.PlatformChar>) -> CInt {
    CNIOWindows_unlink(path)
}

func unlinkat(
    _ fd: FileDescriptor.RawValue,
    _ path: UnsafePointer<CInterop.PlatformChar>,
    _ flags: CInt
) -> CInt {
    fatalError("unlinkat is unavailable on Windows")
}

func futimens(
    _ fd: FileDescriptor.RawValue,
    _ times: UnsafePointer<timespec>?
) -> CInt {
    // POSIX `futimens` takes a 2-element array [access, modification]; a nil
    // pointer means "set both to now". Encode each timestamp for the C shim,
    // mapping the UTIME_OMIT/UTIME_NOW nanosecond sentinels onto the shim's
    // seconds sentinels (-1 = omit, -2 = now).
    func encode(_ ts: timespec) -> (sec: Int64, nsec: Int64) {
        switch CInt(truncatingIfNeeded: ts.tv_nsec) {
        case UTIME_OMIT: return (-1, 0)
        case UTIME_NOW: return (-2, 0)
        default: return (Int64(ts.tv_sec), Int64(ts.tv_nsec))
        }
    }
    let access: (sec: Int64, nsec: Int64)
    let modification: (sec: Int64, nsec: Int64)
    if let times = times {
        access = encode(times[0])
        modification = encode(times[1])
    } else {
        access = (-2, 0)
        modification = (-2, 0)
    }
    return CNIOWindows_futimens(
        fd,
        access.sec,
        access.nsec,
        modification.sec,
        modification.nsec
    )
}

func remove(_ path: UnsafePointer<CInterop.PlatformChar>) -> CInt {
    CNIOWindows_remove(path)
}

func getcwd(
    _ buffer: UnsafeMutablePointer<CInterop.PlatformChar>,
    _ size: Int
) -> UnsafeMutablePointer<CInterop.PlatformChar>? {
    CNIOWindows_getcwd(buffer, CInt(size))
}

func confstr(
    _ name: CInt,
    _ buffer: UnsafeMutablePointer<CInterop.PlatformChar>,
    _ size: Int
) -> Int {
    // `confstr` is only reached from `homeDirectoryFromPasswd`, which is gated to
    // non-Windows platforms; this exists solely so the module links. Report the
    // POSIX "unsupported name" result of 0.
    0
}

func fdopendir(_ fd: FileDescriptor.RawValue) -> CInterop.DirPointer? {
    fatalError("fdopendir is unavailable on Windows")
}

func readdir(_ dir: CInterop.DirPointer) -> UnsafeMutablePointer<CInterop.DirEnt>? {
    fatalError("readdir is unavailable on Windows")
}

func closedir(_ dir: CInterop.DirPointer) -> CInt {
    fatalError("closedir is unavailable on Windows")
}

// MARK: - FTS stubs

func fts_open(
    _ path: [UnsafeMutablePointer<CInterop.PlatformChar>?],
    _ options: CInt,
    _ compare: UnsafeRawPointer?
) -> UnsafeMutablePointer<CInterop.FTS>? {
    fatalError("fts_open is unavailable on Windows")
}

func fts_read(
    _ fts: UnsafeMutablePointer<CInterop.FTS>
) -> UnsafeMutablePointer<CInterop.FTSEnt>? {
    fatalError("fts_read is unavailable on Windows")
}

func fts_close(_ fts: UnsafeMutablePointer<CInterop.FTS>) -> CInt {
    fatalError("fts_close is unavailable on Windows")
}

// MARK: - dirent name accessor

/// Windows equivalent of `CNIOLinux_dirent_dname` / `CNIODarwin_dirent_dname`.
func CNIOWindows_dirent_dname(
    _ entry: UnsafeMutablePointer<CInterop.DirEnt>
) -> UnsafePointer<CInterop.PlatformChar> {
    fatalError("CNIOWindows_dirent_dname is unavailable on Windows")
}

// MARK: - pthread TLS stubs

func pthread_key_create(
    _ key: UnsafeMutablePointer<pthread_key_t>,
    _ destructor: (@convention(c) (UnsafeMutableRawPointer?) -> Void)?
) -> CInt {
    CNIOWindows_fls_key_create(key, destructor)
}

func pthread_setspecific(_ key: pthread_key_t, _ value: UnsafeRawPointer?) -> CInt {
    CNIOWindows_fls_set(key, value)
}

func pthread_getspecific(_ key: pthread_key_t) -> UnsafeMutableRawPointer? {
    CNIOWindows_fls_get(key)
}

#endif  // os(Windows)
