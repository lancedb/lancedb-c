# Releasing

This is the maintainer runbook for `lancedb-c` preview releases. Public
versioning and ABI rules are in [docs/versioning.rst](docs/versioning.rst);
consumer installation is in [README.md](README.md). Stable releases remain a
manual maintainer decision.

## Version source

The C binding has its own release line, independent of the Cargo package and
the pinned `lancedb` SDK. The Cargo package version is fixed at `0.0.0`; the
release version comes from the Git tag.

Tags use strict SemVer with a `v` prefix, for example
`v0.1.0-preview.2`. A tag dispatch uses the explicitly selected tag. For a local
build, if several valid tags point at the same commit, the last stable tag in
Git's version sort wins; otherwise the last prerelease wins. `v0.0.0-dev` is
reserved for the untagged fallback, and untagged or pull-request builds report
`0.0.0-dev`.

CMake renders the installed header with the resolved version, and the package
builder renders the same block for release archives. The checked-in
`include/lancedb.h` deliberately stays at `0.0.0-dev`; do not replace that
block in a normal change. CMake records HEAD and the tag ref as configure-time
dependencies, so an existing build directory reconfigures when either changes.
`LANCEDB_VERSION` is an environment override, not a CMake file input; rerun the
CMake configure step after changing it.

## Package contract

The `Shared Library Packages` workflow builds and tests five native targets.
It runs for `v*` tags and explicit dispatches; pull requests run the fast
packaging and release fixture checks through `CI`.

| Target | Runner | Package |
| --- | --- | --- |
| `linux-amd64` | Ubuntu 22.04 x86_64 | `lancedb-c-<commit>-linux-amd64.tar.gz` |
| `linux-arm64` | Ubuntu 22.04 arm64 | `lancedb-c-<commit>-linux-arm64.tar.gz` |
| `darwin-arm64` | macOS Apple Silicon | `lancedb-c-<commit>-darwin-arm64.tar.gz` |
| `windows-amd64` | Windows x86_64 | `lancedb-c-<commit>-windows-amd64.tar.gz` |
| `windows-arm64` | Windows arm64 | `lancedb-c-<commit>-windows-arm64.tar.gz` |

Intel macOS is not supported. Each package contains:

```text
LICENSE
include/lancedb.h
lib/<shared library>
lib/<import library>       # Windows only
lib/<soname>               # Linux; liblancedb.so is the link
manifest.json
SHA256SUMS
```

The archive is named `lancedb-c-<commit>-<target>.tar.gz` and has a sibling
`.sha256` file. The release also publishes a combined `SHA256SUMS` covering all
archives. `manifest.json` is the machine-readable contract for downstream
loaders; its target names use Go's `GOOS-GOARCH` spelling, and its fields are
documented in [docs/versioning.rst](docs/versioning.rst).

Linux packages from `v0.1.0-preview.2` onward contain a versioned
`liblancedb.so.<major>.<minor>` plus the unversioned `liblancedb.so` symlink.
Before 1.0 the major.minor pair is the ABI boundary; from 1.0 it is the major
version. The `v0.1.0-preview.1` Linux package has no SONAME and contains only
the unversioned library. Packages from preview.2 use
`@rpath/liblancedb.dylib` on macOS.

The package archive contains only the header, shared library, Windows import
library when applicable, license, manifest and checksums. It does not include
the CMake package config or pkg-config file that `cmake --install` produces from
the current source. Do not document an archive
as supporting `find_package(lancedb)` or `pkg-config`; those integrations
belong to a source install or to a package that explicitly contains them.

## Cutting a preview release

1. Confirm the exact commit is on the intended branch and CI is green.
2. Create an annotated or lightweight tag such as `v0.1.0-preview.3` on that
   commit. Do not tag a parent, another branch, or a commit that is not the
   package head.
3. Push the tag. This builds and verifies the five packages and uploads them as
   CI artifacts, but it does not publish a GitHub Release.
4. To publish, dispatch `Shared Library Packages` on the exact tag with the
   `publish_release` Boolean enabled.

The workflow fetches the tag and requires the tag's peeled commit to match the
checked-out commit, then uses the tag version for the build, manifest, installed
header and consumer verification. That is a tag/checkout consistency check, not
a branch or release-commit check: a tag on a parent commit or another branch
still points at the commit that the tag dispatch checks out. Keep the manual
branch and commit checks in steps 1-2; the workflow does not verify release
intent. Each package job builds the target library, creates the package, and
verifies the archive with a real consumer before uploading it.

After all five package jobs pass, the publish job re-verifies every archive,
manifest and checksum against the exact tag, creates a draft release, uploads
the five archives, their `.sha256` files and the combined `SHA256SUMS`, and only
then publishes the release. Publication is limited to preview tags. A tag push
or dispatch without `publish_release` only produces CI artifacts.

## Publication behavior and retries

A retry reuses an existing draft. The workflow verifies the draft tag and
identity, checks every existing asset's digest, and uploads only the missing
assets. A published release is never replaced. If the draft has already become
public, stop rather than editing or replacing it.

If an upload is interrupted after GitHub creates an empty `starter` asset, the
workflow fails closed and names the placeholder. Inspect the exact draft:

```bash
gh release view <tag> --json databaseId,isDraft,assets
```

Confirm that the named asset is the empty `starter` (`size` 0, no digest), then
remove only that asset from the draft:

```bash
gh release delete-asset <tag> <name> --yes
```

Keep every other draft asset and re-dispatch the same exact tag. The retry
uploads only what is missing; do not delete and recreate the tag or release.

## Tag immutability

Tag ref reads and release writes are separate GitHub operations. The workflow
rechecks the exact remote tag before draft creation and again before
publication, but a tag move in the remaining interval cannot be made atomic
from the workflow. An atomic guarantee needs a tag ruleset that blocks updates
and deletions together with release immutability; this repository does not
currently enable those protections. Do not move or delete a release tag after
publication. If a release is wrong, publish a new preview tag rather than
rewriting history.

## Release notes and evidence

The generated release notes record the tag ref object, peeled commit, pinned
LanceDB SDK and Lance revisions, and the SHA-256 of every archive. Keep the
preview ABI note and the shared-library export-surface statement in the notes.
Earlier preview packages also exposed incidental symbols from statically linked
dependencies (CRC, BLAKE3 and AWS-LC jitterentropy). Those symbols were never
part of the C API; mention this export-surface change in the preview notes. A
documented C function or header declaration must not disappear silently.

Before dispatch, run the local fixture checks:

```bash
python3 .github/scripts/test_package.py
python3 .github/scripts/test_release.py
```

These checks validate package layout, checksums, version resolution, tag
verification, draft reuse, partial retry, starter-asset recovery and the
preview-only publication gate without building the Rust library.
