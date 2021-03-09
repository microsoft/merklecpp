// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include <chrono>
#include <fstream>
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
      merkle::Tree tree1;
      for (auto h : hashes)
        tree1.insert(h);
      auto root1 = tree1.root();
      std::cout << "ROOT1=" << root1.to_string() << std::endl;

      {
        // Write a new file if it doesn't exist.
        std::ifstream fi("tree.bytes", std::ifstream::binary);
        if (!fi.good())
        {
          std::vector<uint8_t> bytes;
          tree1.serialise(bytes);
          std::ofstream f("tree.bytes", std::ofstream::binary);
          for (char b : bytes)
            f.write(&b, 1);
          f.close();
          fi.close();
        }
      }

      // Read file if it exists
      std::ifstream f("tree.bytes", std::ifstream::binary);
      if (f.good())
      {
        merkle::Tree tree2;
        std::vector<uint8_t> bytes;
        char t;
        while (!f.eof())
        {
          f.read(&t, 1);
          bytes.push_back(t);
        }
        tree2.deserialise(bytes);
        f.close();
        auto root2 = tree2.root();
        std::cout << "ROOT2=" << root2.to_string() << std::endl;
        if (root1 != root2)
          throw std::runtime_error("root hash mismatch");
      }
    }
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
