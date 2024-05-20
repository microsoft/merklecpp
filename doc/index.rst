merklecpp Documentation
=======================

merklecpp is a header-only C++ library for creation and manipulation of Merkle
trees. The main entry-point is the :cpp:class:`merkle::TreeT` template, which
allows us to create a Merkle tree with a compile-time configurable hash size
and function.

A default implementation without further dependencies is provided as
:cpp:type:`merkle::Tree`, which uses the SHA256 compression function
(:cpp:func:`merkle::sha256_compress`). merklecpp also provides bindings
for the respective OpenSSL and mbedTLS functions (see `Hash functions`_),
which can be specified as a template parameter as illustrated by the following
example:

.. literalinclude:: ../test/demo_tree.cpp
    :language: cpp
    :start-after: SNIPPET_START: OpenSSL-SHA256
    :end-before: SNIPPET_END: OpenSSL-SHA256
    :dedent: 2

Trees
-----

.. doxygenclass:: merkle::TreeT
   :project: merklecpp
   :members:

.. doxygentypedef:: merkle::Tree
   :project: merklecpp

Hashes
------

.. doxygenstruct:: merkle::HashT
   :project: merklecpp
   :members:

Paths
-----

.. doxygenclass:: merkle::PathT
   :project: merklecpp
   :members:

Hash functions
--------------

By default, merklecpp uses the SHA256 compression function
(:cpp:func:`merkle::sha256_compress`) for node hashes. For convenience,
it also provides bindings to the SHA256 implementations in OpenSSL and mbedTLS.
To enable these bindings, merklecpp requires the compiler macros
:code:`HAVE_OPENSSL` and :code:`HAVE_MBEDTLS` to be defined.

.. doxygenfunction:: merkle::sha256_compress
   :project: merklecpp

.. doxygenfunction:: merkle::sha256_openssl
   :project: merklecpp

.. doxygenfunction:: merkle::sha256_compress_mbedtls
   :project: merklecpp

.. doxygenfunction:: merkle::sha256_mbedtls
   :project: merklecpp

.. toctree::
   :maxdepth: 2
   :caption: Contents:


Indices and tables
==================

* :ref:`genindex`
* :ref:`search`
