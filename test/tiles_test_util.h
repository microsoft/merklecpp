// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <merklecpp_pal.h>
#include <string_view>

class TemporaryDirectory
{
private:
  std::filesystem::path path_;

public:
  explicit TemporaryDirectory(
    std::string_view prefix = "merklecpp_tiles")
  {
    static std::atomic<uint64_t> sequence = 0;
    const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
      std::format(
              "{}_{}_{}_{}",
              prefix,
              merkle::pal::process_id(),
              nonce,
              sequence++);
  }

  ~TemporaryDirectory()
  {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const
  {
    return path_;
  }
};
