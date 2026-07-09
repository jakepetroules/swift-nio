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
// It supplies the POSIX-shaped types, constants and free functions the rest of
// the target expects, mapping them onto Win32 / the CRT (mostly by delegating to
// the `CNIOWindows` C shims). Runtime behaviour has not yet been validated by
// the test suite; correctness is a follow-up.
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
    /// `CChar` so the existing `.0/.1/.2` "is this '.' or '..'" check compiles;
    /// the full (wide) name is held separately and returned by
    /// `CNIOWindows_dirent_dname`, since Windows names are UTF-16 and can exceed
    /// the narrow tuple. The pointer is owned by the directory stream and valid
    /// until the next `readdir`/`closedir`, matching POSIX `readdir` semantics.
    struct WindowsDirEnt {
        var d_type: UInt8 = 0
        var d_name:
            (
                CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar,
                CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar
            ) = (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        var d_name_wide: UnsafeMutablePointer<CInterop.PlatformChar>? = nil

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

/// Length in `PlatformChar` units of a NUL-terminated wide string, excluding the
/// terminator.
func _nio_fs_wideLength(_ s: UnsafePointer<CInterop.PlatformChar>) -> Int {
    var length = 0
    while s[length] != 0 { length += 1 }
    return length
}

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

// Decodes the swift-system-gap OpenOptions bits we own out of `oflag` into the
// `windowsFlags` the C `openat` shim expects, so the C side needn't know their
// numeric values.
private func _nio_fs_windowsOpenFlags(_ oflag: CInt) -> CInt {
    var flags: CInt = 0
    if (oflag & FileDescriptor.OpenOptions.noFollow.rawValue) != 0 { flags |= CNIO_O_NOFOLLOW }
    if (oflag & FileDescriptor.OpenOptions.directory.rawValue) != 0 { flags |= CNIO_O_DIRECTORY }
    if (oflag & FileDescriptor.OpenOptions.closeOnExec.rawValue) != 0 { flags |= CNIO_O_CLOEXEC }
    return flags
}

func openat(
    _ fd: FileDescriptor.RawValue,
    _ path: UnsafePointer<CInterop.PlatformChar>,
    _ oflag: CInt
) -> CInt {
    CNIOWindows_openat(fd, path, oflag, _nio_fs_windowsOpenFlags(oflag), 0)
}

func openat(
    _ fd: FileDescriptor.RawValue,
    _ path: UnsafePointer<CInterop.PlatformChar>,
    _ oflag: CInt,
    _ mode: CInterop.Mode
) -> CInt {
    CNIOWindows_openat(fd, path, oflag, _nio_fs_windowsOpenFlags(oflag), UInt32(mode))
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
    // POSIX `symlink(target, linkpath)`; NIOFS passes (destination=target,
    // source=linkpath) matching that order.
    CNIOWindows_symlink(destination, source)
}

func symlinkat(
    _ destination: UnsafePointer<CInterop.PlatformChar>,
    _ dirfd: FileDescriptor.RawValue,
    _ source: UnsafePointer<CInterop.PlatformChar>
) -> CInt {
    CNIOWindows_symlinkat(destination, dirfd, source)
}

func readlink(
    _ path: UnsafePointer<CInterop.PlatformChar>,
    _ buffer: UnsafeMutablePointer<CInterop.PlatformChar>,
    _ size: Int
) -> Int {
    Int(CNIOWindows_readlink(path, buffer, size))
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
    // A non-zero `flags` carries AT_REMOVEDIR (the only flag NIOFS would pass).
    CNIOWindows_unlinkat(fd, path, flags != 0 ? 1 : 0)
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

// MARK: - Directory streams (opendir / readdir / closedir)
//
// The C layer (CNIOWindows_dir_*) provides raw FindFirstFileW iteration; the
// stateful stream — owning the current `dirent` and its wide name buffer — lives
// here so it can build the Swift `WindowsDirEnt` the rest of NIOFS reads.

private final class WindowsDirectoryStream {
    /// Opaque `CNIOWindows_DirStream *`.
    private let handle: UnsafeMutableRawPointer
    /// Whether the native iterator has already been closed.
    private var closed = false
    /// Stable heap storage for the entry returned by `readdir`. Allocating it
    /// (rather than handing out a pointer to a stored property or Array buffer)
    /// keeps the address valid after `next()` returns, as POSIX `readdir`
    /// requires — the returned pointer stays valid until the next
    /// `readdir`/`closedir`.
    private let entry: UnsafeMutablePointer<CInterop.DirEnt>
    /// Heap storage for the wide name referenced by `entry.pointee.d_name_wide`,
    /// grown on demand and freed with the stream.
    private var wideName: UnsafeMutablePointer<CInterop.PlatformChar>
    private var wideNameCapacity: Int

    init?(handle: UnsafeMutableRawPointer?) {
        guard let handle = handle else { return nil }
        self.handle = handle
        self.entry = UnsafeMutablePointer<CInterop.DirEnt>.allocate(capacity: 1)
        self.entry.initialize(to: CInterop.DirEnt())
        self.wideNameCapacity = Int(CInterop.maxPathLength) + 1
        self.wideName = UnsafeMutablePointer<CInterop.PlatformChar>.allocate(
            capacity: self.wideNameCapacity
        )
    }

    /// Advances to the next entry, returning a pointer to the populated storage,
    /// nil at end-of-directory, or setting `errno` and returning nil on error.
    func next() -> UnsafeMutablePointer<CInterop.DirEnt>? {
        var type: UInt8 = 0
        let result = CNIOWindows_dir_next(
            self.handle,
            self.wideName,
            CInt(self.wideNameCapacity),
            &type
        )
        guard result == 1 else {
            // 0 (end) and -1 (error, errno set) both surface as nil; the caller
            // distinguishes them via errno through `optionalValueOrErrno`.
            return nil
        }

        self.entry.pointee.d_type = type
        self.entry.pointee.d_name_wide = self.wideName
        // Fill the narrow d_name tuple far enough for the "." / ".." check; the
        // full name is read via `dirent_dname` from `d_name_wide`.
        let nameLength = _nio_fs_wideLength(self.wideName)
        withUnsafeMutableBytes(of: &self.entry.pointee.d_name) { raw in
            let bytes = raw.bindMemory(to: CChar.self)
            for i in 0..<bytes.count { bytes[i] = 0 }
            // The wide name's leading units are ASCII for "." / ".." so a direct
            // truncating copy is sufficient for the sentinel comparison.
            for i in 0..<min(bytes.count - 1, nameLength) {
                bytes[i] = CChar(truncatingIfNeeded: self.wideName[i])
            }
        }
        return self.entry
    }

    func close() {
        guard !self.closed else { return }
        self.closed = true
        CNIOWindows_dir_close(self.handle)
    }

    deinit {
        // Backstop: close the native iterator if the caller abandoned the stream
        // without calling closedir, then free the owned storage.
        self.close()
        self.entry.deinitialize(count: 1)
        self.entry.deallocate()
        self.wideName.deallocate()
    }
}

func fdopendir(_ fd: FileDescriptor.RawValue) -> CInterop.DirPointer? {
    guard let stream = WindowsDirectoryStream(handle: CNIOWindows_dir_open_fd(fd)) else {
        return nil
    }
    return CInterop.DirPointer(Unmanaged.passRetained(stream).toOpaque())
}

func readdir(_ dir: CInterop.DirPointer) -> UnsafeMutablePointer<CInterop.DirEnt>? {
    let stream = Unmanaged<WindowsDirectoryStream>.fromOpaque(UnsafeRawPointer(dir)).takeUnretainedValue()
    return stream.next()
}

func closedir(_ dir: CInterop.DirPointer) -> CInt {
    let stream = Unmanaged<WindowsDirectoryStream>.fromOpaque(UnsafeRawPointer(dir))
    stream.takeUnretainedValue().close()
    stream.release()
    return 0
}

// MARK: - dirent name accessor

/// Windows equivalent of `CNIOLinux_dirent_dname` / `CNIODarwin_dirent_dname`:
/// returns the entry's full (wide) name.
func CNIOWindows_dirent_dname(
    _ entry: UnsafeMutablePointer<CInterop.DirEnt>
) -> UnsafePointer<CInterop.PlatformChar> {
    UnsafePointer(entry.pointee.d_name_wide!)
}

// MARK: - FTS (fts_open / fts_read / fts_close)
//
// A minimal fts(3) reimplementation over the directory-iteration primitives. It
// honours the flags NIOFS passes (FTS_PHYSICAL — the default — and FTS_NOCHDIR;
// FTS_LOGICAL would follow symlinks but NIOFS only uses physical walks) and
// emits the pre-order (FTS_D), post-order (FTS_DP), file (FTS_F), symlink
// (FTS_SL), and error (FTS_DNR/FTS_ERR) events the enumerator switch handles.

private final class WindowsFTS {
    private struct Frame {
        var stream: UnsafeMutableRawPointer  // CNIOWindows_DirStream *
        var path: String                     // directory path (no trailing separator)
    }

    private var stack: [Frame] = []
    /// Stable heap storage for the entry returned by `fts_read`, valid until the
    /// next `fts_read`/`fts_close` (matching fts(3)).
    private let ent: UnsafeMutablePointer<CInterop.FTSEnt>
    /// Heap storage for the wide path referenced by `ent.pointee.fts_path`, grown
    /// on demand and freed with the walk.
    private var pathStorage: UnsafeMutablePointer<CInterop.PlatformChar>
    private var pathCapacity: Int
    /// Scratch buffer for reading directory entry names.
    private let nameBuffer: UnsafeMutablePointer<CInterop.PlatformChar>
    private let nameCapacity: Int
    private var started = false
    private let rootPath: String

    init(rootPath: String, options: CInt) {
        self.rootPath = rootPath
        self.ent = UnsafeMutablePointer<CInterop.FTSEnt>.allocate(capacity: 1)
        self.ent.initialize(to: CInterop.FTSEnt())
        self.pathCapacity = Int(CInterop.maxPathLength) + 1
        self.pathStorage = UnsafeMutablePointer<CInterop.PlatformChar>.allocate(
            capacity: self.pathCapacity
        )
        self.nameCapacity = Int(CInterop.maxPathLength) + 1
        self.nameBuffer = UnsafeMutablePointer<CInterop.PlatformChar>.allocate(
            capacity: self.nameCapacity
        )
    }

    private func openDirectory(_ path: String) -> UnsafeMutableRawPointer? {
        path.withPlatformString { CNIOWindows_dir_open_path($0) }
    }

    /// Emits an event by populating the owned `ent`/`pathStorage` and returning a
    /// stable pointer to it.
    private func emit(info: CInt, errno: CInt, path: String) -> UnsafeMutablePointer<CInterop.FTSEnt> {
        let units = Array(path.utf16)
        // Grow the owned path buffer if needed (path + NUL).
        if units.count + 1 > self.pathCapacity {
            self.pathStorage.deallocate()
            self.pathCapacity = units.count + 1
            self.pathStorage = UnsafeMutablePointer<CInterop.PlatformChar>.allocate(
                capacity: self.pathCapacity
            )
        }
        for i in 0..<units.count {
            self.pathStorage[i] = CInterop.PlatformChar(units[i])
        }
        self.pathStorage[units.count] = 0
        self.ent.pointee.fts_info = UInt16(truncatingIfNeeded: info)
        self.ent.pointee.fts_errno = errno
        self.ent.pointee.fts_path = self.pathStorage
        return self.ent
    }

    func next() -> UnsafeMutablePointer<CInterop.FTSEnt>? {
        // The very first read emits the root as a pre-order directory and opens it.
        if !self.started {
            self.started = true
            guard let stream = self.openDirectory(self.rootPath) else {
                return self.emit(info: FTS_DNR, errno: _nio_fs_errno, path: self.rootPath)
            }
            self.stack.append(Frame(stream: stream, path: self.rootPath))
            return self.emit(info: FTS_D, errno: 0, path: self.rootPath)
        }

        while let frame = self.stack.last {
            var type: UInt8 = 0
            let result = CNIOWindows_dir_next(
                frame.stream,
                self.nameBuffer,
                CInt(self.nameCapacity),
                &type
            )

            if result == -1 {
                let err = _nio_fs_errno
                return self.emit(info: FTS_ERR, errno: err, path: frame.path)
            }
            if result == 0 {
                // End of this directory: close it, pop, and emit its post-order event.
                CNIOWindows_dir_close(frame.stream)
                self.stack.removeLast()
                return self.emit(info: FTS_DP, errno: 0, path: frame.path)
            }

            let nameLength = _nio_fs_wideLength(self.nameBuffer)
            let name = String(
                decoding: UnsafeBufferPointer(start: self.nameBuffer, count: nameLength)
                    .map { UInt16($0) },
                as: UTF16.self
            )
            if name == "." || name == ".." {
                continue
            }
            let childPath = frame.path + "\\" + name

            switch CInt(type) {
            case DT_DIR:
                // Descend: open the child and emit its pre-order event. If it
                // can't be opened, report it as unreadable and keep going.
                guard let childStream = self.openDirectory(childPath) else {
                    return self.emit(info: FTS_DNR, errno: _nio_fs_errno, path: childPath)
                }
                self.stack.append(Frame(stream: childStream, path: childPath))
                return self.emit(info: FTS_D, errno: 0, path: childPath)
            case DT_LNK:
                return self.emit(info: FTS_SL, errno: 0, path: childPath)
            case DT_REG:
                return self.emit(info: FTS_F, errno: 0, path: childPath)
            default:
                return self.emit(info: FTS_DEFAULT, errno: 0, path: childPath)
            }
        }

        return nil  // walk complete
    }

    func close() {
        while let frame = self.stack.popLast() {
            CNIOWindows_dir_close(frame.stream)
        }
    }

    deinit {
        // Backstop: close any directories still open if the walk was abandoned,
        // then free the owned storage.
        self.close()
        self.ent.deinitialize(count: 1)
        self.ent.deallocate()
        self.pathStorage.deallocate()
        self.nameBuffer.deallocate()
    }
}

func fts_open(
    _ path: [UnsafeMutablePointer<CInterop.PlatformChar>?],
    _ options: CInt,
    _ compare: UnsafeRawPointer?
) -> UnsafeMutablePointer<CInterop.FTS>? {
    // NIOFS passes a single root path followed by a nil terminator.
    guard let first = path.first, let root = first else {
        return nil
    }
    let rootPath = String(decodingCString: root, as: UTF16.self)
    let fts = WindowsFTS(rootPath: rootPath, options: options)
    let opaque = CInterop.FTS(Unmanaged.passRetained(fts).toOpaque())
    // `CInterop.FTS` is `OpaquePointer`; hand back a pointer to it as the API
    // expects `UnsafeMutablePointer<CInterop.FTS>`.
    let box = UnsafeMutablePointer<CInterop.FTS>.allocate(capacity: 1)
    box.initialize(to: opaque)
    return box
}

func fts_read(
    _ fts: UnsafeMutablePointer<CInterop.FTS>
) -> UnsafeMutablePointer<CInterop.FTSEnt>? {
    let instance = Unmanaged<WindowsFTS>.fromOpaque(UnsafeRawPointer(fts.pointee)).takeUnretainedValue()
    return instance.next()
}

func fts_close(_ fts: UnsafeMutablePointer<CInterop.FTS>) -> CInt {
    let unmanaged = Unmanaged<WindowsFTS>.fromOpaque(UnsafeRawPointer(fts.pointee))
    unmanaged.takeUnretainedValue().close()
    unmanaged.release()
    fts.deinitialize(count: 1)
    fts.deallocate()
    return 0
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
