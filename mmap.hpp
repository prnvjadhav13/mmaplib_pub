#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace mmaplib {

/// Signed file offset type used by the portable API.
using file_offset = std::int64_t;

/// Access permissions requested for the mapped file.
enum class Access {
  /// Permit reads only.
  read_only,
  /// Permit reads and writes through the mapping.
  read_write
};

/// Determines whether writes are private to this process or shared with the
/// file.
enum class Sharing {
  /// Use copy-on-write; modifications are not written back to the file.
  private_copy,
  /// Share modifications with other mappings and write them back to the file.
  shared
};

/// Optional advisory lock held for the lifetime of an open MmapFile.
///
/// Locks coordinate only with processes which request compatible locks. They
/// cannot protect a mapping from an unrelated process that truncates or
/// replaces the backing file.
enum class LockMode {
  none,
  shared,
  exclusive
};

/// Origin used when changing the file descriptor's current position with
/// seek().
enum class SeekWhence {
  /// Interpret distance from byte zero of the file.
  begin,
  /// Interpret distance from the current file position.
  current,
  /// Interpret distance from the current end of the file.
  end
};

/// Optional access-pattern hint supplied to the operating system's
/// virtual-memory manager.
enum class Advice {
  /// No special access pattern is expected.
  normal,
  /// Pages are expected to be accessed in increasing order.
  sequential,
  /// Pages are expected to be accessed in a non-sequential pattern.
  random,
  /// Pages are likely to be accessed soon.
  will_need,
  /// Pages are unlikely to be accessed soon.
  dont_need
};

/// Platform-neutral options used when opening and mapping a file.
struct Config {
  /// Read-only mappings reject writes; read-write mappings permit
  /// mutable_bytes() and write().
  Access access{Access::read_only};

  /// Shared mappings can persist modifications to the file; private mappings
  /// use copy-on-write.
  Sharing sharing{Sharing::private_copy};

  /// Optional non-blocking advisory lock acquired when the file is opened.
  /// A conflicting lock makes construction throw std::system_error. Use
  /// exclusive when this process owns resize or truncate operations; every
  /// cooperating process must also request a compatible lock.
  LockMode locking{LockMode::none};

  /// If true, create the file when it does not already exist. Requires
  /// read-write access.
  bool create_if_missing{false};

  /// If true, truncate an existing file before mapping it. Requires read-write
  /// access.
  bool truncate_existing{false};

  /// Byte offset in the file at which the mapping begins. It must satisfy the
  /// platform's alignment requirements.
  file_offset offset{0};

  /// Number of bytes to map. Zero maps from offset through the current end of
  /// the file. For a writable mapping, a nonzero length may extend the file to
  /// the requested size.
  std::size_t length{0};

  /// If true, ask Linux to populate the mapping before the constructor returns.
  /// This increases open latency and memory pressure, but can reduce
  /// first-access page faults.
  bool prefault{false};
};

class MmapFile final {
 public:
  /// Opens path and creates its mapping according to config.
  /// Throws std::system_error for operating-system failures (including a
  /// conflicting requested advisory lock) and std::invalid_argument for
  /// invalid options. The mapping is released automatically when the object
  /// is destroyed.
  explicit MmapFile(const std::filesystem::path& path, Config config = {});

  /// Releases the mapping and underlying file resource. Never throws.
  ~MmapFile() noexcept;

  /// Mappings own an operating-system resource and therefore cannot be copied.
  MmapFile(const MmapFile&) = delete;
  MmapFile& operator=(const MmapFile&) = delete;

  /// Transfers mapping ownership from other. The source becomes closed.
  MmapFile(MmapFile&& other) noexcept;
  MmapFile& operator=(MmapFile&& other) noexcept;

  /// Explicitly releases the mapping and file resource. Safe to call more than
  /// once. Throws std::system_error if unmapping fails on the first close.
  /// Close errors for regular files are ignored.
  void close();

  /// Returns true while the file and its mapping are owned by this object.
  [[nodiscard]] bool is_open() const noexcept;

  /// Returns the number of bytes currently mapped. This is not necessarily the
  /// complete file size.
  [[nodiscard]] std::size_t size() const noexcept;

  /// Returns the current size of the underlying file, in bytes.
  /// This may differ from mapping_length() if the file changes externally.
  /// Throws std::logic_error when the object is moved-from or closed.
  [[nodiscard]] file_offset file_size() const;

  /// Returns the file offset supplied in Config at construction time.
  [[nodiscard]] file_offset offset() const noexcept;

  /// Returns the number of bytes in the active virtual-memory mapping.
  [[nodiscard]] std::size_t mapping_length() const noexcept;

  /// Returns a read-only zero-copy view of the mapped bytes.
  /// The returned span is valid only while this object remains open and the
  /// mapped region is unchanged. Access to the span must be externally
  /// synchronized with close(), resize(), insert(), append(), move-assignment,
  /// and destruction.
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept;

  /// Returns a writable zero-copy view of the mapped bytes.
  /// This deliberately has a distinct name so a non-const read-only MmapFile
  /// still exposes bytes(). Throws std::logic_error when the mapping was opened
  /// read-only.
  [[nodiscard]] std::span<std::byte> mutable_bytes();

  /// Returns a checked read-only zero-copy subview beginning at position for
  /// count bytes. Throws std::out_of_range when the requested range is outside
  /// the mapping. Throws std::logic_error when the object is moved-from or
  /// closed.
  [[nodiscard]] std::span<const std::byte> view(std::size_t position,
                                                std::size_t count) const;

  /// Returns a checked writable zero-copy subview beginning at position for
  /// count bytes. Throws std::logic_error for read-only mappings and
  /// std::out_of_range for invalid ranges.
  [[nodiscard]] std::span<std::byte> mutable_view(std::size_t position,
                                                  std::size_t count);

  /// Copies up to destination.size() bytes from the mapping into destination
  /// starting at position. Returns the number of bytes copied. This is the
  /// copying alternative to bytes() and view(). Throws std::logic_error when
  /// the object is moved-from or closed.
  [[nodiscard]] std::size_t read(std::span<std::byte> destination,
                                 std::size_t position = 0) const;

  /// Copies source into the mapping starting at position.
  /// Throws std::logic_error for read-only mappings and std::out_of_range for
  /// invalid ranges. Throws std::logic_error when the object is moved-from or
  /// closed.
  void write(std::span<const std::byte> source, std::size_t position = 0);

  /// Copies the bytes of a string view into the mapping starting at position.
  /// The terminating null character, if any, is not written.
  /// Throws std::logic_error for read-only mappings and std::out_of_range for
  /// invalid ranges.
  void write(std::string_view source, std::size_t position = 0);

  /// Inserts binary data at position and shifts the existing suffix toward the
  /// end of the file. The mapping grows to accommodate source. Existing spans
  /// become invalid after this call. Fixed-length mappings cannot be grown and
  /// are rejected. Throws std::logic_error for read-only or fixed-length
  /// mappings and std::out_of_range for invalid positions.
  void insert(std::span<const std::byte> source, std::size_t position = 0);

  /// Inserts string contents at position without inserting a terminating null
  /// character. Existing bytes at position and after it are shifted toward the
  /// end of the file.
  void insert(std::string_view source, std::size_t position = 0);

  /// Appends binary data after the current end of the mapping.
  /// The mapping grows and existing spans become invalid after this call.
  void append(std::span<const std::byte> source);

  /// Appends string contents after the current end of the mapping without its
  /// terminating null character.
  void append(std::string_view source);

  /// Moves the file's current position by distance relative to whence and
  /// returns the new position. This affects seek-related file operations, not
  /// pointer-based bytes() or view() access. Throws std::system_error if the
  /// native seek operation fails.
  [[nodiscard]] file_offset seek(file_offset distance,
                                 SeekWhence whence = SeekWhence::begin) const;

  /// Changes the underlying file size and recreates the mapping with the
  /// resulting range. Existing spans become invalid after this call and must
  /// not be used. Growth is supported only for shared writable mappings;
  /// private mappings retain copy-on-write pages. Throws std::invalid_argument,
  /// std::logic_error, or std::system_error on failure. For shrink operations,
  /// the replacement mapping is prepared before the file is truncated so an
  /// mmap failure leaves the original mapping and file unchanged. The operation
  /// is not transactional against external writers; use LockMode::exclusive
  /// when all file users cooperate. A failed shrink may leave a valid smaller
  /// mapping over the unchanged larger file; remap() restores an automatically
  /// sized mapping to the current extent.
  void resize(file_offset new_size);

  /// Recreates the mapping for the backing file's current size without
  /// changing that size. This is the explicit recovery operation after a
  /// detected external size change or a failed growth operation. Existing
  /// spans become invalid after a successful call. It cannot make an mmap
  /// safe against concurrent, non-cooperating truncation. Writable private
  /// mappings reject this operation because remapping would silently discard
  /// their copy-on-write modifications.
  void remap();

  /// Requests synchronous writeback of modified shared-mapping pages.
  /// Throws std::logic_error for private copy-on-write mappings.
  /// invalidate also asks the operating system to invalidate cached pages where
  /// supported. This method does not protect against another process truncating
  /// the file.
  void sync(bool invalidate = false);

  /// Requests synchronous writeback of a mapped subrange. position must be page
  /// aligned. This avoids flushing an entire large mapping when only a
  /// committed record range changed.
  void sync_range(std::size_t position, std::size_t count,
                  bool invalidate = false);

  /// Supplies an access-pattern hint to the operating system; it does not
  /// change file contents. Advice is a performance hint and may be ignored by
  /// the operating system.
  void advise(Advice advice);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mmaplib
