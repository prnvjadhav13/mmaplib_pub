#include "mmap.hpp"

#include <unistd.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <cstring>
#include <iomanip>

namespace {

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
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
    path_ =
        std::filesystem::temp_directory_path() /
        ("linuxfs-mmap-" + std::to_string(static_cast<long long>(::getpid())) +
         "-" + std::string(name) + "-" + std::to_string(sequence++));
  }

  ~TemporaryPath() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  TemporaryPath(const TemporaryPath&) = delete;
  TemporaryPath& operator=(const TemporaryPath&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

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
  check(static_cast<char>(file.view(0, 5)[0]) == 'h',
        "initial mapping contents differ");
  file.mutable_view(0, 5)[0] = std::byte{'H'};
  // file.view(0, 5)[5] = "\n";
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

  expect_throw<std::logic_error>([&] { (void)source.file_size(); },
                                 "file_size accepted moved-from object");
  expect_throw<std::logic_error>([&] { (void)source.view(0, 0); },
                                 "view accepted moved-from object");
  std::byte destination{};
  expect_throw<std::logic_error>(
      [&] { (void)source.read(std::span{&destination, 1}); },
      "read accepted moved-from object");
  expect_throw<std::logic_error>(
      [&] { source.write(std::span<const std::byte>{}); },
      "write accepted moved-from object");
  expect_throw<std::logic_error>(
      [&] { source.insert(std::span<const std::byte>{}); },
      "insert accepted moved-from object");
  expect_throw<std::logic_error>([&] { (void)source.seek(0); },
                                 "seek accepted moved-from object");
  expect_throw<std::logic_error>([&] { source.resize(1); },
                                 "resize accepted moved-from object");
  expect_throw<std::logic_error>([&] { source.sync(); },
                                 "sync accepted moved-from object");
  expect_throw<std::logic_error>(
      [&] { source.advise(mmaplib::Advice::normal); },
      "advise accepted moved-from object");
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
  expect_throw<std::invalid_argument>(
      [&] { mmaplib::MmapFile file(temporary.path(), config); },
      "read-only create configuration was accepted");
  config = {};
  config.truncate_existing = true;
  expect_throw<std::invalid_argument>(
      [&] { mmaplib::MmapFile file(temporary.path(), config); },
      "read-only truncate configuration was accepted");
}

void access_and_length_restrictions_are_enforced() {
  TemporaryPath temporary("restrictions");
  write_file(temporary.path(), "fixed data");
  mmaplib::MmapFile read_only(temporary.path());
  std::byte value{};
  expect_throw<std::logic_error>([&] { read_only.write(std::span{&value, 1}); },
                                 "read-only write was accepted");
  expect_throw<std::logic_error>([&] { read_only.resize(1); },
                                 "read-only resize was accepted");
  expect_throw<std::logic_error>([&] { read_only.insert("x"); },
                                 "read-only insert was accepted");

  mmaplib::Config fixed_config;
  fixed_config.access = mmaplib::Access::read_write;
  fixed_config.length = 4;
  mmaplib::MmapFile fixed(temporary.path(), fixed_config);
  expect_throw<std::logic_error>([&] { fixed.insert("x"); },
                                 "fixed mapping insert was accepted");
  expect_throw<std::invalid_argument>([&] { fixed.resize(3); },
                                      "fixed mapping shrink was accepted");
}

void aligned_offsets_and_prefault_remapping_work() {
  TemporaryPath temporary("offset");
  const long page_size = ::sysconf(_SC_PAGESIZE);
  check(page_size > 0, "invalid system page size");
  write_file(temporary.path(),
             std::string(static_cast<std::size_t>(page_size), 'x'));
  mmaplib::Config config;
  config.access = mmaplib::Access::read_write;
  config.sharing = mmaplib::Sharing::shared;
  config.offset = page_size;
  config.length = 1;
  config.prefault = true;
  mmaplib::MmapFile file(temporary.path(), config);
  check(file.mapping_length() == 1, "nonzero offset mapping has wrong length");
  file.resize(static_cast<mmaplib::file_offset>(page_size) + 2);
  check(file.mapping_length() == 1,
        "fixed mapping changed length after resize");
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

void const_read_only_views_and_overlapping_copies_are_safe() {
  TemporaryPath temporary("const-overlap");
  write_file(temporary.path(), "abcdef");
  const mmaplib::MmapFile read_only(temporary.path());
  check(read_only.bytes().size() == 6,
        "const bytes failed for a read-only mapping");
  check(static_cast<char>(read_only.view(1, 1)[0]) == 'b',
        "const view failed for a read-only mapping");

  mmaplib::Config config;
  config.access = mmaplib::Access::read_write;
  config.sharing = mmaplib::Sharing::shared;
  mmaplib::MmapFile file(temporary.path(), config);
  file.write(file.view(0, 4), 1);
  check(std::string(reinterpret_cast<const char*>(file.bytes().data()),
                    file.bytes().size()) == "aabcdf",
        "overlapping write did not use move semantics");
  check(file.read(file.mutable_view(1, 4), 0) == 4,
        "overlapping read returned an incorrect length");
  check(std::string(reinterpret_cast<const char*>(file.bytes().data()),
                    file.bytes().size()) == "aaabcf",
        "overlapping read did not use move semantics");
}

void private_growth_and_dont_need_are_rejected_without_data_loss() {
  TemporaryPath temporary("private-protection");
  write_file(temporary.path(), "abcdef");
  mmaplib::Config config;
  config.access = mmaplib::Access::read_write;
  mmaplib::MmapFile file(temporary.path(), config);
  file.write("X", 0);
  expect_throw<std::logic_error>([&] { file.resize(6); },
                                 "private resize was accepted");
  expect_throw<std::logic_error>(
      [&] { file.advise(mmaplib::Advice::dont_need); },
      "private dont_need was accepted");
  check(static_cast<char>(file.bytes()[0]) == 'X',
        "private modification was lost");
}

void invalid_resize_never_truncates_file() {
  TemporaryPath temporary("resize-offset");
  const long page_size = ::sysconf(_SC_PAGESIZE);
  check(page_size > 0, "invalid system page size");
  write_file(temporary.path(),
             std::string(static_cast<std::size_t>(page_size) * 2, 'x'));
  mmaplib::Config config;
  config.access = mmaplib::Access::read_write;
  config.sharing = mmaplib::Sharing::shared;
  config.offset = page_size;
  mmaplib::MmapFile file(temporary.path(), config);
  expect_throw<std::invalid_argument>(
      [&] { file.resize(page_size - 1); },
      "resize below mapping offset was accepted");
  check(file.file_size() == page_size * 2,
        "invalid resize truncated the backing file");
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
  check(second.is_open() && second.mapping_length() == 5,
        "move assignment lost destination mapping");
  expect_throw<std::logic_error>([&] { (void)first.file_size(); },
                                 "move-assigned source was usable");
}

void print_hex_dump(std::span<const std::byte> data) {
  for (std::size_t index = 0; index < data.size(); ++index) {
    const auto value = std::to_integer<unsigned int>(data[index]);

    std::cout << std::hex << std::setw(2) << std::setfill('0') << value << ' ';

    // Start a new line every 16 bytes.
    if ((index + 1) % 16 == 0) {
      std::cout << '\n';
    }
  }

  if (data.size() % 16 != 0) {
    std::cout << '\n';
  }

  std::cout << std::dec;  // Restore normal decimal output.
}

void my_test() {
  const std::filesystem::path path =
      "/tmp/linuxfs-mmap-test.bin";

  try {
    mmaplib::Config config;
    config.access = mmaplib::Access::read_write;
    config.sharing = mmaplib::Sharing::shared;
    config.offset = 0;
    mmaplib::MmapFile server_map(path, config);
    const auto data = server_map.bytes();
    auto mutable_data = server_map.mutable_view(0, server_map.mapping_length());

    if (data.size() < 3) {
      throw std::runtime_error("mapping contains fewer than 3 bytes");
    }

    const char* text =
    reinterpret_cast<const char*>(data.data());
    
    std::cout << "this is a string = ";
    std::cout.write(text, static_cast<std::streamsize>(data.size()));
    std::cout << '\n';

    //mutable_data.repl
    std::string_view stext(reinterpret_cast<const char*>(mutable_data.data()), mutable_data.size());
    std::string_view from = "My era";
    std::string_view to   = "Coolra";

    auto pos = stext.find(from);
    if (pos != std::string_view::npos) {
        if (to.size() != from.size()) {
            throw std::runtime_error("in-place mmap replacement must be same length");
        }

        std::memcpy(mutable_data.data() + pos, to.data(), to.size());
    }

    //print_hex_dump(data.first(std::min<std::size_t>(64, data.size())));
  } catch (const std::system_error& error) {
    std::cerr << "mmap failed: " << error.what() << '\n';
  } catch (const std::exception& error) {
    std::cerr << "test failed: " << error.what() << '\n';
  }
}

}  // namespace

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
  const_read_only_views_and_overlapping_copies_are_safe();
  private_growth_and_dont_need_are_rejected_without_data_loss();
  invalid_resize_never_truncates_file();
  move_assignment_releases_previous_mapping();

  my_test();
  std::cout << "mmap tests passed\n";
}
