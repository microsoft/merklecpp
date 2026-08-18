merklecpp
=========

merklecpp is a header-only C++ library for creation and manipulation of Merkle
trees. The main entry-point is the :cpp:class:`merkle::TreeT` template, which
allows us to create a Merkle tree with a compile-time configurable hash size
and function.

A default implementation without further dependencies is provided as
:cpp:type:`merkle::Tree`, which uses the built-in SHA256 function
(:cpp:func:`merkle::sha256`). merklecpp also provides bindings for the
respective OpenSSL functions (see `Hash functions`_),
which can be specified as a template parameter as illustrated by the following
example:

.. literalinclude:: ../test/demo_tree.cpp
    :language: cpp
    :start-after: SNIPPET_START: OpenSSL-SHA256
    :end-before: SNIPPET_END: OpenSSL-SHA256
    :dedent: 2

Tiled storage
-------------

The optional :code:`merklecpp_tiles.h` companion persists complete tree ranges
as immutable `tlog-tiles <https://c2sp.org/tlog-tiles>`_ files. Start with the
compiled :ref:`tiled-tree-quick-start`, then continue through the
:doc:`practical guide <tiles-guide>` for configuration and lifecycle operations.
Use the
:doc:`illustrated walkthrough <tiles-illustrated>` to understand tile geometry
and proof resolution.

.. toctree::
   :maxdepth: 2
   :caption: Tiled storage

   tiles-guide
   tiles-illustrated

Proof visualization
-------------------

Explore the `interactive tile and frontier proof atlas <proof-viz/>`_ to see
which durable tiles and resident Merkle nodes supply each proof hash.

API reference
-------------

Trees
~~~~~

.. doxygenclass:: merkle::TreeT
   :project: merklecpp
   :members:

.. doxygentypedef:: merkle::Tree
   :project: merklecpp

Hashes
~~~~~~

.. doxygenstruct:: merkle::HashT
   :project: merklecpp
   :members:

Paths
~~~~~

.. doxygenclass:: merkle::PathT
   :project: merklecpp
   :members:

Hash functions
~~~~~~~~~~~~~~

By default, merklecpp uses its built-in SHA256 function
(:cpp:func:`merkle::sha256`) for node hashes. For convenience, merklecpp also
provides bindings to the SHA256 implementation in OpenSSL.
To enable these bindings, merklecpp requires the compiler macro
:code:`HAVE_OPENSSL` to be defined.

.. doxygenfunction:: merkle::sha256
   :project: merklecpp

.. doxygenfunction:: merkle::sha256_openssl
   :project: merklecpp

Indices and tables
==================

* :ref:`genindex`
* :ref:`search`
