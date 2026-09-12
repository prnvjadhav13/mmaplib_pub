#include "mmap.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mmaplib {
namespace {

[[noreturn]] void throw_errno(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void check_result(int result, const char* operation) {
    if (result == -1) throw_errno(operation);
}

int open_flags(const Config& config) {
    int flags = config.access == Access::read_write ? O_RDWR : O_RDONLY;
    flags |= O_CLOEXEC;
    if (config.create_if_missing) flags |= O_CREAT;
    if (config.truncate_existing) flags |= O_TRUNC;
    return flags;
}

int protection(const Config& config) {
    return config.access == Access::read_write ? PROT_READ | PROT_WRITE : PROT_READ;
}

int mapping_flags(const Config& config) {
    return config.sharing == Sharing::shared ? MAP_SHARED : MAP_PRIVATE;
}

int seek_whence(SeekWhence whence) {
    switch (whence) {
    case SeekWhence::begin: return SEEK_SET;
    case SeekWhence::current: return SEEK_CUR;
    case SeekWhence::end: return SEEK_END;
    }
    throw std::invalid_argument("invalid seek origin");
}

int advice_value(Advice advice) {
    switch (advice) {
    case Advice::normal: return MADV_NORMAL;
    case Advice::sequential: return MADV_SEQUENTIAL;
    case Advice::random: return MADV_RANDOM;
    case Advice::will_need: return MADV_WILLNEED;
    case Advice::dont_need: return MADV_DONTNEED;
    }
    throw std::invalid_argument("invalid mapping advice");
}

void validate_config(const Config& config) {
    if (config.access == Access::read_only &&
        (config.create_if_missing || config.truncate_existing)) {
        throw std::invalid_argument("creating or truncating a file requires read-write access");
    }
}

} // namespace

class MmapFile::Impl {
public:
    Impl(const std::filesystem::path& path, Config config) : config_(config) {
        validate_config(config_);
        if (config_.offset < 0) throw std::invalid_argument("mmap offset cannot be negative");
        const long page_size = ::sysconf(_SC_PAGESIZE);
        if (page_size <= 0 || config_.offset % page_size != 0) {
            throw std::invalid_argument("mmap offset must be page aligned");
        }
        fd_ = ::open(path.c_str(), open_flags(config_), 0644);
        if (fd_ == -1) throw_errno("open");
        try {
            struct stat status {};
            check_result(::fstat(fd_, &status), "fstat");
            const std::size_t requested = length_for_file(status.st_size);
            const auto available = static_cast<std::uintmax_t>(status.st_size - config_.offset);
            if (static_cast<std::uintmax_t>(requested) > available) {
                if (static_cast<std::uintmax_t>(requested) >
                    static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max()) -
                    static_cast<std::uintmax_t>(config_.offset)) {
                    throw std::length_error("mmap end offset overflows off_t");
                }
                check_result(::ftruncate(fd_, config_.offset + static_cast<off_t>(requested)), "ftruncate");
            }
            map(requested);
        } catch (...) {
            ::close(std::exchange(fd_, -1));
            throw;
        }
    }

    ~Impl() noexcept {
        unmap();
        if (fd_ != -1) ::close(fd_);
    }

    void close() {
        if (fd_ == -1) return;
        if (address_ != nullptr && ::munmap(address_, length_) == -1) {
            const int saved_errno = errno;
            address_ = nullptr;
            length_ = 0;
            ::close(std::exchange(fd_, -1));
            throw std::system_error(saved_errno, std::generic_category(), "munmap");
        }
        address_ = nullptr;
        length_ = 0;
        (void)::close(std::exchange(fd_, -1));
    }

    [[nodiscard]] bool is_open() const noexcept { return fd_ != -1; }
    [[nodiscard]] std::size_t size() const noexcept { return length_; }
    [[nodiscard]] file_offset file_size() const {
        require_open();
        struct stat status {};
        check_result(::fstat(fd_, &status), "fstat");
        return status.st_size;
    }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {static_cast<const std::byte*>(address_), length_};
    }
    [[nodiscard]] std::span<std::byte> bytes() {
        require_writable();
        return {static_cast<std::byte*>(address_), length_};
    }
    [[nodiscard]] file_offset seek(file_offset distance, SeekWhence whence) const {
        require_open();
        const off_t result = ::lseek(fd_, distance, seek_whence(whence));
        if (result == static_cast<off_t>(-1)) throw_errno("lseek");
        return result;
    }
    void resize(file_offset new_size) {
        require_writable();
        if (new_size < 0) throw std::invalid_argument("file size cannot be negative");
        if (config_.length != 0 &&
            (new_size < config_.offset || static_cast<std::uintmax_t>(new_size - config_.offset) < config_.length)) {
            throw std::invalid_argument("file size cannot be smaller than the fixed mapping");
        }
        const file_offset old_size = file_size();
        const std::size_t old_length = length_;
        const bool shrinking = new_size < old_size;
        bool truncated = false;
        if (shrinking) unmap();
        try {
            check_result(::ftruncate(fd_, new_size), "ftruncate");
            truncated = true;
            remap(length_for_file(new_size));
        } catch (...) {
            if (shrinking && !truncated && address_ == nullptr) {
                try {
                    map(old_length);
                } catch (...) {
                }
            }
            throw;
        }
    }
    void sync(bool invalidate) {
        require_open();
        if (config_.sharing != Sharing::shared) {
            throw std::logic_error("sync requires a shared mapping");
        }
        if (length_ != 0 && ::msync(address_, length_, MS_SYNC | (invalidate ? MS_INVALIDATE : 0)) == -1) {
            throw_errno("msync");
        }
    }
    void advise(Advice advice) {
        require_open();
        if (length_ != 0 && ::madvise(address_, length_, advice_value(advice)) == -1) {
            throw_errno("madvise");
        }
    }
    [[nodiscard]] file_offset offset() const noexcept { return config_.offset; }

    [[nodiscard]] std::span<const std::byte> view(std::size_t position, std::size_t count) const {
        check_range(position, count);
        return bytes().subspan(position, count);
    }
    [[nodiscard]] std::span<std::byte> view(std::size_t position, std::size_t count) {
        check_range(position, count);
        return bytes().subspan(position, count);
    }
    [[nodiscard]] std::size_t read(std::span<std::byte> destination, std::size_t position) const {
        check_range(position, 0);
        const std::size_t count = std::min(destination.size(), length_ - position);
        if (count == 0) return 0;
        std::memcpy(destination.data(), static_cast<const std::byte*>(address_) + position, count);
        return count;
    }
    void write(std::span<const std::byte> source, std::size_t position) {
        require_writable();
        check_range(position, source.size());
        if (source.empty()) return;
        std::memcpy(static_cast<std::byte*>(address_) + position, source.data(), source.size());
    }
    void insert(std::span<const std::byte> source, std::size_t position) {
        require_writable();
        check_range(position, 0);
        if (source.empty()) return;
        if (config_.length != 0) {
            throw std::logic_error("fixed-length mappings cannot be grown by insert");
        }
        if (source.size() > std::numeric_limits<std::size_t>::max() - length_) {
            throw std::length_error("insert size overflows mapping length");
        }

        // Preserve the input before resize() invalidates any span that aliases this mapping.
        const std::vector<std::byte> inserted(source.begin(), source.end());
        const std::size_t old_length = length_;
        const file_offset current_file_size = file_size();
        if (inserted.size() > static_cast<std::size_t>(std::numeric_limits<file_offset>::max())) {
            throw std::length_error("insert size exceeds file offset range");
        }
        if (current_file_size > std::numeric_limits<file_offset>::max() -
                                    static_cast<file_offset>(inserted.size())) {
            throw std::length_error("insert size overflows file size");
        }

        resize(current_file_size + static_cast<file_offset>(inserted.size()));
        auto mapped = bytes();
        std::move_backward(mapped.begin() + static_cast<std::ptrdiff_t>(position),
                           mapped.begin() + static_cast<std::ptrdiff_t>(old_length),
                           mapped.end());
        std::memcpy(mapped.data() + position, inserted.data(), inserted.size());
    }

private:
    void require_open() const {
        if (fd_ == -1) throw std::logic_error("mmap file is closed");
    }
    void require_writable() const {
        require_open();
        if (config_.access != Access::read_write) throw std::logic_error("mmap is not writable");
    }
    void check_range(std::size_t position, std::size_t count) const {
        require_open();
        if (position > length_ || count > length_ - position) {
            throw std::out_of_range("mmap range is outside the mapped region");
        }
    }
    [[nodiscard]] std::size_t length_for_file(file_offset current_size) const {
        if (current_size < config_.offset) throw std::invalid_argument("mmap offset is beyond the file");
        const auto available = static_cast<std::uintmax_t>(current_size - config_.offset);
        const auto requested = config_.length == 0 ? available : static_cast<std::uintmax_t>(config_.length);
        if (requested > std::numeric_limits<std::size_t>::max()) throw std::length_error("mmap length is too large");
        if (config_.length != 0 && requested > available && config_.access == Access::read_only) {
            throw std::invalid_argument("read-only mapping extends beyond the file");
        }
        return static_cast<std::size_t>(requested);
    }
    void map(std::size_t length) {
        if (length == 0) return;
        int flags = mapping_flags(config_);
        if (config_.prefault) flags |= MAP_POPULATE;
        address_ = ::mmap(nullptr, length, protection(config_), flags, fd_, config_.offset);
        if (address_ == MAP_FAILED) {
            address_ = nullptr;
            throw_errno("mmap");
        }
        length_ = length;
    }
    void remap(std::size_t length) {
        void* new_address = nullptr;
        if (length != 0) {
            int flags = mapping_flags(config_);
            if (config_.prefault) flags |= MAP_POPULATE;
            new_address = ::mmap(nullptr, length, protection(config_), flags, fd_, config_.offset);
            if (new_address == MAP_FAILED) throw_errno("mmap");
        }
        unmap();
        address_ = new_address;
        length_ = length;
    }
    void unmap() noexcept {
        if (address_ != nullptr) {
            ::munmap(address_, length_);
            address_ = nullptr;
            length_ = 0;
        }
    }

    Config config_;
    int fd_{-1};
    void* address_{nullptr};
    std::size_t length_{0};
};

MmapFile::MmapFile(const std::filesystem::path& path, Config config)
    : impl_(std::make_unique<Impl>(path, config)) {}
MmapFile::~MmapFile() noexcept = default;
MmapFile::MmapFile(MmapFile&&) noexcept = default;
MmapFile& MmapFile::operator=(MmapFile&&) noexcept = default;
void MmapFile::close() {
    if (impl_ != nullptr) impl_->close();
}
bool MmapFile::is_open() const noexcept { return impl_ != nullptr && impl_->is_open(); }
std::size_t MmapFile::size() const noexcept { return impl_ == nullptr ? 0 : impl_->size(); }
file_offset MmapFile::file_size() const {
    if (impl_ == nullptr) throw std::logic_error("mmap file is closed");
    return impl_->file_size();
}
file_offset MmapFile::offset() const noexcept { return impl_ == nullptr ? 0 : impl_->offset(); }
std::size_t MmapFile::mapping_length() const noexcept { return size(); }
std::span<const std::byte> MmapFile::bytes() const noexcept {
    return impl_ == nullptr ? std::span<const std::byte>{} : impl_->bytes();
}
std::span<std::byte> MmapFile::bytes() {
    if (impl_ == nullptr) throw std::logic_error("mmap file is closed");
    return impl_->bytes();
}
std::span<const std::byte> MmapFile::view(std::size_t p, std::size_t n) const {
    if (impl_ == nullptr) throw std::logic_error("mmap file is closed");
    return impl_->view(p, n);
}
std::span<std::byte> MmapFile::view(std::size_t p, std::size_t n) {
    if (impl_ == nullptr) throw std::logic_error("mmap file is closed");
    return impl_->view(p, n);
}
std::size_t MmapFile::read(std::span<std::byte> d, std::size_t p) const {
    if (impl_ == nullptr) throw std::logic_error("mmap file is closed");
    return impl_->read(d, p);
}
void MmapFile::write(std::span<const std::byte> s, std::size_t p) {
    if (impl_ == nullptr) throw std::logic_error("mmap file is closed");
    impl_->write(s, p);
}
void MmapFile::write(std::string_view s, std::size_t p) {
    write(std::as_bytes(std::span{s.data(), s.size()}), p);
}
void MmapFile::insert(std::span<const std::byte> s, std::size_t p) {
    if (impl_ == nullptr) throw std::logic_error("mmap file is closed");
    impl_->insert(s, p);
}
void MmapFile::insert(std::string_view s, std::size_t p) {
    insert(std::as_bytes(std::span{s.data(), s.size()}), p);
}
void MmapFile::append(std::span<const std::byte> s) { insert(s, mapping_length()); }
void MmapFile::append(std::string_view s) { insert(s, mapping_length()); }
file_offset MmapFile::seek(file_offset d, SeekWhence w) const {
    if (impl_ == nullptr) throw std::logic_error("mmap file is closed");
    return impl_->seek(d, w);
}
void MmapFile::resize(file_offset s) {
    if (impl_ == nullptr) throw std::logic_error("mmap file is closed");
    impl_->resize(s);
}
void MmapFile::sync(bool i) {
    if (impl_ == nullptr) throw std::logic_error("mmap file is closed");
    impl_->sync(i);
}
void MmapFile::advise(Advice a) {
    if (impl_ == nullptr) throw std::logic_error("mmap file is closed");
    impl_->advise(a);
}

} // namespace mmaplib
