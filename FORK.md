# arngory/naiveproxy

A fork of [klzgrad/naiveproxy](https://github.com/klzgrad/naiveproxy) for the naive client of
[Niko](https://github.com/arngory/niko). Our branch is **`niko`**: the naive stack on
**Chromium 158.0.8127.36**, with SagerNet's Cronet C API, from which
[arngory/cronet-go](https://github.com/arngory/cronet-go) builds libcronet (its `naiveproxy` submodule). `master` and
the version branches are upstream's, as upstream has them.

## What the branch is

Built the way upstream builds a Chromium version: an import commit and the patch series on top of it.

1. `Import chromium-158.0.8127.36` (`tools/import-upstream.sh`), on upstream's import of 154.0.8037.49.
2. klzgrad's patches from `master`, moved onto 158 with their authors kept: the minimized build, the stubs for
   Android, the builtin certificate verifier, forced tunnels, HTTP/2 and HTTP/3 CONNECT Fast Open, the preamble, the
   platform fixes (ARM, MIPS, loong64, OpenWrt, musl), the naive binary and its tools, tests and CI.
3. SagerNet's Cronet native C API and `grpc_support`, and their integration for cronet-go, moved onto 158 as well.
4. What 158 needed: `CHROMIUM_VERSION`, Rust removed from `net`, no dbus in Cronet builds, `use_nss_certs=false`
   working again, PartitionAlloc for the naive binary.
5. Ours for Niko, each its own commit:
   - naive behaving as on 154 under 158: Chrome 158's Accept headers and socket priority in the preamble, TLS Trust
     Anchor IDs as Chrome sends them, the known-root check for QUIC certificates, receive-window auto-tuning only
     where naive sets it, CONNECT tunnels forced only when the naive binary asks;
   - roots: Android reads no system roots and Windows trusts no roots of its stores unless the environment names
     them; an option for QUIC to accept a locally trusted root;
   - connect-udp: HTTP Datagrams on bidirectional streams over QUIC and a datagram stream in the C API, with the
     capsule handling it needs (raycontrol `docs/spec/naive-udp-v1.md`);
   - exact server certificates in the native API (raycontrol `docs/spec/cert-pin-v1.md`);
   - no Android JNI audit logging, no LanguageTag in prefs.

The tests of the C API changes are cronet-go's (its `test/` module runs against the libraries built from here).

## Who pins it

| Where | Pin |
|---|---|
| cronet-go source branch and `go-158` (`niko/go-158-pins`) | submodule `naiveproxy` at `89a5411` |
| cronet-go `niko/quic-local-roots` | submodule at `46938b7` |

Every pinned commit is in `niko`, which is never rewritten or deleted (no rebase, no force-push): cronet-go's pins
are commits of it, and Niko's releases name the commit their libcronet was built from as the source of its Chromium
code. `claude/clever-feynman-w0e2v1` is the branch the port was made on; it holds the same commits up to `02a1b88`.

## Working on it

- A change goes to `niko` by a pull request from a branch of its own, then cronet-go moves its submodule
  ("Update naiveproxy") and its Build tests the libraries. Upstream's `build.yml` runs on `master` only.
- A new Chromium version is a new branch, as 158 was made: the import, then this branch's series moved onto it.
- Checks (`.github/workflows/checks.yml`): "Commit messages" (no bare `#N`; in a fork GitHub links a bare `#N` to
  upstream's issues, so write `arngory/naiveproxy#N`) and "Secrets" (gitleaks, rules in `.gitleaks.toml`). Both take
  only the pull request's own line, not upstream's commits or an import.
- A pull request that changes the checks themselves is merged by the owner.
- The rules shared with raycontrol, niko-server and Niko: raycontrol's
  [CONTRIBUTING.md](https://github.com/arngory/raycontrol/blob/main/CONTRIBUTING.md).
