// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include <chrono>
#include <iomanip>
#include <iostream>

#ifdef HAVE_EVERCRYPT
#  include <MerkleTree.h>
#endif

#include "util.h"

#include <merklecpp.h>

#define PRINT_HASH_SIZE 3

int main()
{
  try
  {
    const size_t num_leaves = 11;
    {
      auto hashes = make_hashes(num_leaves);

      // Insert a number of hashes into the tree
      merkle::Tree mt;
      for (auto h : hashes)
        mt.insert(h);
      merkle::Tree::Hash root = mt.root();
      std::cout << mt.to_string(PRINT_HASH_SIZE) << std::endl;

      // Extract some paths
      std::cout << "Paths: " << std::endl;
      for (size_t i = mt.min_index(); i <= mt.max_index(); i++)
      {
        mt.flush_to(i);
        auto path = mt.path(i);
        std::cout << "P" << std::setw(2) << std::setfill('0') << i << ": "
                  << path->to_string(PRINT_HASH_SIZE) << " " << std::endl;
        if (!path->verify(root))
          throw std::runtime_error("root hash mismatch");
        std::vector<uint8_t> chk = *path;
      }

      // Serialise, then deserialise the tree
      std::vector<uint8_t> buffer;
      mt.serialise(buffer);
      merkle::Tree dmt(buffer);
      if (mt.root() != dmt.root())
        throw std::runtime_error("root hash mismatch");

      std::cout << std::endl;
    }

#ifdef HAVE_OPENSSL
    {
      auto hashes = make_hashes(num_leaves);
      /// SNIPPET_START: OpenSSL-SHA256
      merkle::TreeT<32, merkle::sha256_openssl> tree;
      for (auto h : hashes)
        tree.insert(h);
      auto root = tree.root();
      auto path = tree.path(hashes.size() - 1);
      assert(path->verify(root));
      /// SNIPPET_END: OpenSSL-SHA256
    }
#endif

#ifdef HAVE_MBEDTLS
    {
      auto hashes = make_hashes(num_leaves);
      /// SNIPPET_START: mbedTLS-SHA256
      merkle::TreeT<32, merkle::sha256_openssl> tree;
      for (auto h : hashes)
        tree.insert(h);
      auto root = tree.root();
      auto path = tree.path(hashes.size() - 1);
      assert(path->verify(root));
      /// SNIPPET_END: mbedTLS-SHA256
    }
#endif
  }
  catch (std::exception& ex)
  {
    std::cout << "Error: " << ex.what() << std::endl;
    return 1;
  }
  catch (...)
  {
    std::cout << "Error" << std::endl;
    return 1;
  }

  return 0;
}
