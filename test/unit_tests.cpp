// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "util.h"

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <merklecpp.h>

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
  REQUIRE_NOTHROW(merkle::Tree dt(buffer));
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
  merkle::Tree dt(buffer);
  REQUIRE(dt.root() == tree.root());
}

TEST_CASE("Three-node tree")
{
  merkle::Tree::Hash h0, h1, hr;
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
  merkle::Tree dt(buffer);
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
  REQUIRE_NOTHROW(merkle::Tree384 dt(buffer));
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
  merkle::Tree384 dt(buffer);
  REQUIRE(dt.root() == tree.root());
}

TEST_CASE("SHA384 three-node tree")
{
  merkle::Tree384::Hash h0, h1, hr;
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
  merkle::Tree384 dt(buffer);
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
    tree.insert(h);
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
  REQUIRE_NOTHROW(merkle::Tree512 dt(buffer));
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
  merkle::Tree512 dt(buffer);
  REQUIRE(dt.root() == tree.root());
}

TEST_CASE("SHA512 three-node tree")
{
  merkle::Tree512::Hash h0, h1, hr;
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
  merkle::Tree512 dt(buffer);
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
    tree.insert(h);
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