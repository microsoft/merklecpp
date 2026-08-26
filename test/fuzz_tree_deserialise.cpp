// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <merklecpp.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  std::vector<uint8_t> bytes;
  if (size != 0)
  {
    bytes.assign(data, data + size);
  }

  try
  {
    size_t position = 0;
    merkle::Tree tree;
    tree.deserialise(bytes, position);
  }
  // NOLINTNEXTLINE(bugprone-empty-catch) -- expected malformed input rejection
  catch (const std::runtime_error&)
  {}
  // NOLINTNEXTLINE(bugprone-empty-catch) -- expected malformed input rejection
  catch (const std::out_of_range&)
  {}

  return 0;
}
