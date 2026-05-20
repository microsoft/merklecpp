// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "util.h"

#include <cstring>
#include <iostream>
#include <list>
#include <merklecpp.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
  void require(bool condition, const std::string& message)
  {
    if (!condition)
    {
      throw std::runtime_error(message);
    }
  }

  template <typename F>
  void require_throws(F&& f, const std::string& message)
  {
    try
    {
      std::forward<F>(f)();
    }
    catch (const std::exception&)
    {
      return;
    }
    throw std::runtime_error(message);
  }

  merkle::Hash hash_with_byte(uint8_t byte)
  {
    merkle::Hash h;
    h.bytes[0] = byte;
    h.bytes[31] = byte;
    return h;
  }

  merkle::Path single_element_path(
    const merkle::Hash& leaf,
    size_t leaf_index,
    size_t max_index,
    const merkle::Hash& sibling,
    merkle::Path::Direction direction)
  {
    merkle::Path::Element e;
    e.hash = sibling;
    e.direction = direction;

    std::list<merkle::Path::Element> elements;
    elements.push_back(e);
    return {leaf, leaf_index, std::move(elements), max_index};
  }

  merkle::Tree make_tree(size_t num_leaves)
  {
    merkle::Tree tree;
    auto hashes = make_hashes(num_leaves);
    for (const auto& hash : hashes)
    {
      tree.insert(hash);
    }
    return tree;
  }

  void test_serialisation_helpers()
  {
    const uint32_t n = 0x01020304U;
    const uint32_t converted = merkle::convert_endianness(n);
    uint8_t bytes[sizeof(converted)] = {};
    std::memcpy(bytes, &converted, sizeof(converted));
    require(bytes[0] == 0x01, "convert_endianness byte 0 mismatch");
    require(bytes[1] == 0x02, "convert_endianness byte 1 mismatch");
    require(bytes[2] == 0x03, "convert_endianness byte 2 mismatch");
    require(bytes[3] == 0x04, "convert_endianness byte 3 mismatch");
    require(
      merkle::convert_endianness(converted) == n,
      "convert_endianness should round-trip");

    std::vector<uint8_t> buffer = {0xAA};
    merkle::serialise_uint64_t(0x0102030405060708ULL, buffer);
    require(buffer.size() == 9, "serialise_uint64_t should append 8 bytes");
    for (size_t i = 1; i < buffer.size(); i++)
    {
      require(
        buffer[i] == static_cast<uint8_t>(i),
        "serialise_uint64_t should write big-endian bytes");
    }

    size_t position = 1;
    const auto value = merkle::deserialise_uint64_t(buffer, position);
    require(value == 0x0102030405060708ULL, "deserialise_uint64_t mismatch");
    require(
      position == buffer.size(), "deserialise_uint64_t position mismatch");

    std::vector<uint8_t> short_buffer(7, 0);
    size_t short_position = 0;
    require_throws(
      [&] { merkle::deserialise_uint64_t(short_buffer, short_position); },
      "deserialise_uint64_t should reject short buffers");
  }

  void test_path_metadata_and_equality()
  {
    merkle::Tree tree;
    const auto hashes = make_hashes(4);
    for (const auto& hash : hashes)
    {
      tree.insert(hash);
    }

    const auto root = tree.root();
    const auto path = tree.path(2);
    require(path->leaf() == hashes[2], "path leaf mismatch");
    require(path->leaf_index() == 2, "path leaf_index mismatch");
    require(path->max_index() == 3, "path max_index mismatch");
    require(path->verify(root), "path should verify root");

    const std::string path_string = path->to_string(1);
    require(path_string.find("(L)") != std::string::npos, "path missing left");
    require(path_string.find("(R)") != std::string::npos, "path missing right");

    std::vector<uint8_t> serialised_path = {0xFF};
    path->serialise(serialised_path);

    size_t position = 1;
    const merkle::Path from_position(serialised_path, position);
    require(from_position == *path, "position constructor should round-trip");
    require(
      position == serialised_path.size(),
      "position constructor should advance position");

    const std::vector<uint8_t> exact_path(
      serialised_path.begin() + 1, serialised_path.end());
    const merkle::Path from_exact_buffer(exact_path);
    require(from_exact_buffer == *path, "buffer constructor should round-trip");

    const auto leaf = hash_with_byte(0x10);
    const auto sibling = hash_with_byte(0x20);
    const auto same = single_element_path(
      leaf, 4, 9, sibling, merkle::Path::Direction::PATH_LEFT);
    const auto different_leaf_index = single_element_path(
      leaf, 5, 9, sibling, merkle::Path::Direction::PATH_LEFT);
    const auto different_max_index = single_element_path(
      leaf, 4, 10, sibling, merkle::Path::Direction::PATH_LEFT);
    const auto different_direction = single_element_path(
      leaf, 4, 9, sibling, merkle::Path::Direction::PATH_RIGHT);
    const auto equivalent = single_element_path(
      leaf, 4, 9, sibling, merkle::Path::Direction::PATH_LEFT);
    const auto& same_ref = same;

    require(same == same_ref, "path should equal itself");
    require(same == equivalent, "path should equal equivalent path");
    require(
      !(same == different_leaf_index),
      "path equality should include leaf index");
    require(
      !(same == different_max_index), "path equality should include max index");
    require(
      same != different_direction,
      "path inequality should include element direction");
  }

  void test_tree_partial_serialisation_bounds()
  {
    merkle::Tree empty_tree;
    std::vector<uint8_t> buffer = {0xAA};
    require_throws(
      [&] { empty_tree.serialise(0, 0, buffer); },
      "empty tree partial serialise should throw");
    require(buffer.size() == 1, "failed partial serialise should not append");
    require_throws(
      [&] { empty_tree.serialised_size(0, 0); },
      "empty tree partial serialised_size should throw");

    auto tree = make_tree(6);
    tree.root();
    tree.flush_to(2);
    require(tree.min_index() == 2, "flush_to should update min_index");
    require(tree.max_index() == 5, "unexpected max_index after flush");

    buffer.clear();
    tree.serialise(2, 5, buffer);
    require(
      tree.serialised_size(2, 5) == buffer.size(),
      "partial serialised_size should match serialise size");

    require_throws(
      [&] { tree.serialise(1, 3, buffer); },
      "partial serialise should reject flushed-from index");
    require_throws(
      [&] { tree.serialised_size(1, 3); },
      "partial serialised_size should reject flushed-from index");
    require_throws(
      [&] { tree.serialise(2, 6, buffer); },
      "partial serialise should reject too-large to index");
    require_throws(
      [&] { tree.serialised_size(2, 6); },
      "partial serialised_size should reject too-large to index");
    require_throws(
      [&] { tree.serialise(4, 3, buffer); },
      "partial serialise should reject reversed range");
    require_throws(
      [&] { tree.serialised_size(4, 3); },
      "partial serialised_size should reject reversed range");

    tree.retract_to(4);
    require(tree.min_index() == 2, "retract_to should preserve min_index");
    require(tree.max_index() == 4, "retract_to should update max_index");

    buffer.clear();
    tree.serialise(2, 4, buffer);
    require(
      tree.serialised_size(2, 4) == buffer.size(),
      "partial serialised_size should match after retract");
    require_throws(
      [&] { tree.serialise(2, 5, buffer); },
      "partial serialise should reject retracted to index");
    require_throws(
      [&] { tree.serialised_size(2, 5); },
      "partial serialised_size should reject retracted to index");
  }

  void test_tree_assignment_and_moves()
  {
    const auto source_hashes = make_hashes(5);
    auto source = make_tree(5);
    source.root();
    source.flush_to(1);
    const merkle::Hash expected_root = source.root();
    const auto expected_min = source.min_index();
    const auto expected_max = source.max_index();

    merkle::Tree moved(std::move(source));
    require(moved.root() == expected_root, "move constructor root mismatch");
    require(moved.min_index() == expected_min, "move constructor min mismatch");
    require(moved.max_index() == expected_max, "move constructor max mismatch");
    require(
      moved.leaf(expected_min) == source_hashes[expected_min],
      "moved leaf mismatch");

    auto assign_source = make_tree(4);
    const merkle::Hash assign_root = assign_source.root();
    merkle::Tree move_assigned(hash_with_byte(0xAA));
    move_assigned = std::move(assign_source);
    require(
      move_assigned.root() == assign_root, "move assignment root mismatch");
    require(move_assigned.min_index() == 0, "move assignment min mismatch");
    require(move_assigned.max_index() == 3, "move assignment max mismatch");

    auto self_assigned = make_tree(3);
    self_assigned.root();
    self_assigned.flush_to(1);
    const merkle::Hash self_root = self_assigned.root();

    // Intentional self-assignment coverage for TreeT::operator=.
    self_assigned = self_assigned;
    require(self_assigned.root() == self_root, "self assignment root mismatch");
    require(self_assigned.min_index() == 1, "self assignment min mismatch");
    require(self_assigned.max_index() == 2, "self assignment max mismatch");
  }
}

int main()
{
  try
  {
    test_serialisation_helpers();
    test_path_metadata_and_equality();
    test_tree_partial_serialisation_bounds();
    test_tree_assignment_and_moves();
  }
  catch (const std::exception& ex)
  {
    std::cout << "Error: " << ex.what() << '\n';
    return 1;
  }
  catch (...)
  {
    std::cout << "Error" << '\n';
    return 1;
  }

  return 0;
}
