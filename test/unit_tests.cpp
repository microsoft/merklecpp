// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "util.h"

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <merklecpp.h>

TEST_CASE("HashT constructors and error paths")
{
  // Default constructor: all bytes zero
  const merkle::Hash h0;
  REQUIRE(h0.size() == 32);
  REQUIRE(h0.serialised_size() == 32);
  for (const auto& b : h0.bytes)
  {
    REQUIRE(b == 0);
  }

  // uint8_t* constructor
  std::array<uint8_t, 32> arr{};
  arr[0] = 0xAB;
  arr[31] = 0xCD;
  const merkle::Hash h_ptr(arr.data());
  REQUIRE(h_ptr.bytes[0] == 0xAB);
  REQUIRE(h_ptr.bytes[31] == 0xCD);

  // std::array constructor
  const merkle::Hash h_arr(arr);
  REQUIRE(h_arr.bytes[0] == 0xAB);
  REQUIRE(h_arr.bytes[31] == 0xCD);

  // String constructor: valid 64-char hex string
  std::string valid_hex(64, '0');
  valid_hex[0] = 'a';
  valid_hex[1] = 'b';
  const merkle::Hash h_str(valid_hex);
  REQUIRE(h_str.bytes[0] == 0xAB);

  // String constructor: invalid length throws
  REQUIRE_THROWS(merkle::Hash(std::string(63, '0')));
  REQUIRE_THROWS(merkle::Hash(std::string(65, '0')));
  REQUIRE_THROWS(merkle::Hash(std::string()));

  // Vector constructor: valid
  std::vector<uint8_t> vec(32, 0);
  vec[0] = 0xCA;
  const merkle::Hash h_vec(vec);
  REQUIRE(h_vec.bytes[0] == 0xCA);

  // Vector constructor: too short throws
  const std::vector<uint8_t> short_vec(31, 0);
  REQUIRE_THROWS(merkle::Hash(short_vec));
  const std::vector<uint8_t> empty_vec;
  REQUIRE_THROWS(merkle::Hash(empty_vec));

  // Vector+position constructor: valid
  std::vector<uint8_t> pos_vec(40, 0);
  pos_vec[8] = 0xFE;
  size_t pos = 8;
  const merkle::Hash h_pos(pos_vec, pos);
  REQUIRE(h_pos.bytes[0] == 0xFE);
  REQUIRE(pos == 40);

  // Vector+position constructor: not enough bytes remaining throws
  size_t pos2 = 10;
  const std::vector<uint8_t> too_short(41, 0); // only 31 bytes from position 10
  REQUIRE_THROWS(merkle::Hash(too_short, pos2));
}

TEST_CASE("HashT methods")
{
  // zero() clears all bytes
  merkle::Hash h;
  h.bytes[0] = 0xFF;
  h.bytes[31] = 0xFF;
  h.zero();
  for (const auto& b : h.bytes)
  {
    REQUIRE(b == 0);
  }

  // to_string (lower case)
  merkle::Hash h2;
  h2.bytes[0] = 0xAB;
  h2.bytes[1] = 0xCD;
  const std::string s = h2.to_string();
  REQUIRE(s.size() == 64);
  REQUIRE(s.substr(0, 4) == "abcd");

  // to_string upper case
  const std::string s_upper = h2.to_string(32, false);
  REQUIRE(s_upper.substr(0, 4) == "ABCD");

  // to_string with limited num_bytes
  const std::string s2 = h2.to_string(2);
  REQUIRE(s2.size() == 4);
  REQUIRE(s2 == "abcd");

  // operator== and operator!=
  merkle::Hash ha;
  merkle::Hash hb;
  REQUIRE(ha == hb);
  REQUIRE_FALSE(ha != hb);
  hb.bytes[0] = 1;
  REQUIRE_FALSE(ha == hb);
  REQUIRE(ha != hb);

  // Assignment operator
  merkle::Hash hc;
  hc = hb;
  REQUIRE(hc == hb);

  // serialise / deserialise round-trip
  std::vector<uint8_t> buf;
  hb.serialise(buf);
  REQUIRE(buf.size() == 32);
  REQUIRE(buf[0] == 1);
  size_t pos = 0;
  merkle::Hash hd;
  hd.deserialise(buf, pos);
  REQUIRE(hd == hb);
  REQUIRE(pos == 32);

  // operator std::vector<uint8_t>()
  merkle::Hash he;
  he.bytes[5] = 0x42;
  const std::vector<uint8_t> converted = he;
  REQUIRE(converted.size() == 32);
  REQUIRE(converted[5] == 0x42);
}

TEST_CASE("Empty tree")
{
  merkle::Tree tree;

  REQUIRE(tree.empty());
  REQUIRE(tree.min_index() == 0);
  REQUIRE(tree.max_index() == 0);

  REQUIRE_THROWS(tree.root());
  REQUIRE_THROWS(tree.path(0));
  REQUIRE_THROWS(tree.past_root(0));
  REQUIRE_THROWS(tree.past_path(0, 1));

  REQUIRE_NOTHROW(tree.flush_to(0));
  REQUIRE_NOTHROW(tree.retract_to(0));

  std::vector<uint8_t> buffer;
  REQUIRE_NOTHROW(tree.serialise(buffer));
  REQUIRE_NOTHROW(merkle::Tree dt(buffer)); // NOLINT(misc-const-correctness)
}

TEST_CASE("One-node tree")
{
  merkle::Tree::Hash h;
  merkle::Tree tree(h);

  REQUIRE(tree.min_index() == 0);
  REQUIRE(tree.max_index() == 0);

  REQUIRE(tree.root() == h);
  REQUIRE(tree.leaf(0) == h);
  REQUIRE(*tree.path(0)->root() == h);
  REQUIRE(*tree.past_root(0) == h);
  REQUIRE_THROWS(tree.past_root(1));
  REQUIRE(*tree.past_path(0, 0)->root() == h);
  REQUIRE_THROWS(tree.past_path(0, 1));

  REQUIRE_NOTHROW(tree.flush_to(0));
  REQUIRE_NOTHROW(tree.retract_to(0));

  std::vector<uint8_t> buffer;
  REQUIRE_NOTHROW(tree.serialise(buffer));
  merkle::Tree dt(buffer); // NOLINT(misc-const-correctness)
  REQUIRE(dt.root() == tree.root());
}

TEST_CASE("Three-node tree")
{
  merkle::Tree::Hash h0;
  merkle::Tree::Hash h1;
  merkle::Tree::Hash hr;
  h1.bytes[31] = 1;

  merkle::Tree tree;

  REQUIRE_NOTHROW(tree.insert(h0));
  REQUIRE_NOTHROW(tree.insert(h1));

  hr = tree.root();

  REQUIRE(tree.min_index() == 0);
  REQUIRE(tree.max_index() == 1);

  REQUIRE(tree.leaf(0) == h0);
  REQUIRE(tree.leaf(1) == h1);
  REQUIRE(*tree.path(0)->root() == hr);
  REQUIRE(*tree.past_root(0) == h0);
  REQUIRE(*tree.past_root(1) == hr);
  REQUIRE(*tree.past_root(0) == h0);
  REQUIRE(*tree.past_root(1) == hr);

  auto pp00 = tree.past_path(0, 0);
  REQUIRE(pp00->size() == 0);
  REQUIRE(pp00->leaf() == h0);
  REQUIRE(*pp00->root() == h0);
  REQUIRE(pp00->begin() == pp00->end());

  auto pp01 = tree.past_path(0, 1);
  REQUIRE(pp01->size() == 1);
  REQUIRE(pp01->leaf() == h0);
  REQUIRE((*pp01)[0] == h1);
  auto it = pp01->begin();
  REQUIRE(it->hash == h1);
  REQUIRE(it->direction == merkle::Path::PATH_RIGHT);
  it++;
  REQUIRE(it == pp01->end());
  REQUIRE(*pp01->root() == hr);

  std::vector<uint8_t> buffer;
  REQUIRE_NOTHROW(tree.serialise(buffer));
  merkle::Tree dt(buffer); // NOLINT(misc-const-correctness)
  REQUIRE(dt.root() == tree.root());

  merkle::Tree copy = tree;

  REQUIRE_NOTHROW(tree.flush_to(0));
  REQUIRE_NOTHROW(tree.flush_to(1));
  REQUIRE_THROWS(tree.retract_to(0));

  REQUIRE_NOTHROW(copy.flush_to(0));
  REQUIRE_NOTHROW(copy.retract_to(1));
  REQUIRE_NOTHROW(copy.retract_to(0));
  REQUIRE_THROWS(copy.flush_to(1));

  REQUIRE(copy.size() == 1);
  REQUIRE(copy.root() == h0);
}

#ifdef HAVE_OPENSSL
TEST_CASE("SHA384 empty tree")
{
  merkle::Tree384 tree;

  REQUIRE(tree.empty());
  REQUIRE(tree.min_index() == 0);
  REQUIRE(tree.max_index() == 0);

  REQUIRE_THROWS(tree.root());
  REQUIRE_THROWS(tree.path(0));
  REQUIRE_THROWS(tree.past_root(0));
  REQUIRE_THROWS(tree.past_path(0, 1));

  REQUIRE_NOTHROW(tree.flush_to(0));
  REQUIRE_NOTHROW(tree.retract_to(0));

  std::vector<uint8_t> buffer;
  REQUIRE_NOTHROW(tree.serialise(buffer));
  REQUIRE_NOTHROW(merkle::Tree384 dt(buffer)); // NOLINT(misc-const-correctness)
}

TEST_CASE("SHA384 one-node tree")
{
  merkle::Tree384::Hash h;
  merkle::Tree384 tree(h);

  REQUIRE(tree.min_index() == 0);
  REQUIRE(tree.max_index() == 0);

  REQUIRE(tree.root() == h);
  REQUIRE(tree.leaf(0) == h);
  REQUIRE(*tree.path(0)->root() == h);
  REQUIRE(*tree.past_root(0) == h);
  REQUIRE_THROWS(tree.past_root(1));
  REQUIRE(*tree.past_path(0, 0)->root() == h);
  REQUIRE_THROWS(tree.past_path(0, 1));

  REQUIRE_NOTHROW(tree.flush_to(0));
  REQUIRE_NOTHROW(tree.retract_to(0));

  std::vector<uint8_t> buffer;
  REQUIRE_NOTHROW(tree.serialise(buffer));
  merkle::Tree384 dt(buffer); // NOLINT(misc-const-correctness)
  REQUIRE(dt.root() == tree.root());
}

TEST_CASE("SHA384 three-node tree")
{
  merkle::Tree384::Hash h0;
  merkle::Tree384::Hash h1;
  merkle::Tree384::Hash hr;
  h1.bytes[47] = 1;

  merkle::Tree384 tree;

  REQUIRE_NOTHROW(tree.insert(h0));
  REQUIRE_NOTHROW(tree.insert(h1));

  hr = tree.root();

  REQUIRE(tree.min_index() == 0);
  REQUIRE(tree.max_index() == 1);

  REQUIRE(tree.leaf(0) == h0);
  REQUIRE(tree.leaf(1) == h1);
  REQUIRE(*tree.path(0)->root() == hr);
  REQUIRE(*tree.past_root(0) == h0);
  REQUIRE(*tree.past_root(1) == hr);

  auto pp00 = tree.past_path(0, 0);
  REQUIRE(pp00->size() == 0);
  REQUIRE(pp00->leaf() == h0);
  REQUIRE(*pp00->root() == h0);

  auto pp01 = tree.past_path(0, 1);
  REQUIRE(pp01->size() == 1);
  REQUIRE(pp01->leaf() == h0);
  REQUIRE((*pp01)[0] == h1);
  REQUIRE(*pp01->root() == hr);

  std::vector<uint8_t> buffer;
  REQUIRE_NOTHROW(tree.serialise(buffer));
  merkle::Tree384 dt(buffer); // NOLINT(misc-const-correctness)
  REQUIRE(dt.root() == tree.root());

  merkle::Tree384 copy = tree;

  REQUIRE_NOTHROW(tree.flush_to(0));
  REQUIRE_NOTHROW(tree.flush_to(1));
  REQUIRE_THROWS(tree.retract_to(0));

  REQUIRE_NOTHROW(copy.flush_to(0));
  REQUIRE_NOTHROW(copy.retract_to(1));
  REQUIRE_NOTHROW(copy.retract_to(0));
  REQUIRE_THROWS(copy.flush_to(1));

  REQUIRE(copy.size() == 1);
  REQUIRE(copy.root() == h0);
}

TEST_CASE("SHA384 paths")
{
  const size_t num_leaves = 64;
  auto hashes = make_hashesT<48>(num_leaves);

  merkle::Tree384 tree;
  for (auto& h : hashes)
  {
    tree.insert(h);
  }
  auto root = tree.root();

  for (size_t i = 0; i < num_leaves; i++)
  {
    auto path = tree.path(i);
    REQUIRE(path->verify(root));
    std::vector<uint8_t> serialised_path;
    path->serialise(serialised_path);
    REQUIRE(path->serialised_size() == serialised_path.size());
  }
}

TEST_CASE("SHA512 empty tree")
{
  merkle::Tree512 tree;

  REQUIRE(tree.empty());
  REQUIRE(tree.min_index() == 0);
  REQUIRE(tree.max_index() == 0);

  REQUIRE_THROWS(tree.root());
  REQUIRE_THROWS(tree.path(0));
  REQUIRE_THROWS(tree.past_root(0));
  REQUIRE_THROWS(tree.past_path(0, 1));

  REQUIRE_NOTHROW(tree.flush_to(0));
  REQUIRE_NOTHROW(tree.retract_to(0));

  std::vector<uint8_t> buffer;
  REQUIRE_NOTHROW(tree.serialise(buffer));
  REQUIRE_NOTHROW(merkle::Tree512 dt(buffer)); // NOLINT(misc-const-correctness)
}

TEST_CASE("SHA512 one-node tree")
{
  merkle::Tree512::Hash h;
  merkle::Tree512 tree(h);

  REQUIRE(tree.min_index() == 0);
  REQUIRE(tree.max_index() == 0);

  REQUIRE(tree.root() == h);
  REQUIRE(tree.leaf(0) == h);
  REQUIRE(*tree.path(0)->root() == h);
  REQUIRE(*tree.past_root(0) == h);
  REQUIRE_THROWS(tree.past_root(1));
  REQUIRE(*tree.past_path(0, 0)->root() == h);
  REQUIRE_THROWS(tree.past_path(0, 1));

  REQUIRE_NOTHROW(tree.flush_to(0));
  REQUIRE_NOTHROW(tree.retract_to(0));

  std::vector<uint8_t> buffer;
  REQUIRE_NOTHROW(tree.serialise(buffer));
  merkle::Tree512 dt(buffer); // NOLINT(misc-const-correctness)
  REQUIRE(dt.root() == tree.root());
}

TEST_CASE("SHA512 three-node tree")
{
  merkle::Tree512::Hash h0;
  merkle::Tree512::Hash h1;
  merkle::Tree512::Hash hr;
  h1.bytes[63] = 1;

  merkle::Tree512 tree;

  REQUIRE_NOTHROW(tree.insert(h0));
  REQUIRE_NOTHROW(tree.insert(h1));

  hr = tree.root();

  REQUIRE(tree.min_index() == 0);
  REQUIRE(tree.max_index() == 1);

  REQUIRE(tree.leaf(0) == h0);
  REQUIRE(tree.leaf(1) == h1);
  REQUIRE(*tree.path(0)->root() == hr);
  REQUIRE(*tree.past_root(0) == h0);
  REQUIRE(*tree.past_root(1) == hr);

  auto pp00 = tree.past_path(0, 0);
  REQUIRE(pp00->size() == 0);
  REQUIRE(pp00->leaf() == h0);
  REQUIRE(*pp00->root() == h0);

  auto pp01 = tree.past_path(0, 1);
  REQUIRE(pp01->size() == 1);
  REQUIRE(pp01->leaf() == h0);
  REQUIRE((*pp01)[0] == h1);
  REQUIRE(*pp01->root() == hr);

  std::vector<uint8_t> buffer;
  REQUIRE_NOTHROW(tree.serialise(buffer));
  merkle::Tree512 dt(buffer); // NOLINT(misc-const-correctness)
  REQUIRE(dt.root() == tree.root());

  merkle::Tree512 copy = tree;

  REQUIRE_NOTHROW(tree.flush_to(0));
  REQUIRE_NOTHROW(tree.flush_to(1));
  REQUIRE_THROWS(tree.retract_to(0));

  REQUIRE_NOTHROW(copy.flush_to(0));
  REQUIRE_NOTHROW(copy.retract_to(1));
  REQUIRE_NOTHROW(copy.retract_to(0));
  REQUIRE_THROWS(copy.flush_to(1));

  REQUIRE(copy.size() == 1);
  REQUIRE(copy.root() == h0);
}

TEST_CASE("SHA512 paths")
{
  const size_t num_leaves = 64;
  auto hashes = make_hashesT<64>(num_leaves);

  merkle::Tree512 tree;
  for (auto& h : hashes)
  {
    tree.insert(h);
  }
  auto root = tree.root();

  for (size_t i = 0; i < num_leaves; i++)
  {
    auto path = tree.path(i);
    REQUIRE(path->verify(root));
    std::vector<uint8_t> serialised_path;
    path->serialise(serialised_path);
    REQUIRE(path->serialised_size() == serialised_path.size());
  }
}

TEST_CASE("SHA256 vs SHA384 vs SHA512 produce different roots")
{
  merkle::Tree tree256;
  merkle::Tree384 tree384;
  merkle::Tree512 tree512;

  merkle::Hash h256;
  merkle::Hash384 h384;
  merkle::Hash512 h512;

  h256.bytes[0] = 42;
  h384.bytes[0] = 42;
  h512.bytes[0] = 42;

  tree256.insert(h256);
  tree256.insert(h256);
  tree384.insert(h384);
  tree384.insert(h384);
  tree512.insert(h512);
  tree512.insert(h512);

  auto r256 = tree256.root();
  auto r384 = tree384.root();
  auto r512 = tree512.root();

  REQUIRE(r256.size() == 32);
  REQUIRE(r384.size() == 48);
  REQUIRE(r512.size() == 64);
}
#endif