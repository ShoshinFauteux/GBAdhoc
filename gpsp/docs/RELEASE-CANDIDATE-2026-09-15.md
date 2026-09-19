# Release candidate checkpoint — 2026-09-15

The user accepted Quick Load as the fallback candidate before the Unbound
performance campaign. This checkpoint contains the FF artifact/profile fixes,
Mystery Gift delivery/polish, and ROM loading improvements documented alongside
this file. No stability fix is being reverted for the performance campaign.

The exact candidate binaries and build-input hashes live in
`releases/candidate-2026-09-15/`. Its XMB title is **GBAdhoc**; the only difference
from the tested Quick Load PBP is the PARAM.SFO title. Executable, artwork and ME
PRX bytes are unchanged. `tools/relabel_pbp.py` performs and checks that operation.

To reproduce, export this checkpoint's sources to a fresh build directory. Run
the saved `build.sh` using the pinned Docker image in `manifest.json` with the
export mounted at `/build`. Then run `tools/relabel_pbp.py` on the resulting PBP
with title `GBAdhoc`. Compare the output hashes to the manifest. The build keeps
the recorded `GIT_VERSION` and diagnostic flag to reproduce the tested binary;
changing either produces another build requiring validation.

Local tag: `candidate/2026-09-15`. Performance experiments branch from this
checkpoint; returning to the tag restores the candidate source and binaries.
This is a local rollback point, not a published release or a claim that every
historical public-release checklist has been re-run. Unbound performance with
music remains a known limitation. README/public release/app-repository work is
still deferred.

The companion Android app is checkpointed separately on its existing feature
branch. The accepted APK is `0.1.0-mgift2` (version 3), whose hash is recorded in
the manifest. It retains the project's existing development signing key.
