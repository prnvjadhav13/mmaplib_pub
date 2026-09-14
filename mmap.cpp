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
[[noreturn]] void throw_errno(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}
void check_result(int result, const char* operation) {
  if (result == -1) {
    throw_errno(operation);
  }
}
int open_flags(const Config& c) {
  int flags = c.access == Access::read_write ? O_RDWR : O_RDONLY;
  if (c.create_if_missing) {
    flags |= O_CREAT;
  }
  return flags | O_CLOEXEC;
}
int protection(const Config& c) {
  return c.access == Access::read_write ? PROT_READ | PROT_WRITE : PROT_READ;
}
int mapping_flags(const Config& c) {
  return c.sharing == Sharing::shared ? MAP_SHARED : MAP_PRIVATE;
}
void acquire_lock(int fd, LockMode mode) {
  if (mode == LockMode::none) {
    return;
  }
  const int operation =
      (mode == LockMode::shared ? LOCK_SH : LOCK_EX) | LOCK_NB;
  check_result(::flock(fd, operation), "flock");
}
int seek_whence(SeekWhence w) {
  switch (w) {
    case SeekWhence::begin:
      return SEEK_SET;
    case SeekWhence::current:
      return SEEK_CUR;
    case SeekWhence::end:
      return SEEK_END;
  }
  throw std::invalid_argument("invalid seek origin");
}
int advice_value(Advice a) {
  switch (a) {
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
void validate_config(const Config& c) {
  if (c.access == Access::read_only &&
      (c.create_if_missing || c.truncate_existing)) {
    throw std::invalid_argument(
        "creating or truncating a file requires read-write access");
  }
}
}  // namespace

class MmapFile::Impl {
 public:
  Impl(const std::filesystem::path& path, Config config) : config_(config) {
    validate_config(config_);
    if (config_.offset < 0) {
      throw std::invalid_argument("mmap offset cannot be negative");
    }
    page_size_ = ::sysconf(_SC_PAGESIZE);
    if (page_size_ <= 0 || config_.offset % page_size_ != 0) {
      throw std::invalid_argument("mmap offset must be page aligned");
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
  ~Impl() noexcept {
    unmap_noexcept();
    if (fd_ != -1) {
      (void)::close(fd_);
    }
  }

  void close() {
    if (fd_ == -1) {
      return;
    }
    unmap_checked();
    (void)::close(std::exchange(fd_, -1));
  }
  [[nodiscard]] bool is_open() const noexcept {
    return fd_ != -1;
  }
  [[nodiscard]] std::size_t size() const noexcept {
    return length_;
  }
  [[nodiscard]] file_offset file_size() const {
    require_open();
    return checked_file_size();
  }
  [[nodiscard]] file_offset offset() const noexcept {
    return config_.offset;
  }
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return {static_cast<const std::byte*>(address_), length_};
  }
  [[nodiscard]] std::span<std::byte> mutable_bytes() {
    require_writable();
    return {static_cast<std::byte*>(address_), length_};
  }
  [[nodiscard]] std::span<const std::byte> view(std::size_t p,
                                                std::size_t n) const {
    check_range(p, n);
    return bytes().subspan(p, n);
  }
  [[nodiscard]] std::span<std::byte> mutable_view(std::size_t p,
                                                  std::size_t n) {
    require_writable();
    check_range(p, n);
    return mutable_bytes().subspan(p, n);
  }
  [[nodiscard]] std::size_t read(std::span<std::byte> d, std::size_t p) const {
    check_range(p, 0);
    const std::size_t n = std::min(d.size(), length_ - p);
    if (n != 0) {
      std::memmove(d.data(), bytes().data() + p, n);
    }
    return n;
  }
  void write(std::span<const std::byte> s, std::size_t p) {
    require_writable();
    check_range(p, s.size());
    if (!s.empty()) {
      std::memmove(mutable_bytes().data() + p, s.data(), s.size());
    }
  }
  [[nodiscard]] file_offset seek(file_offset d, SeekWhence w) const {
    require_open();
    const off_t result = ::lseek(fd_, static_cast<off_t>(d), seek_whence(w));
    if (result == static_cast<off_t>(-1)) {
      throw_errno("lseek");
    }
    return static_cast<file_offset>(result);
  }
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
      // mapped before ftruncate(). This preserves the old mapping and file if
      // allocating the replacement mapping or truncating the file fails.
      void* replacement = map_new(new_length);
      try {
        check_result(::ftruncate(fd_, static_cast<off_t>(new_size)),
                     "ftruncate");
      } catch (...) {
        if (replacement != nullptr) {
          (void)::munmap(replacement, new_length);
        }
        throw;
      }
      replace_mapping_with_prepared(replacement, new_length);
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
  void remap() {
    require_open();
    const file_offset current_size = checked_file_size();
    const std::size_t new_length = length_for_remap(current_size);
    if (new_length == length_) {
      return;
    }
    replace_mapping(new_length);
  }
  void sync(std::size_t p, std::size_t n, bool invalidate) {
    require_open();
    if (config_.sharing != Sharing::shared) {
      throw std::logic_error("sync requires a shared mapping");
    }
    check_range(p, n);
    if (n == 0) {
      return;
    }
    if (p % static_cast<std::size_t>(page_size_) != 0) {
      throw std::invalid_argument("sync range position must be page aligned");
    }
    if (::msync(static_cast<std::byte*>(address_) + p, n,
                MS_SYNC | (invalidate ? MS_INVALIDATE : 0)) == -1) {
      throw_errno("msync");
    }
  }
  void advise(Advice a) {
    require_open();
    if (a == Advice::dont_need && config_.access == Access::read_write &&
        config_.sharing == Sharing::private_copy) {
      throw std::logic_error(
          "dont_need may discard private mapping modifications");
    }
    if (length_ != 0 && ::madvise(address_, length_, advice_value(a)) == -1) {
      throw_errno("madvise");
    }
  }

 private:
  void require_open() const {
    if (fd_ == -1) {
      throw std::logic_error("mmap file is closed");
    }
  }
  void require_writable() const {
    require_open();
    if (config_.access != Access::read_write) {
      throw std::logic_error("mmap is not writable");
    }
  }
  void require_shared_growth() const {
    if (config_.sharing != Sharing::shared) {
      throw std::logic_error(
          "resizing a private mapping is unsupported because it would discard "
          "copy-on-write data");
    }
  }
  void require_variable_mapping_matches_file() const {
    if (config_.length == 0 && checked_file_size() != mapping_end(length_)) {
      throw std::logic_error(
          "backing file size changed; call remap() before modifying it");
    }
  }
  void check_range(std::size_t p, std::size_t n) const {
    require_open();
    if (p > length_ || n > length_ - p) {
      throw std::out_of_range("mmap range is outside the mapped region");
    }
  }
  [[nodiscard]] file_offset checked_file_size() const {
    struct stat st{};
    check_result(::fstat(fd_, &st), "fstat");
    if (!S_ISREG(st.st_mode) || st.st_size < 0) {
      throw std::invalid_argument("mmaplib supports regular files only");
    }
    return static_cast<file_offset>(st.st_size);
  }
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
  [[nodiscard]] std::size_t initial_length_for_file(file_offset size) const {
    if (size < config_.offset) {
      if (config_.access == Access::read_write && config_.length != 0) {
        return config_.length;
      }
      throw std::invalid_argument("mmap offset is beyond the file");
    }
    return length_for_file(size);
  }
  [[nodiscard]] std::size_t length_for_remap(file_offset size) const {
    const std::size_t available = available_length(size);
    if (config_.length != 0 && config_.length > available) {
      throw std::invalid_argument("mapping extends beyond the backing file");
    }
    return config_.length == 0 ? available : config_.length;
  }
  [[nodiscard]] off_t mapping_end(std::size_t length) const {
    const auto offset = static_cast<std::uintmax_t>(config_.offset);
    if (length > std::numeric_limits<std::uintmax_t>::max() - offset ||
        offset + length >
            static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max())) {
      throw std::length_error("mmap end offset overflows off_t");
    }
    return static_cast<off_t>(offset + length);
  }
  void validate_resize_target(file_offset n) const {
    if (n < config_.offset) {
      throw std::invalid_argument(
          "file size cannot be smaller than mapping offset");
    }
    if (static_cast<std::uintmax_t>(n) >
        static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max())) {
      throw std::length_error("file size exceeds off_t range");
    }
    if (config_.length != 0 && available_length(n) < config_.length) {
      throw std::invalid_argument(
          "file size cannot be smaller than the fixed mapping");
    }
  }
  [[nodiscard]] void* map_new(std::size_t n) const {
    if (n == 0) {
      return nullptr;
    }
    int flags = mapping_flags(config_);
    if (config_.prefault) {
      flags |= MAP_POPULATE;
    }
    void* result =
        ::mmap(nullptr, n, protection(config_), flags, fd_, config_.offset);
    if (result == MAP_FAILED) {
      throw_errno("mmap");
    }
    return result;
  }
  void install(void* p, std::size_t n) noexcept {
    address_ = p;
    length_ = n;
  }
  void unmap_checked() {
    if (address_ != nullptr && ::munmap(address_, length_) == -1) {
      throw_errno("munmap");
    }
    address_ = nullptr;
    length_ = 0;
  }
  void unmap_noexcept() noexcept {
    if (address_ != nullptr) {
      (void)::munmap(address_, length_);
      address_ = nullptr;
      length_ = 0;
    }
  }
  void replace_mapping(std::size_t n) {
    void* replacement = map_new(n);
    replace_mapping_with_prepared(replacement, n);
  }
  void replace_mapping_with_prepared(void* replacement, std::size_t n) {
    try {
      unmap_checked();
    } catch (...) {
      if (replacement != nullptr) {
        (void)::munmap(replacement, n);
      }
      throw;
    }
    install(replacement, n);
  }
  Config config_;
  int fd_{-1};
  void* address_{nullptr};
  std::size_t length_{0};
  long page_size_{0};
};

MmapFile::MmapFile(const std::filesystem::path& path, Config config)
    : impl_(std::make_unique<Impl>(path, config)) {
}
MmapFile::~MmapFile() noexcept = default;
MmapFile::MmapFile(MmapFile&&) noexcept = default;
MmapFile& MmapFile::operator=(MmapFile&&) noexcept = default;
void MmapFile::close() {
  if (impl_ != nullptr) {
    impl_->close();
  }
}
bool MmapFile::is_open() const noexcept {
  return impl_ != nullptr && impl_->is_open();
}
std::size_t MmapFile::size() const noexcept {
  return impl_ == nullptr ? 0 : impl_->size();
}
file_offset MmapFile::file_size() const {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  return impl_->file_size();
}
file_offset MmapFile::offset() const noexcept {
  return impl_ == nullptr ? 0 : impl_->offset();
}
std::size_t MmapFile::mapping_length() const noexcept {
  return size();
}
std::span<const std::byte> MmapFile::bytes() const noexcept {
  return impl_ == nullptr ? std::span<const std::byte>{}
                          : static_cast<const Impl&>(*impl_).bytes();
}
std::span<std::byte> MmapFile::mutable_bytes() {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  return impl_->mutable_bytes();
}
std::span<const std::byte> MmapFile::view(std::size_t p, std::size_t n) const {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  return static_cast<const Impl&>(*impl_).view(p, n);
}
std::span<std::byte> MmapFile::mutable_view(std::size_t p, std::size_t n) {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  return impl_->mutable_view(p, n);
}
std::size_t MmapFile::read(std::span<std::byte> d, std::size_t p) const {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  return static_cast<const Impl&>(*impl_).read(d, p);
}
void MmapFile::write(std::span<const std::byte> s, std::size_t p) {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  impl_->write(s, p);
}
void MmapFile::write(std::string_view s, std::size_t p) {
  write(std::as_bytes(std::span{s.data(), s.size()}), p);
}
void MmapFile::insert(std::span<const std::byte> s, std::size_t p) {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  impl_->insert(s, p);
}
void MmapFile::insert(std::string_view s, std::size_t p) {
  insert(std::as_bytes(std::span{s.data(), s.size()}), p);
}
void MmapFile::append(std::span<const std::byte> s) {
  insert(s, mapping_length());
}
void MmapFile::append(std::string_view s) {
  insert(s, mapping_length());
}
file_offset MmapFile::seek(file_offset d, SeekWhence w) const {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  return impl_->seek(d, w);
}
void MmapFile::resize(file_offset n) {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  impl_->resize(n);
}
void MmapFile::remap() {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  impl_->remap();
}
void MmapFile::sync(bool invalidate) {
  sync_range(0, mapping_length(), invalidate);
}
void MmapFile::sync_range(std::size_t p, std::size_t n, bool invalidate) {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  impl_->sync(p, n, invalidate);
}
void MmapFile::advise(Advice a) {
  if (impl_ == nullptr) {
    throw std::logic_error("mmap file is closed");
  }
  impl_->advise(a);
}
}  // namespace mmaplib
