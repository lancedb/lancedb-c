Versioning and ABI
==================

The C binding has its own release line. That line is independent of the
``lancedb`` Rust SDK version it pins, and independent of the Cargo package
version in ``Cargo.toml``. A release names the SDK revision it bundles in its
release notes.

Version sources
---------------

The release version comes from the Git tag. Tags use strict SemVer with a ``v``
prefix, for example ``v0.1.0-preview.2``. The Cargo package version is fixed at
``0.0.0`` because the crate is not published and a Cargo version that looks like
an SDK version would mislead readers.

The public header exposes:

.. code-block:: c

   LANCEDB_VERSION_MAJOR
   LANCEDB_VERSION_MINOR
   LANCEDB_VERSION_PATCH
   LANCEDB_VERSION_STRING
   LANCEDB_VERSION  /* alias for LANCEDB_VERSION_STRING */

``lancedb_version()`` returns the version embedded in the loaded library as a
static, NUL-terminated string owned by the library. The caller must not free it.
The function is declared in the :doc:`binding version group <api/version>`.

The checked-in ``include/lancedb.h`` reports ``0.0.0-dev`` so it remains usable
directly without a generated file. CMake renders the installed header with the
configure-time version, and the package builder renders the same block for
release archives. Header, runtime string and package manifest therefore agree
for a package built from the same source.

A direct ``cargo build`` uses the same ``LANCEDB_VERSION`` override or exact-tag
resolution for the runtime string, and falls back to ``0.0.0-dev`` when no tag is
present. Use the CMake-installed or package-rendered header when a tagged direct
Cargo build needs compile-time macros that match the runtime; the checked-in
header deliberately stays at the development default.

Compatibility policy
--------------------

Before 1.0, the C ABI may change incompatibly. A change that breaks source or
binary compatibility bumps the minor version, and the release notes tell C and
C++ callers what to change and that they must recompile against the published
header. Preview releases make no long-term C ABI promise.

From 1.0 onward, add new functions and enum values without changing existing
declarations. Adding a field to an existing public struct changes its size and
layout and is a breaking ABI change. Changing a function signature, an existing
enum value or documented behavior also requires a major version.

CMake shared-library builds from this revision, and release packages from
``v0.1.0-preview.2`` onward, use this metadata:

- On Linux, before 1.0 the SONAME is ``liblancedb.so.<major>.<minor>``; from 1.0
  it is ``liblancedb.so.<major>``. The unversioned ``liblancedb.so`` linker name
  remains available beside it.
- On macOS, the install name is ``@rpath/liblancedb.dylib``.
- On Windows, consumers link through ``lancedb.dll.lib`` (MSVC) or the GNU import
  library. Use the installed CMake target rather than relying on a library-name
  spelling.

Shared-library exports
----------------------

Shared libraries built by the release package workflow, and by CMake on Linux,
macOS or native Windows MSVC, export only the ``lancedb_*`` functions declared
in ``include/lancedb.h``. The build derives that allowlist from the header's
``extern "C"`` declarations and passes a platform-specific export control to
the final ``liblancedb`` link: an ELF version script, a Mach-O
exported-symbols list, or an MSVC ``.def`` file with its paired import library.
Symbols that are merely present in the linked artifact, including global
symbols from statically linked Rust dependencies, are hidden.

``.github/scripts/check_exports.py`` verifies the result. It reads the installed
or packaged header, requires the dynamic export table to match the header
declarations exactly, loads every public function, and checks that
``lancedb_version()`` returns the package version. Package verification and
``check-install`` use this check, so a library with an extra or missing export
fails before publication.

The allowlist describes the export surface of a built shared library; it does
not add a compatibility promise. The preview ABI policy above still applies.
Adding a public function still changes the Rust FFI and ``include/lancedb.h``
together, and the function is not exported until it is declared in the header.

The boundary covers Linux ELF, macOS Mach-O and native Windows MSVC shared
builds. Windows GNU shared builds remain supported for source consumers but do
not use this policy, and they are not release targets. A plain ``cargo build``
or ``cargo rustc`` without the export linker wrapper is also a development
path: it does not apply the boundary, and its ``cdylib`` may export dependency
symbols. It is not a release artifact. A static library built with
``-DBUILD_SHARED_LIBS=OFF`` has no dynamic export surface; static consumers
link the archive together with the native dependencies reported by the
installed CMake target or by pkg-config's ``Libs.private``.

Earlier preview packages exposed incidental symbols from statically linked
dependencies. The ``v0.1.0-preview.2`` archives exported ``crc_fast_*``
functions on Linux, macOS and Windows; macOS also exported
``blake3_compress_in_place_portable``, and Windows also exported
``aws_lc_0_41_0_jent_*`` functions. These were never part of the C API. The
first published preview built with the allowlist was ``v0.1.0-preview.3``;
published previews from that release onward export only ``lancedb_*``
functions. No documented C function or header declaration was removed.

Published packages
------------------

The package workflow publishes five native targets: ``linux-amd64``,
``linux-arm64``, ``darwin-arm64`` (Apple Silicon), ``windows-amd64`` and
``windows-arm64``. Intel macOS is not supported.

Each archive is named ``lancedb-c-<commit>-<target>.tar.gz`` and contains:

- ``LICENSE``
- ``include/lancedb.h`` rendered with the release version
- the target shared library, with the Linux SONAME and unversioned alias from
  ``v0.1.0-preview.2`` onward, and the MSVC import library on Windows
- ``manifest.json``
- ``SHA256SUMS`` covering the package contents

The ``v0.1.0-preview.1`` Linux archive predates the SONAME rule and contains
only the unversioned ``liblancedb.so``. The archive has a sibling ``.sha256``
file, and the release includes a combined ``SHA256SUMS``.

A prebuilt archive does not contain the CMake package config or pkg-config
file that a source install provides, so it does not support
``find_package(lancedb)`` or ``pkg-config`` directly. A source install can
produce a shared or static library; the export allowlist above applies only to
its shared library. The manifest is the machine-readable contract for
downstream loaders:

.. list-table::
   :header-rows: 1

   * - Field
     - Meaning
   * - ``schemaVersion``
     - Manifest schema version, currently ``1``
   * - ``name``
     - Package name, ``lancedb-c``
   * - ``version``
     - Tag-derived binding version
   * - ``target``
     - Go-style ``GOOS-GOARCH`` target name
   * - ``rustTarget``
     - Rust target triple used to build the library
   * - ``commit``
     - Packaged commit
   * - ``library``
     - Package-relative shared-library path
   * - ``include``
     - Package-relative public header path
   * - ``importLibrary``
     - Package-relative import library, or ``null`` when not needed
   * - ``license``
     - Package-relative license path

The release page is the authoritative record of a published preview. It names
the tag ref object, peeled commit, pinned LanceDB and Lance revisions, and every
archive digest. See the `LanceDB C releases
<https://github.com/lancedb/lancedb-c/releases>`_ for the current list.

Tag dispatches without ``publish_release`` only build CI artifacts.
Publication is limited to preview tags; stable releases remain a manual project
decision. A published release is never replaced.
