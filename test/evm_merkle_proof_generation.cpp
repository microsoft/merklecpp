#include <node.h>
#include "merklecppEVM.h" 
#include <iostream>
#include <unordered_map>
#include <memory>

class MerkleTreeEVM{

  private: 
    merkle::KeccakTree m_tree;

  public:
    void insert(std::vector<std::string> vec){
      try{
        for (uint i =0; i < vec.size();i++){               
          m_tree.insert(merkle::KeccakTree::Hash(vec[i].substr(2)));
        }
      }catch(...){
        std::cout << "Insertion error" << std::endl;
      }
    }
    void insert_one(std::string element){
      try{                                 
        m_tree.insert(merkle::KeccakTree::Hash(element.substr(2)));                    
      }catch(...){
        std::cout << "Insertion error" << std::endl;
      }
    }
    std::string root(){
      try{
        return "0x"+m_tree.root().to_string();
      }catch(...){
        std::cout << "Get Root error" << std::endl;
      }
      return "";
    }
    std::string proof(int index){
      std::string pr = "";
      try{
        auto path = m_tree.path(index);
        for (auto it = path->begin(); it != path->end(); ++it) {
          pr += "0x"+it->hash.to_string() + "|";
        }
      }catch(...){
        std::cout << "Get Root error" << std::endl;
        return "";
      }
      if (pr.length()>0){
        pr.pop_back();
      }
      return pr;
    }
  // This tree behaves the same as const tree = new MerkleTree(leaves, keccak256, { sortPairs: true }); in javascript.
  // But in C++ it could be much faster if the leaves size are huge like 100k users.
  // The proof can be verified on chain by solidity library @openzeppelin/contracts/utils/cryptography/MerkleProof.sol
  // So the C++ merklecpp can replace javascript to perform lighltning speed proof generation. 
  // For more details please refer to https://medium.com/block6/using-merkle-trees-in-solidity-64409513989a          
}; 
