#include "mmap.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace mmaplib {
namespace {
/// Throws a system_error for the current errno value.
/// operation identifies the failed system call in the exception message.
[[noreturn]] void throw_errno(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}

/// Validates a POSIX integer result where -1 indicates failure.
/// result is the syscall result and operation names the attempted operation.
void check_result(int result, const char* operation) {
  if (result == -1) {
    throw_errno(operation);
  }
}

/// Converts public open-related configuration into Linux open() flags.
/// config supplies access mode and file-creation behavior.
int open_flags(const Config& config) {
  int flags = config.access == Access::read_write ? O_RDWR : O_RDONLY;
  if (config.create_if_missing) {
    flags |= O_CREAT;
  }
  return flags | O_CLOEXEC;
}

/// Converts the requested access mode into mmap() protection bits.
/// config determines whether the returned mapping permits writes.
int protection(const Config& config) {
  return config.access == Access::read_write ? PROT_READ | PROT_WRITE
                                             : PROT_READ;
}

/// Converts the requested sharing policy into Linux mmap() flags.
/// config selects shared file-backed writes or private copy-on-write pages.
int mapping_flags(const Config& config) {
  return config.sharing == Sharing::shared ? MAP_SHARED : MAP_PRIVATE;
}

/// Acquires the configured non-blocking advisory lock on an open descriptor.
/// fd is the owned file descriptor and mode selects shared or exclusive access.
void acquire_lock(int fd, LockMode mode) {
  if (mode == LockMode::none) {
    return;
  }
  const int operation =
      (mode == LockMode::shared ? LOCK_SH : LOCK_EX) | LOCK_NB;
  check_result(::flock(fd, operation), "flock");
}

/// Converts the public seek origin to the constant expected by lseek().
/// whence identifies whether distance is relative to start, current, or end.
int seek_whence(SeekWhence whence) {
  switch (whence) {
    case SeekWhence::begin:
      return SEEK_SET;
    case SeekWhence::current:
      return SEEK_CUR;
    case SeekWhence::end:
      return SEEK_END;
  }
  throw std::invalid_argument("invalid seek origin");
}

/// Converts the public access hint into the corresponding madvise() value.
/// advice describes the caller's expected mapping access pattern.
int advice_value(Advice advice) {
  switch (advice) {
    case Advice::normal:
      return MADV_NORMAL;
    case Advice::sequential:
      return MADV_SEQUENTIAL;
    case Advice::random:
      return MADV_RANDOM;
    case Advice::will_need:
      return MADV_WILLNEED;
    case Advice::dont_need:
      return MADV_DONTNEED;
  }
  throw std::invalid_argument("invalid mapping advice");
}

/// Rejects option combinations that cannot be honored safely.
/// config is validated before opening or modifying the backing file.
void validate_config(const Config& config) {
  if (config.access == Access::read_only &&
      (config.create_if_missing || config.truncate_existing)) {
    throw std::invalid_argument(
        "creating or truncating a file requires read-write access");
  }
}
}  // namespace

class MmapFile::Impl {
 public:
  /// Opens path, validates config, optionally locks or sizes the file, and maps
  /// the requested region. config controls access, sharing, offset, and length.
  Impl(const std::filesystem::path& path, Config config) : config_(config) {
    validate_config(config_);
    if (config_.offset < 0) {
      throw std::invalid_argument("mmap offset cannot be negative");
    }
    page_size_ = ::sysconf(_SC_PAGESIZE);
    if (page_size_ <= 0 || config_.offset % page_size_ != 0) {
      throw std::invalid_argument("mmap offset must be page aligned");
    }
    // Validate caller-controlled arithmetic before open() can create a file or
    // truncate_existing can destroy its contents.
    if (config_.length != 0) {
      (void)mapping_end(config_.length);
    }
    fd_ = ::open(path.c_str(), open_flags(config_), 0644);
    if (fd_ == -1) {
      throw_errno("open");
    }
    try {
      acquire_lock(fd_, config_.locking);
      if (config_.truncate_existing) {
        check_result(::ftruncate(fd_, 0), "ftruncate");
      }
      const file_offset size = checked_file_size();
      const std::size_t requested = initial_length_for_file(size);
      if (size < config_.offset || requested > available_length(size)) {
        check_result(::ftruncate(fd_, mapping_end(requested)), "ftruncate");
      }
      install(map_new(requested), requested);
    } catch (...) {
      (void)::close(std::exchange(fd_, -1));
      throw;
    }
  }

  /// Releases the mapping and descriptor during automatic object cleanup.
  /// Errors cannot escape a destructor and are handled by no-throw helpers.
  ~Impl() noexcept {
    unmap_noexcept();
    if (fd_ != -1) {
      (void)::close(fd_);
    }
  }

  /// Explicitly releases the mapping and file descriptor.
  /// Repeated calls are harmless; a first-call munmap failure is reported.
  void close() {
    if (fd_ == -1) {
      return;
    }
    unmap_checked();
    (void)::close(std::exchange(fd_, -1));
  }

  /// Reports whether this implementation still owns an open file descriptor.
  [[nodiscard]] bool is_open() const noexcept {
    return fd_ != -1;
  }

  /// Returns the number of bytes represented by the active mapping.
  [[nodiscard]] std::size_t size() const noexcept {
    return length_;
  }

  /// Queries and returns the backing regular file's current byte size.
  [[nodiscard]] file_offset file_size() const {
    require_open();
    return checked_file_size();
  }

  /// Returns the configured byte offset where this mapping begins in the file.
  [[nodiscard]] file_offset offset() const noexcept {
    return config_.offset;
  }

  /// Creates a non-owning read-only span over the complete mapped region.
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return {static_cast<const std::byte*>(address_), length_};
  }

  /// Creates a writable span after confirming that write access was requested.
  [[nodiscard]] std::span<std::byte> mutable_bytes() {
    require_writable();
    return {static_cast<std::byte*>(address_), length_};
  }

  /// Returns a checked read-only subspan.
  /// position is mapping-relative and count is the requested byte count.
  [[nodiscard]] std::span<const std::byte> view(std::size_t position,
                                                std::size_t count) const {
    check_range(position, count);
    return bytes().subspan(position, count);
  }

  /// Returns a checked writable subspan.
  /// position is mapping-relative and count is the requested byte count.
  [[nodiscard]] std::span<std::byte> mutable_view(std::size_t position,
                                                  std::size_t count) {
    require_writable();
    check_range(position, count);
    return mutable_bytes().subspan(position, count);
  }

  /// Copies mapped bytes into caller-owned storage with overlap safety.
  /// destination receives data starting at mapping-relative position.
  [[nodiscard]] std::size_t read(std::span<std::byte> destination,
                                 std::size_t position) const {
    check_range(position, 0);
    const std::size_t count =
        std::min(destination.size(), length_ - position);
    if (count != 0) {
      std::memmove(destination.data(), bytes().data() + position, count);
    }
    return count;
  }

  /// Copies source bytes into the writable mapping with overlap safety.
  /// position is the mapping-relative destination offset.
  void write(std::span<const std::byte> source, std::size_t position) {
    require_writable();
    check_range(position, source.size());
    if (!source.empty()) {
      std::memmove(mutable_bytes().data() + position, source.data(),
                   source.size());
    }
  }

  /// Moves the descriptor's file position without changing pointer-based views.
  /// distance is interpreted relative to whence and the new offset is returned.
  [[nodiscard]] file_offset seek(file_offset distance,
                                 SeekWhence whence) const {
    require_open();
    const off_t result =
        ::lseek(fd_, static_cast<off_t>(distance), seek_whence(whence));
    if (result == static_cast<off_t>(-1)) {
      throw_errno("lseek");
    }
    return static_cast<file_offset>(result);
  }

  /// Changes the backing file to new_size bytes and rebuilds its mapping.
  /// new_size is an absolute file size, not a mapping-relative byte count.
  void resize(file_offset new_size) {
    require_writable();
    validate_resize_target(new_size);
    require_shared_growth();
    require_variable_mapping_matches_file();
    const file_offset old_size = checked_file_size();
    const std::size_t new_length = length_for_file(new_size);
    if (new_size == old_size && new_length == length_) {
      return;
    }
    if (new_size < old_size) {
      // The target range is contained in the current file, so it can be
      // installed before ftruncate(). If mmap or munmap fails, the file has
      // not changed and the old mapping remains valid. If ftruncate fails, the
      // new smaller mapping is still entirely within the unchanged file.
      replace_mapping(new_length);
      check_result(::ftruncate(fd_, static_cast<off_t>(new_size)),
                   "ftruncate");
      return;
    }
    check_result(::ftruncate(fd_, static_cast<off_t>(new_size)), "ftruncate");
    try {
      replace_mapping(new_length);
    } catch (...) {
      // An exclusive advisory lock establishes the ownership needed to safely
      // restore the old extent. Without that contract, preserve the actual
      // file size and require remap() before another structural operation.
      if (config_.locking == LockMode::exclusive) {
        (void)::ftruncate(fd_, static_cast<off_t>(old_size));
      }
      throw;
    }
  }

  /// Inserts source at a mapping-relative position and shifts the old suffix.
  /// source is copied first so it may safely alias the mapping being replaced.
  void insert(std::span<const std::byte> source, std::size_t position) {
    require_writable();
    require_shared_growth();
    require_variable_mapping_matches_file();
    check_range(position, 0);
    if (source.empty()) {
      return;
    }
    if (config_.length != 0) {
      throw std::logic_error("fixed-length mappings cannot be grown by insert");
    }
    if (source.size() > std::numeric_limits<std::size_t>::max() - length_) {
      throw std::length_error("insert size overflows mapping length");
    }
    const std::vector<std::byte> saved(source.begin(), source.end());
    const file_offset old_size = checked_file_size();
    if (saved.size() >
            static_cast<std::size_t>(std::numeric_limits<file_offset>::max()) ||
        old_size > std::numeric_limits<file_offset>::max() -
                       static_cast<file_offset>(saved.size())) {
      throw std::length_error("insert size overflows file size");
    }
    const std::size_t old_length = length_;
    resize(old_size + static_cast<file_offset>(saved.size()));
    auto mapped = mutable_bytes();
    std::memmove(mapped.data() + position + saved.size(),
                 mapped.data() + position, old_length - position);
    std::memcpy(mapped.data() + position, saved.data(), saved.size());
  }

  /// Rebuilds the mapping to reflect the backing file's current extent.
  /// The file size is observed but never modified by this operation.
  void remap() {
    require_open();
    if (config_.access == Access::read_write &&
        config_.sharing == Sharing::private_copy) {
      throw std::logic_error(
          "remap would discard private writable mapping changes");
    }
    const file_offset current_size = checked_file_size();
    const std::size_t new_length = length_for_remap(current_size);
    if (new_length == length_) {
      return;
    }
    replace_mapping(new_length);
  }

  /// Writes a mapped range back to storage synchronously.
  /// position and count select bytes; invalidate also requests cache
  /// invalidation.
  void sync(std::size_t position, std::size_t count, bool invalidate) {
    require_open();
    if (config_.sharing != Sharing::shared) {
      throw std::logic_error("sync requires a shared mapping");
    }
    check_range(position, count);
    if (count == 0) {
      return;
    }
    if (position % static_cast<std::size_t>(page_size_) != 0) {
      throw std::invalid_argument("sync range position must be page aligned");
    }
    if (::msync(static_cast<std::byte*>(address_) + position, count,
                MS_SYNC | (invalidate ? MS_INVALIDATE : 0)) == -1) {
      throw_errno("msync");
    }
  }

  /// Supplies the kernel with a usage hint for the full mapping.
  /// advice selects the expected access or page-reclamation behavior.
  void advise(Advice advice) {
    require_open();
    if (advice == Advice::dont_need &&
        config_.access == Access::read_write &&
        config_.sharing == Sharing::private_copy) {
      throw std::logic_error(
          "dont_need may discard private mapping modifications");
    }
    if (length_ != 0 &&
        ::madvise(address_, length_, advice_value(advice)) == -1) {
      throw_errno("madvise");
    }
  }

 private:
  /// Ensures that this instance still owns an open file descriptor.
  void require_open() const {
    if (fd_ == -1) {
      throw std::logic_error("mmap file is closed");
    }
  }

  /// Ensures that the configured mapping permits caller-visible writes.
  void require_writable() const {
    require_open();
    if (config_.access != Access::read_write) {
      throw std::logic_error("mmap is not writable");
    }
  }

  /// Ensures that structural growth is compatible with sharing and lock mode.
  void require_shared_growth() const {
    if (config_.sharing != Sharing::shared) {
      throw std::logic_error(
          "resizing a private mapping is unsupported because it would discard "
          "copy-on-write data");
    }
    if (config_.locking == LockMode::shared) {
      throw std::logic_error(
          "structural modification is not permitted under a shared lock");
    }
  }

  /// Rejects stale automatically sized mappings before structural mutation.
  /// The check detects size changes but cannot close races with external
  /// writers.
  void require_variable_mapping_matches_file() const {
    if (config_.length == 0 && checked_file_size() != mapping_end(length_)) {
      throw std::logic_error(
          "backing file size changed; call remap() before modifying it");
    }
  }

  /// Validates a mapping-relative half-open byte range without integer
  /// overflow.
  /// position is the first byte and count is the number of requested bytes.
  void check_range(std::size_t position, std::size_t count) const {
    require_open();
    if (position > length_ || count > length_ - position) {
      throw std::out_of_range("mmap range is outside the mapped region");
    }
  }

  /// Returns the current size after verifying that fd_ refers to a regular
  /// file.
  [[nodiscard]] file_offset checked_file_size() const {
    struct stat st{};
    check_result(::fstat(fd_, &st), "fstat");
    if (!S_ISREG(st.st_mode) || st.st_size < 0) {
      throw std::invalid_argument("mmaplib supports regular files only");
    }
    return static_cast<file_offset>(st.st_size);
  }

  /// Converts a file size into bytes available after the configured offset.
  /// size is the complete backing-file extent in bytes.
  [[nodiscard]] std::size_t available_length(file_offset size) const {
    if (size < config_.offset) {
      throw std::invalid_argument("mmap offset is beyond the file");
    }
    const auto available = static_cast<std::uintmax_t>(size - config_.offset);
    if (available > std::numeric_limits<std::size_t>::max()) {
      throw std::length_error("mmap length is too large");
    }
    return static_cast<std::size_t>(available);
  }

  /// Calculates the desired mapping length for a given complete file size.
  /// size is checked against fixed-length and read-only configuration rules.
  [[nodiscard]] std::size_t length_for_file(file_offset size) const {
    const std::size_t available = available_length(size);
    if (config_.length == 0) {
      return available;
    }
    if (config_.length > available && config_.access == Access::read_only) {
      throw std::invalid_argument("read-only mapping extends beyond the file");
    }
    return config_.length;
  }

  /// Calculates the initial mapping length and permits configured file growth.
  /// size is the backing-file size observed immediately after open and locking.
  [[nodiscard]] std::size_t initial_length_for_file(file_offset size) const {
    if (size < config_.offset) {
      if (config_.access == Access::read_write && config_.length != 0) {
        return config_.length;
      }
      throw std::invalid_argument("mmap offset is beyond the file");
    }
    return length_for_file(size);
  }

  /// Calculates a safe length when remapping an already open file.
  /// size must already contain every byte required by a fixed-length mapping.
  [[nodiscard]] std::size_t length_for_remap(file_offset size) const {
    const std::size_t available = available_length(size);
    if (config_.length != 0 && config_.length > available) {
      throw std::invalid_argument("mapping extends beyond the backing file");
    }
    return config_.length == 0 ? available : config_.length;
  }

  /// Calculates the absolute file offset immediately after a mapping.
  /// length is checked with config_.offset for uintmax_t and off_t overflow.
  [[nodiscard]] off_t mapping_end(std::size_t length) const {
    const auto offset = static_cast<std::uintmax_t>(config_.offset);
    if (length > std::numeric_limits<std::uintmax_t>::max() - offset ||
        offset + length >
            static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max())) {
      throw std::length_error("mmap end offset overflows off_t");
    }
    return static_cast<off_t>(offset + length);
  }

  /// Validates a requested absolute file size before resize changes any state.
  /// new_size must retain the configured offset and any fixed mapping extent.
  void validate_resize_target(file_offset new_size) const {
    if (new_size < config_.offset) {
      throw std::invalid_argument(
          "file size cannot be smaller than mapping offset");
    }
    if (static_cast<std::uintmax_t>(new_size) >
        static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max())) {
      throw std::length_error("file size exceeds off_t range");
    }
    if (config_.length != 0 &&
        available_length(new_size) < config_.length) {
      throw std::invalid_argument(
          "file size cannot be smaller than the fixed mapping");
    }
  }

  /// Creates a new mapping using the stored access and sharing configuration.
  /// length is the number of bytes mapped from config_.offset.
  [[nodiscard]] void* map_new(std::size_t length) const {
    if (length == 0) {
      return nullptr;
    }
    int flags = mapping_flags(config_);
    if (config_.prefault) {
      flags |= MAP_POPULATE;
    }
    void* result =
        ::mmap(nullptr, length, protection(config_), flags, fd_,
               config_.offset);
    if (result == MAP_FAILED) {
      throw_errno("mmap");
    }
    return result;
  }

  /// Publishes a successfully created mapping in this implementation object.
  /// address and length must describe the same live mapping, or an empty one.
  void install(void* address, std::size_t length) noexcept {
    address_ = address;
    length_ = length;
  }

  /// Unmaps the active region and reports failure without forgetting ownership.
  void unmap_checked() {
    if (address_ != nullptr && ::munmap(address_, length_) == -1) {
      throw_errno("munmap");
    }
    address_ = nullptr;
    length_ = 0;
  }

  /// Best-effort destructor cleanup for the active mapping.
  /// munmap errors are ignored because this function cannot throw.
  void unmap_noexcept() noexcept {
    if (address_ != nullptr) {
      (void)::munmap(address_, length_);
      address_ = nullptr;
      length_ = 0;
    }
  }

  /// Allocates and installs a replacement while preserving the old map on
  /// allocation or unmap failure. length is the new mapped byte count.
  void replace_mapping(std::size_t length) {
    void* replacement = map_new(length);
    replace_mapping_with_prepared(replacement, length);
  }

  /// Installs a pre-created replacement after releasing the old mapping.
  /// replacement and length identify the new mapping to adopt on success.
  void replace_mapping_with_prepared(void* replacement, std::size_t length) {
    try {
      unmap_checked();
    } catch (...) {
      if (replacement != nullptr) {
        (void)::munmap(replacement, length);
      }
      throw;
    }
    install(replacement, length);
  }

  Config config_;
  int fd_{-1};
  void* address_{nullptr};
  std::size_t length_{0};
  long page_size_{0};
};

/// Constructs the public owner for path using the requested mapping config.
MmapFile::MmapFile(const std::filesystem::path& path, Config config)
    : impl_(std::make_unique<Impl>(path, config)) {
}

/// Releases owned resources through Impl's no-throw destructor.
MmapFile::~MmapFile() noexcept = default;

/// Transfers mapping ownership and leaves the source object closed.
MmapFile::MmapFile(MmapFile&&) noexcept = default;

/// Releases current resources, then transfers ownership from the source object.
MmapFile& MmapFile::operator=(MmapFile&&) noexcept = default;

/// Explicitly releases resources; calling close repeatedly is safe.
void MmapFile::close() {
  if (impl_ != nullptr) {
    impl_->close();
  }
}

/// Reports whether this object currently owns an open mapped file.
bool MmapFile::is_open() const noexcept {
  return impl_ != nullptr && impl_->is_open();
}

/// Returns the current mapped byte count, or zero for a moved-from object.
std::size_t MmapFile::size() const noexcept {
  return impl_ == nullptr ? 0 : impl_->size();
}

/// Returns the backing file's current absolute byte size.
file_offset MmapFile::file_size() const {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  return impl_->file_size();
}

/// Returns the configured mapping offset, or zero after ownership was moved.
file_offset MmapFile::offset() const noexcept {
  return impl_ == nullptr ? 0 : impl_->offset();
}

/// Returns the current mapped byte count as the explicit mapping-length API.
std::size_t MmapFile::mapping_length() const noexcept {
  return size();
}

/// Returns a non-owning read-only span over all currently mapped bytes.
std::span<const std::byte> MmapFile::bytes() const noexcept {
  return impl_ == nullptr ? std::span<const std::byte>{}
                          : static_cast<const Impl&>(*impl_).bytes();
}

/// Returns a non-owning writable span after validating mapping access.
std::span<std::byte> MmapFile::mutable_bytes() {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  return impl_->mutable_bytes();
}

/// Returns a checked read-only mapping subspan.
/// position is mapping-relative and count is the requested number of bytes.
std::span<const std::byte> MmapFile::view(std::size_t position,
                                          std::size_t count) const {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  return static_cast<const Impl&>(*impl_).view(position, count);
}

/// Returns a checked writable mapping subspan.
/// position is mapping-relative and count is the requested number of bytes.
std::span<std::byte> MmapFile::mutable_view(std::size_t position,
                                            std::size_t count) {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  return impl_->mutable_view(position, count);
}

/// Copies bytes from the mapping into destination starting at position.
std::size_t MmapFile::read(std::span<std::byte> destination,
                           std::size_t position) const {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  return static_cast<const Impl&>(*impl_).read(destination, position);
}

/// Copies binary source bytes into the mapping at the given position.
void MmapFile::write(std::span<const std::byte> source,
                     std::size_t position) {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  impl_->write(source, position);
}

/// Copies text source bytes, excluding any terminator, at the given position.
void MmapFile::write(std::string_view source, std::size_t position) {
  write(std::as_bytes(std::span{source.data(), source.size()}), position);
}

/// Inserts binary source bytes before the mapping-relative position.
void MmapFile::insert(std::span<const std::byte> source,
                      std::size_t position) {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  impl_->insert(source, position);
}

/// Inserts text bytes, excluding any terminator, before position.
void MmapFile::insert(std::string_view source, std::size_t position) {
  insert(std::as_bytes(std::span{source.data(), source.size()}), position);
}

/// Appends binary source bytes after the current mapped extent.
void MmapFile::append(std::span<const std::byte> source) {
  insert(source, mapping_length());
}

/// Appends text bytes, excluding any terminator, after the current extent.
void MmapFile::append(std::string_view source) {
  insert(source, mapping_length());
}

/// Moves the descriptor cursor by distance relative to whence.
file_offset MmapFile::seek(file_offset distance, SeekWhence whence) const {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  return impl_->seek(distance, whence);
}

/// Changes the backing file to the requested absolute new_size.
void MmapFile::resize(file_offset new_size) {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  impl_->resize(new_size);
}

/// Rebuilds the mapping from the backing file's current size.
void MmapFile::remap() {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  impl_->remap();
}

/// Synchronizes the complete mapping; invalidate requests cache invalidation.
void MmapFile::sync(bool invalidate) {
  sync_range(0, mapping_length(), invalidate);
}

/// Synchronizes count bytes beginning at page-aligned mapping position.
/// invalidate also asks the kernel to invalidate cached pages where supported.
void MmapFile::sync_range(std::size_t position, std::size_t count,
                          bool invalidate) {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  impl_->sync(position, count, invalidate);
}

/// Supplies advice for the complete mapping to the Linux memory manager.
void MmapFile::advise(Advice advice) {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  impl_->advise(advice);
}

}  // namespace mmaplib
