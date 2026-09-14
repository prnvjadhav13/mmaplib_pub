#include <cerrno>
#include <system_error>
#include <utility>

#include "mmap.hpp"

namespace mmaplib {
namespace {
/// Reports that this fallback backend has no native mapping implementation.
[[noreturn]] void unsupported() {
  throw std::system_error(ENOTSUP, std::generic_category(),
                          "MmapFile is not supported on this platform");
}
}  // namespace

class MmapFile::Impl {};

/// Rejects construction because path and config cannot be used on this host.
MmapFile::MmapFile(const std::filesystem::path&, Config) {
  unsupported();
}

/// Performs no cleanup because unsupported construction never owns resources.
MmapFile::~MmapFile() noexcept = default;

/// Transfers the empty implementation pointer from other.
MmapFile::MmapFile(MmapFile&& other) noexcept
    : impl_(std::exchange(other.impl_, nullptr)) {
}

/// Replaces this empty implementation pointer with the one from other.
MmapFile& MmapFile::operator=(MmapFile&& other) noexcept {
  impl_ = std::exchange(other.impl_, nullptr);
  return *this;
}

/// Reports that explicit close is unsupported by this backend.
void MmapFile::close() {
  unsupported();
}

/// Always reports closed because this backend cannot create mappings.
bool MmapFile::is_open() const noexcept {
  return false;
}

/// Always reports zero because this backend cannot map bytes.
std::size_t MmapFile::size() const noexcept {
  return 0;
}

/// Rejects querying a backing file size on the unsupported backend.
file_offset MmapFile::file_size() const {
  unsupported();
}

/// Always reports zero because no mapping offset exists.
file_offset MmapFile::offset() const noexcept {
  return 0;
}

/// Always reports zero because no mapping exists.
std::size_t MmapFile::mapping_length() const noexcept {
  return 0;
}

/// Returns an empty read-only span because no mapping exists.
std::span<const std::byte> MmapFile::bytes() const noexcept {
  return {};
}

/// Rejects writable access because mappings are unsupported.
std::span<std::byte> MmapFile::mutable_bytes() {
  unsupported();
}

/// Rejects a read-only subview; both position and count are unusable here.
std::span<const std::byte> MmapFile::view(std::size_t, std::size_t) const {
  unsupported();
}

/// Rejects a writable subview; both position and count are unusable here.
std::span<std::byte> MmapFile::mutable_view(std::size_t, std::size_t) {
  unsupported();
}

/// Rejects copying into destination from an unsupported mapping position.
std::size_t MmapFile::read(std::span<std::byte>, std::size_t) const {
  unsupported();
}

/// Rejects writing binary source bytes at a mapping-relative position.
void MmapFile::write(std::span<const std::byte>, std::size_t) {
  unsupported();
}

/// Rejects writing text source bytes at a mapping-relative position.
void MmapFile::write(std::string_view, std::size_t) {
  unsupported();
}

/// Rejects inserting binary source bytes at a mapping-relative position.
void MmapFile::insert(std::span<const std::byte>, std::size_t) {
  unsupported();
}

/// Rejects inserting text source bytes at a mapping-relative position.
void MmapFile::insert(std::string_view, std::size_t) {
  unsupported();
}

/// Rejects appending binary source bytes on the unsupported backend.
void MmapFile::append(std::span<const std::byte>) {
  unsupported();
}

/// Rejects appending text source bytes on the unsupported backend.
void MmapFile::append(std::string_view) {
  unsupported();
}

/// Rejects moving the descriptor cursor by a distance and seek origin.
file_offset MmapFile::seek(file_offset, SeekWhence) const {
  unsupported();
}

/// Rejects changing the backing file to an absolute new size.
void MmapFile::resize(file_offset) {
  unsupported();
}

/// Rejects rebuilding a mapping from the current backing-file extent.
void MmapFile::remap() {
  unsupported();
}

/// Rejects full synchronization and its optional invalidation request.
void MmapFile::sync(bool) {
  unsupported();
}

/// Rejects synchronizing a position/count range and invalidation request.
void MmapFile::sync_range(std::size_t, std::size_t, bool) {
  unsupported();
}

/// Rejects applying a mapping-access advice value.
void MmapFile::advise(Advice) {
  unsupported();
}

}  // namespace mmaplib
