#include "mmap.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <unistd.h>

namespace {

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Exception, typename Callable>
void expect_throw(Callable&& callable, std::string_view message) {
    try {
        callable();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

class TemporaryPath {
public:
    explicit TemporaryPath(std::string_view name) {
        static unsigned long sequence = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("linuxfs-mmap-" + std::to_string(static_cast<long long>(::getpid())) +
                 "-" + std::string(name) + "-" + std::to_string(sequence++));
    }

    ~TemporaryPath() { std::error_code ignored; std::filesystem::remove(path_, ignored); }

    TemporaryPath(const TemporaryPath&) = delete;
    TemporaryPath& operator=(const TemporaryPath&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary);
    check(output.good(), "failed to create test file");
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    check(output.good(), "failed to write test file");
}

void mapping_reads_and_writes_without_a_class_buffer() {
    TemporaryPath temporary("read-write");
    const auto& path = temporary.path();
    write_file(path, "hello world this is AI era.");

    mmaplib::Config config;
    config.access = mmaplib::Access::read_write;
    config.sharing = mmaplib::Sharing::shared;
    
    mmaplib::MmapFile file(path, config);
    check(file.mapping_length() == 27, "unexpected initial mapping length");
    check(static_cast<char>(file.view(0, 5)[0]) == 'h', "initial mapping contents differ");
    file.view(0, 5)[0] = std::byte{'H'};
    //file.view(0, 5)[5] = "\n";
    file.insert(" inserting this text.", 6);
    file.append("\n");
    file.sync();
    file.close();

    std::ifstream input(path, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(input)), {});
    std::string expected = "hello world this is AI era.";
    expected[0] = 'H';
    expected.insert(6, " inserting this text.");
    expected += "\n";
    check(contents == expected, "shared mapping contents differ");
}

void invalid_ranges_are_rejected() {
    TemporaryPath temporary("range");
    const auto& path = temporary.path();
    write_file(path, "This is AI era.\n");
    
    mmaplib::Config config;
    config.access = mmaplib::Access::read_write;
    config.sharing = mmaplib::Sharing::shared;
    mmaplib::MmapFile file(path, config);
    bool rejected = false;
    try {
        (void)file.view(3, 22);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    check(rejected, "invalid range was accepted");
}

void private_mappings_reject_sync() {
    TemporaryPath temporary("private-sync");
    const auto& path = temporary.path();
    write_file(path, "private mapping");

    mmaplib::MmapFile file(path);
    bool rejected = false;
    try {
        file.sync();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    check(rejected, "private mapping accepted sync");
}

void moved_from_operations_throw() {
    TemporaryPath temporary("moved-from");
    write_file(temporary.path(), "moved-from");
    mmaplib::Config config;
    config.access = mmaplib::Access::read_write;
    mmaplib::MmapFile source(temporary.path(), config);
    mmaplib::MmapFile target(std::move(source));
    check(!source.is_open(), "moved-from object remains open");

    expect_throw<std::logic_error>([&] { (void)source.file_size(); }, "file_size accepted moved-from object");
    expect_throw<std::logic_error>([&] { (void)source.view(0, 0); }, "view accepted moved-from object");
    std::byte destination{};
    expect_throw<std::logic_error>([&] { (void)source.read(std::span{&destination, 1}); }, "read accepted moved-from object");
    expect_throw<std::logic_error>([&] { source.write(std::span<const std::byte>{}); }, "write accepted moved-from object");
    expect_throw<std::logic_error>([&] { source.insert(std::span<const std::byte>{}); }, "insert accepted moved-from object");
    expect_throw<std::logic_error>([&] { (void)source.seek(0); }, "seek accepted moved-from object");
    expect_throw<std::logic_error>([&] { source.resize(1); }, "resize accepted moved-from object");
    expect_throw<std::logic_error>([&] { source.sync(); }, "sync accepted moved-from object");
    expect_throw<std::logic_error>([&] { source.advise(mmaplib::Advice::normal); }, "advise accepted moved-from object");
    check(target.is_open(), "move lost the active mapping");
}

void empty_mapping_operations_are_safe() {
    TemporaryPath temporary("empty");
    write_file(temporary.path(), "");
    mmaplib::Config config;
    config.access = mmaplib::Access::read_write;
    config.sharing = mmaplib::Sharing::shared;
    mmaplib::MmapFile file(temporary.path(), config);
    std::byte value{};
    check(file.read(std::span{&value, 1}) == 0, "empty read returned data");
    file.write(std::span<const std::byte>{});
    check(file.mapping_length() == 0, "empty mapping has nonzero length");
}

void invalid_configurations_are_rejected() {
    TemporaryPath temporary("config");
    write_file(temporary.path(), "data");
    mmaplib::Config config;
    config.create_if_missing = true;
    expect_throw<std::invalid_argument>([&] { mmaplib::MmapFile file(temporary.path(), config); },
                                        "read-only create configuration was accepted");
    config = {};
    config.truncate_existing = true;
    expect_throw<std::invalid_argument>([&] { mmaplib::MmapFile file(temporary.path(), config); },
                                        "read-only truncate configuration was accepted");
}

void access_and_length_restrictions_are_enforced() {
    TemporaryPath temporary("restrictions");
    write_file(temporary.path(), "fixed data");
    mmaplib::MmapFile read_only(temporary.path());
    std::byte value{};
    expect_throw<std::logic_error>([&] { read_only.write(std::span{&value, 1}); },
                                   "read-only write was accepted");
    expect_throw<std::logic_error>([&] { read_only.resize(1); }, "read-only resize was accepted");
    expect_throw<std::logic_error>([&] { read_only.insert("x"); }, "read-only insert was accepted");

    mmaplib::Config fixed_config;
    fixed_config.access = mmaplib::Access::read_write;
    fixed_config.length = 4;
    mmaplib::MmapFile fixed(temporary.path(), fixed_config);
    expect_throw<std::logic_error>([&] { fixed.insert("x"); }, "fixed mapping insert was accepted");
    expect_throw<std::invalid_argument>([&] { fixed.resize(3); }, "fixed mapping shrink was accepted");
}

void aligned_offsets_and_prefault_remapping_work() {
    TemporaryPath temporary("offset");
    const long page_size = ::sysconf(_SC_PAGESIZE);
    check(page_size > 0, "invalid system page size");
    write_file(temporary.path(), std::string(static_cast<std::size_t>(page_size), 'x'));
    mmaplib::Config config;
    config.access = mmaplib::Access::read_write;
    config.sharing = mmaplib::Sharing::shared;
    config.offset = page_size;
    config.length = 1;
    config.prefault = true;
    mmaplib::MmapFile file(temporary.path(), config);
    check(file.mapping_length() == 1, "nonzero offset mapping has wrong length");
    file.resize(static_cast<mmaplib::file_offset>(page_size) + 2);
    check(file.mapping_length() == 1, "fixed mapping changed length after resize");
}

void private_changes_do_not_persist() {
    TemporaryPath temporary("private");
    write_file(temporary.path(), "original");
    mmaplib::Config config;
    config.access = mmaplib::Access::read_write;
    mmaplib::MmapFile file(temporary.path(), config);
    file.write("changed", 0);
    file.close();
    std::ifstream input(temporary.path(), std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(input)), {});
    check(contents == "original", "private mapping change reached the file");
}

void move_assignment_releases_previous_mapping() {
    TemporaryPath first_path("move-first");
    TemporaryPath second_path("move-second");
    write_file(first_path.path(), "first");
    write_file(second_path.path(), "second");
    mmaplib::MmapFile first(first_path.path());
    mmaplib::MmapFile second(second_path.path());
    second = std::move(first);
    check(!first.is_open(), "move-assigned source remains open");
    check(second.is_open() && second.mapping_length() == 5, "move assignment lost destination mapping");
    expect_throw<std::logic_error>([&] { (void)first.file_size(); }, "move-assigned source was usable");
}

} // namespace

int main() {
    mapping_reads_and_writes_without_a_class_buffer();
    invalid_ranges_are_rejected();
    private_mappings_reject_sync();
    moved_from_operations_throw();
    empty_mapping_operations_are_safe();
    invalid_configurations_are_rejected();
    access_and_length_restrictions_are_enforced();
    aligned_offsets_and_prefault_remapping_work();
    private_changes_do_not_persist();
    move_assignment_releases_previous_mapping();
    std::cout << "mmap tests passed\n";
}
