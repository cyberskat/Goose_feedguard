# Publication inputs

The review bundle is ready for an owner decision on:

| Input | Decision needed |
|---|---|
| Repository | Existing owner/name, or owner/name for a new repository |
| Visibility | Public or private |
| License | The owner's chosen license for the project code |
| Publication approval | Approval of the reviewed source, evidence and documents |

No license grant has been assigned by this draft. Add the approved license and
copyright details before publishing. Third-party compiler, runtime and library
licenses remain with their respective projects; build dependencies are not
vendored in the bundle.

The package includes recorded public market messages, instrument metadata,
normalized input, measured CSV/raw samples, build provenance and source snapshots.
The synthetic workload is reproducible through its generator. Internal staged
notes, local environments, generated executables and unrelated captures are
outside the selected publication scope.

The evidence retains the original machine's build paths and executable hashes
as provenance. Portable audit relocates input paths; it does not assert that a
new local executable has the same file hash as the historical measured binary.

`RELEASE_MANIFEST.json` binds each selected file to its SHA-256; it excludes
itself. The ZIP checksum is written beside the archive. Review the manifest and
[technical report](technical_report.md), then select the publication inputs above.
