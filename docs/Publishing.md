# Preparing a public source copy

English | [日本語](ja/Publishing.md)

Create the public repository from a reviewed source copy, without this workspace's Git history. From the source root:

```bat
python scripts/export_public_source.py out/public-source
```

The destination must not exist. The script copies the selected source files and checks their hashes. It includes the MIT license, English/Japanese documentation, project test sources and the two standalone sample binaries. It excludes Git history, private archives, upstream test checkouts, generated tests, build outputs, agent settings and the legacy build batch.

The selection is defined by file/directory lists and source extensions in `scripts/export_public_source.py`. Review the destination before publication; selection and hash verification do not establish that every included file is free of secrets or third-party material.

Validate the copied source independently with the [build and test](Build-and-Test.md) instructions. Its configuration may fetch upstream tests into its own ignored `Materials/` directory. Those acquired sources and generated binaries are not part of the public source copy.

```bat
cmake --preset x64-Release
cmake --build out/build/x64-Release -j 1
ctest --test-dir out/build/x64-Release --output-on-failure
```

When transferring files to the new repository, use the exporter again with a fresh destination. Do not copy the whole validated build directory, which now contains upstream checkouts and generated outputs. Create the new Git history only from the selected source copy, and keep `.git`, `Archive/`, `Materials/`, `Bin/`, and `out/` out of the transfer.

This script does not initialize a repository, commit, create a remote, or publish anything to GitHub.
