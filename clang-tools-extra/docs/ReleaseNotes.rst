====================================================
Extra Clang Tools |release| |ReleaseNotesTitle|
====================================================

.. contents::
   :local:
   :depth: 3

Written by the `LLVM Team <https://llvm.org/>`_

.. only:: PreRelease

  .. warning::
     These are in-progress notes for the upcoming Extra Clang Tools |version| release.
     Release notes for previous releases can be found on
     `the Download Page <https://releases.llvm.org/download.html>`_.

Introduction
============

This document contains the release notes for the Extra Clang Tools, part of the
Clang release |release|. Here we describe the status of the Extra Clang Tools in
some detail, including major improvements from the previous release and new
feature work. All LLVM releases may be downloaded from the `LLVM releases web
site <https://llvm.org/releases/>`_.

For more information about Clang or LLVM, including information about
the latest release, please see the `Clang Web Site <https://clang.llvm.org>`_ or
the `LLVM Web Site <https://llvm.org>`_.

Note that if you are reading this file from a Git checkout or the
main Clang web page, this document applies to the *next* release, not
the current one. To see the release notes for a specific release, please
see the `releases page <https://llvm.org/releases/>`_.

What's New in Extra Clang Tools |release|?
==========================================

Some of the major new features and improvements to Extra Clang Tools are listed
here. Generic improvements to Extra Clang Tools as a whole or to its underlying
infrastructure are described first, followed by tool-specific sections.

Major New Features
------------------

...

Improvements to clangd
----------------------

Inlay hints
^^^^^^^^^^^

- Type hints
    * Improved heuristics for showing sugared vs. desguared types
    * Some hints which provide no information (e.g. ``<dependent-type>``) are now omitted
- Parameter hints
    * Parameter hints are now shown for calls through function pointers
    * Parameter hints are now shown for calls to a class's ``operator()``
    * No longer show bogus parameter hints for some builtins like ``__builtin_dump_struct``

Compile flags
^^^^^^^^^^^^^

- System include extractor (``--query-driver``) improvements
    * The directory containing builtin headers is now excluded from extracted system includes
    * Various flags which can affect the system includes (``--target``, ``--stdlib``, ``-specs``) are now forwarded to the driver
    * Fixed a bug where clangd would sometimes try to call a driver that didn't have obj-c support with ``-x objective-c++-header``
    * The driver path is now dot-normalized before being compared to the ``--query-driver`` pattern
    * ``--query-driver`` is now supported by ``clangd-indexer``
- Fixed a regression in clangd 17 where response files would not be expanded

Hover
^^^^^

- Hover now shows alignment info for fields and records

Code completion
^^^^^^^^^^^^^^^

- Refined heuristics for determining whether the use of a function can be a call or not

Code actions
^^^^^^^^^^^^

Signature help
^^^^^^^^^^^^^^

- Improved support for calls through function pointer types

Cross-references
^^^^^^^^^^^^^^^^

- Improved support for C++20 concepts
- Find-references now works for labels
- Improvements to template heuristics

Objective-C
^^^^^^^^^^^

Miscellaneous
^^^^^^^^^^^^^

- Various stability improvements, e.g. crash fixes
- Improved error recovery on invalid code
- Clangd now bails gracefully on assembly and IR source files

Improvements to clang-doc
-------------------------

Improvements to clang-query
---------------------------

The improvements are...

Improvements to clang-rename
----------------------------

The improvements are...

Improvements to clang-tidy
--------------------------

New checks
^^^^^^^^^^

New check aliases
^^^^^^^^^^^^^^^^^

Changes in existing checks
^^^^^^^^^^^^^^^^^^^^^^^^^^

Removed checks
^^^^^^^^^^^^^^

Improvements to include-cleaner
-----------------------------

- Support for ``--only-headers`` flag to limit analysis to headers matching a regex
- Recognizes references through ``concept``s
- Builtin headers are not analyzed
- Handling of references through ``friend`` declarations
- Fixes around handling of IWYU pragmas on stdlib headers
- Improved handling around references to/from template specializations

Improvements to clang-include-fixer
-----------------------------------

The improvements are...

Improvements to modularize
--------------------------

The improvements are...

Improvements to pp-trace
------------------------

Clang-tidy Visual Studio plugin
-------------------------------
