#include "mmap.hpp"

#include <cerrno>
#include <system_error>
#include <utility>

namespace mmaplib {
namespace {
[[noreturn]] void unsupported() {
    throw std::system_error(ENOTSUP, std::generic_category(), "MmapFile is not supported on this platform");
}
}

class MmapFile::Impl {};

MmapFile::MmapFile(const std::filesystem::path&, Config) { unsupported(); }
MmapFile::~MmapFile() noexcept = default;
MmapFile::MmapFile(MmapFile&& other) noexcept : impl_(std::exchange(other.impl_, nullptr)) {}
MmapFile& MmapFile::operator=(MmapFile&& other) noexcept {
    impl_ = std::exchange(other.impl_, nullptr);
    return *this;
}
void MmapFile::close() { unsupported(); }
bool MmapFile::is_open() const noexcept { return false; }
std::size_t MmapFile::size() const noexcept { return 0; }
file_offset MmapFile::file_size() const { unsupported(); }
file_offset MmapFile::offset() const noexcept { return 0; }
std::size_t MmapFile::mapping_length() const noexcept { return 0; }
std::span<const std::byte> MmapFile::bytes() const noexcept { return {}; }
std::span<std::byte> MmapFile::bytes() { unsupported(); }
std::span<const std::byte> MmapFile::view(std::size_t, std::size_t) const { unsupported(); }
std::span<std::byte> MmapFile::view(std::size_t, std::size_t) { unsupported(); }
std::size_t MmapFile::read(std::span<std::byte>, std::size_t) const { unsupported(); }
void MmapFile::write(std::span<const std::byte>, std::size_t) { unsupported(); }
void MmapFile::write(std::string_view, std::size_t) { unsupported(); }
void MmapFile::insert(std::span<const std::byte>, std::size_t) { unsupported(); }
void MmapFile::insert(std::string_view, std::size_t) { unsupported(); }
void MmapFile::append(std::span<const std::byte>) { unsupported(); }
void MmapFile::append(std::string_view) { unsupported(); }
file_offset MmapFile::seek(file_offset, SeekWhence) const { unsupported(); }
void MmapFile::resize(file_offset) { unsupported(); }
void MmapFile::sync(bool) { unsupported(); }
void MmapFile::advise(Advice) { unsupported(); }

} // namespace mmaplib
