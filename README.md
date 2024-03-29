[![Continuous Integration](https://github.com/microsoft/merklecpp/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/microsoft/merklecpp/actions/workflows/ci.yml)
[![Documentation](https://github.com/microsoft/merklecpp/actions/workflows/build-docs.yml/badge.svg?branch=main)](https://github.com/microsoft/merklecpp/actions/workflows/build-docs.yml)

# merklecpp

A header-only C++ library for creation and manipulation of Merkle trees. It supports the usual
operations, like hash insertion, root computation, and path extraction, as well as some more
unusual features like flushing, retracting, and tree segment serialisation.

## Usage

    #include <merklecpp.h>

    merkle::Tree::Hash hash("fa8f44eabb728d4020e7f33d1aa973faaef19de6c06679bccdc5100a3c01f54a");

    merkle::Tree tree;
    tree.insert(hash);
    ...
    auto root = tree.root();
    auto path = tree.path(0);
    assert(path->verify(root));


## Contributing

This project welcomes contributions and suggestions.  Most contributions require you to agree to a
Contributor License Agreement (CLA) declaring that you have the right to, and actually do, grant us
the rights to use your contribution. For details, visit https://cla.opensource.microsoft.com.

When you submit a pull request, a CLA bot will automatically determine whether you need to provide
a CLA and decorate the PR appropriately (e.g., status check, comment). Simply follow the instructions
provided by the bot. You will only need to do this once across all repos using our CLA.

This project has adopted the [Microsoft Open Source Code of Conduct](https://opensource.microsoft.com/codeofconduct/).
For more information see the [Code of Conduct FAQ](https://opensource.microsoft.com/codeofconduct/faq/) or
contact [opencode@microsoft.com](mailto:opencode@microsoft.com) with any additional questions or comments.

## Trademarks

This project may contain trademarks or logos for projects, products, or services. Authorized use of Microsoft
trademarks or logos is subject to and must follow
[Microsoft's Trademark & Brand Guidelines](https://www.microsoft.com/en-us/legal/intellectualproperty/trademarks/usage/general).
Use of Microsoft trademarks or logos in modified versions of this project must not cause confusion or imply Microsoft sponsorship.
Any use of third-party trademarks or logos are subject to those third-party's policies.
