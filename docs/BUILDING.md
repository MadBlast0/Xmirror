## Building from source

x64 builds are automated by the **release** GitHub Actions workflow:

1. [Fork the repository](https://github.com/MadBlast0/XMirror/fork).
2. Open the **Actions** tab.
3. Select **release** and choose **Run workflow**.
4. Download the `XMirror-<version>-x64` artifact (installer and portable ZIP).

A manual run only builds; it never publishes a release. ARM64 is supported by
`build.ps1` but is not built by CI yet.

For local development, see [DEVELOPERS-GUIDE.md](./DEVELOPERS-GUIDE.md).

## Publishing a release

Releases are cut from `main` by pushing a version tag:

1. Set `VERSION` to the new version, for example `0.2.0`, and commit it to `main`.
2. Tag that commit and push the tag:

   ```
   git tag v0.2.0
   git push origin v0.2.0
   ```

The workflow checks that the tag is `v` plus exactly the contents of `VERSION`
and that the commit is on `main`, builds the installer, uploads it to a draft
release, verifies GitHub published a matching sha256
digest for each file, and only then publishes the release. Installed copies of
XMirror find it through the in-app updater within a day.

Tags with a suffix (`v0.2.0-rc1`) are rejected: the updater would never offer
them. If a run fails after creating the draft, delete the draft release before
re-running.
