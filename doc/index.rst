merklecpp Documentation
=======================

merklecpp is a header-only C++ library for creation and manipulation of Merkle
trees. The main entry-point is the :cpp:class:`merkle::TreeT` template, which allows us to
create a Merkle tree with a compile-time configurable hash size and function.
:cpp:type:`merkle::Tree` provides a default implementation using the SHA256 compression
function (:cpp:func:`merkle::sha256_compress`).

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

.. doxygenfunction:: merkle::sha256_compress
   :project: merklecpp

.. doxygenfunction:: merkle::sha256_compress_openssl
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
