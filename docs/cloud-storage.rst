Cloud storage and native features
=================================

``lancedb-c`` declares the SDK features it builds against and forwards them, so
a build states what it asks the SDK for instead of inheriting it. These are
forwarding declarations: the public header and the error codes are the same for
every combination, and a feature only changes what the built library can reach.

.. list-table::
   :header-rows: 1
   :widths: 18 28 54

   * - C feature
     - Forwarded to
     - SDK-side wiring
   * - ``remote``
     - ``lancedb/remote``
     - The ``db://`` client and the REST namespace
   * - ``aws``
     - ``lancedb/aws``
     - The SDK's AWS support and the ``dir-aws`` namespace
   * - ``gcs``
     - ``lancedb/gcs``
     - The SDK's GCS support and the ``dir-gcp`` namespace
   * - ``azure``
     - ``lancedb/azure``
     - The ``dir-azure`` namespace and Azure namespace credential vending
   * - ``dynamodb``
     - ``lancedb/dynamodb``
     - The DynamoDB commit handler for ``s3+ddb://``; implies the SDK's ``aws``
   * - ``oss``
     - ``lancedb/oss``
     - The ``dir-oss`` namespace
   * - ``geo``
     - ``lancedb/geo``
     - Lance's ``geo`` feature
   * - ``fp16kernels``
     - ``lancedb/fp16kernels``
     - Native f16/bf16 kernels in ``lance-linalg``

Most of these backends are already reachable without the switches. An embedded
connection opens a generic ``dir`` namespace whose root is resolved through the
connection's object-store registry, and the direct ``lance`` and ``lance-io``
dependencies enable the Lance-side providers for S3, GCS, Azure, OSS, COS, TOS,
GooseFS and Hugging Face as well as Lance's ``geo``. What the forwarding changes
is the SDK's own side of that wiring: the ``dir-aws``/``dir-gcp``/``dir-azure``
implementations, the SDK's ``object_store`` edges, Azure namespace credential
vending and the DynamoDB commit handler. The last two are the capabilities a
default build did not have before, and the ``s3+ddb://`` commit path with them.

AWS profiles and regions
------------------------

On the default S3 backend and the DynamoDB commit handler, ``aws_profile`` is a
storage option that selects the AWS shared profile per store, so one process can
open connections under different profiles without changing ``AWS_PROFILE``:

.. code-block:: c

   LanceDBConnectBuilder* builder =
       lancedb_connect("s3+ddb://bucket/db?ddbTableName=commits");
   builder = lancedb_connect_builder_storage_option(
       builder, "aws_profile", "analytics");
   builder = lancedb_connect_builder_storage_option(
       builder, "aws_region", "eu-west-1");

On those two paths the option takes effect from LanceDB SDK
``v0.42.0-beta.22`` with its Lance ``v15.0.0-beta.26``. A build pinned to the
earlier ``v0.42.0-beta.21``/``v15.0.0-beta.25`` pair ignores ``aws_profile``
there and keeps both on the ambient chains; ``Cargo.toml`` records which pair a
source build uses. The ``v0.1.0-preview.1`` release predates the ``dynamodb``
forwarding and the profile routing: it was built without the ``dynamodb``
feature, so ``s3+ddb://`` is unavailable there.

``aws_region`` is the explicit region and wins over both the environment and the
profile. Without it, the default S3 backend takes ``AWS_REGION`` when it is set
and then falls back to ordinary bucket-region discovery. Custom S3 endpoints and
the DynamoDB client instead use the SDK-resolved region, or the applicable
default chain. An explicit ``aws_profile`` still follows the AWS SDK's
environment precedence: ``AWS_REGION`` and the standard credential variables
take precedence over the profile's own region and keys.

The default set is ``remote,aws,gcs,azure,dynamodb``, and packages built from
this revision with the default flags contain it. ``oss``, ``geo`` and
``fp16kernels`` are opt-in for source builds:

.. code-block:: bash

   cargo rustc --release --locked -p lancedb-c -- \
     -C linker=./cmake/export-linker.sh
   cargo rustc --release --locked -p lancedb-c \
     --features geo,fp16kernels -- -C linker=./cmake/export-linker.sh
   cargo rustc --release --locked -p lancedb-c \
     --no-default-features --features remote,gcs -- \
     -C linker=./cmake/export-linker.sh

Feature selection applies to the packages Cargo selects. A workspace build also
selects ``examples/external_provider``, which depends on this package with its
default features, and feature unification then turns those defaults back on even
when ``--no-default-features`` was passed. Use ``-p lancedb-c`` when the
narrowing is meant to apply to the library.

``--no-default-features`` turns off these forwarding declarations only; the
Lance defaults above still apply, so the Lance-side providers and ``geo``
remain. Without ``remote`` the library cannot produce ``LANCEDB_HTTP`` or
``LANCEDB_RETRY``, because the SDK does not define those error variants; both
codes keep their values for source compatibility.

``geo`` is what builds an RTree index natively; the C index-type enumeration has
no RTree entry yet, so enabling ``geo`` alone does not expose that index type to
C callers.

fp16 kernels
------------

``fp16kernels`` compiles C kernels with the ``cc`` crate, per target:

- Windows: no kernels. The Lance build script warns and skips them.
- arm64: NEON only (``-march=armv8.2-a+fp16`` on Linux, Apple Silicon tuning on
  macOS).
- Non-Windows x86_64: AVX2 always, plus AVX-512 and AMX-FP16 when the compiler
  supports them. The AVX2 object needs ``_Float16``, so the compiler baseline is
  Clang 6 or GCC 12 and newer. GCC 11 rejects the AVX2 object with
  ``'_Float16' is not supported on this target`` and does not know
  ``-mavx512fp16``. Point ``CC`` at a supported compiler when the default is
  older, for example:

  .. code-block:: bash

     CC=clang cargo rustc --release --locked -p lancedb-c \
       --features fp16kernels -- -C linker=./cmake/export-linker.sh
