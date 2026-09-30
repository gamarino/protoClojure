# Installing protoClojure

protoClojure is a Clojure-flavoured language on the protoCore runtime. It is a
consumer of protoCore, never a bundler of it: `bin/protoclj` links
`libprotoCore.so.2`, and every package protoClojure produces declares a runtime
dependency on protoCore's own package instead of shipping a copy.

---

## Prerequisites

- A **C++20** compiler (GCC or Clang).
- **CMake** 3.20 or newer.
- **libreadline** (`libreadline-dev` on Debian/Ubuntu, `readline-devel` on
  Fedora/RHEL, `brew install readline` on macOS). It is a hard requirement: the
  REPL needs history, arrow-key editing and the multi-line continuation prompt,
  and configuration fails with a `FATAL_ERROR` when it is missing.
- **protoCore 2.6.1 or newer**, installed, with its CMake package
  configuration. See protoCore's `docs/INSTALLATION.md`.
- **protoIO 0.1** (the input and output library shared by the protoCore
  runtimes), either installed (the `protoio-dev` package, or any prefix
  given with `-DCMAKE_PREFIX_PATH`, or a build tree given with
  `-DprotoIO_DIR=<path>/protoIO/build_release`) or checked out next to
  protoClojure as `../protoIO`, in which case it is built as part of
  protoClojure's build. It is linked statically: the installed `protoclj`
  does not need it.
- **OpenSSL 3** development files (`libssl-dev` on Debian/Ubuntu,
  `openssl-devel` on Fedora/RHEL, `brew install openssl@3` on macOS), for
  TLS sockets and `https`. `libssl` becomes a runtime dependency of
  `protoclj`.

---

## Building against an installed protoCore

```bash
# protoCore installed in a default prefix: nothing to pass.
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release

# protoCore installed elsewhere.
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release -DPROTO_CORE_PREFIX=$HOME/.local
# equivalently
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$HOME/.local

cmake --build build_release -j4
ctest --test-dir build_release --output-on-failure
```

The discovery is `find_package(protoCore 2.0 CONFIG)`, so the prefix must hold
`lib/cmake/protoCore/protoCoreConfig.cmake`. **A prefix holding only
`libprotoCore` and `protoCore.h` is no longer accepted**: without the package
configuration there is no way to tell protoCore 1.x from 2.x, and linking the
wrong major version is silent.

The version floor is `2.6.1` and the ceiling is the next major version.
The HTTP server creates a thread per connection from its accept thread, and
protoCore before 2.6.1 could detach a live context's roots when a thread was
created from another thread. (protoClojure's maps are protoCore `ProtoMap`s and
its actor mailboxes protoCore `ProtoMPSCQueue`s, both added in 2.1.0.)
protoCore's major version and its soname move together.
protoClojure additionally asserts that the package's `SOVERSION` is `2`.

## Building against a sibling developer tree

When no installed package is found *and* no prefix was named, protoClojure falls
back to the sibling source tree `../protoCore`, searching `build_release`, then
`build`, then `build_check`. The fallback prints a `WARNING`: it performs no
package version check (it does check that the build carries `SOVERSION 2`) and
must not be used to produce a distributable package.

Pass `-DPROTOCORE_REQUIRE_PACKAGE=ON` to turn the fallback into a hard error.
**Every packaging build sets it.** Switching a build directory between the two
modes leaves a stale `PROTOCORE_LIBRARY` cache entry; delete the build directory
rather than reconfiguring in place.

---

## Installing

```bash
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$HOME/.local -DCMAKE_PREFIX_PATH=$HOME/.local
cmake --build build_release -j4
cmake --install build_release --component protoClojure
```

Installed layout, relative to the prefix:

| Content | Location |
|---------|----------|
| `protoclj` | `bin/` |
| `LICENSE`, `README.md` and the Markdown documentation | `share/doc/protoClojure/` |
| Example `.clj` scripts | `share/protoClojure/examples/` |
| Benchmark scripts and their published results | `share/protoClojure/benchmarks/` |

**`protoclj` needs no external data file at run time.** The reader, the compiler
and the core namespaces are compiled into the binary; the examples and the
benchmarks are reference material a user can run, not a runtime dependency. The
only paths `protoclj` reads from the environment are `PROTOCLJ_ACTOR_WORKERS`
(the actor pool size) and `HOME` (the REPL history file). Two further variables
are diagnostics, not configuration: `PROTOCLJ_GC_STATS=1` prints a one-line
census of the collector's work to standard error when a script ends, and
`PROTOCLJ_NO_GC_SAFEPOINT=1` disables the VM's garbage-collection safepoint so
its cost can be measured — it makes a long-running loop retain everything it
allocates, and is for A/B measurement only.

protoClojure installs **no** copy of protoCore. `bin/protoclj` carries the
install RPATH `$ORIGIN/../<libdir>` (`@executable_path/../<libdir>` on macOS),
so a protoCore installed into the same prefix is found with no
`LD_LIBRARY_PATH`.

---

## Packages (CPack)

```bash
cmake -S . -B build_pkg -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=<protocore-prefix> -DPROTOCORE_REQUIRE_PACKAGE=ON
cmake --build build_pkg -j4
cd build_pkg && cpack -G DEB
```

Generators are chosen at configure time; the DEB and RPM generators are enabled
only when `dpkg` and `rpmbuild` are found, because `cpack` aborts the whole run
when a generator's tool is missing and would take the TGZ down with it. Each
configure prints whether a generator was enabled or disabled, and why.

Package names are pinned rather than left to each generator's default casing:
`protoclojure` for DEB, `protoClojure` for RPM. Both declare a bounded
dependency on protoCore's own package:

| Format | Relation |
|--------|----------|
| DEB | `Depends: protocore (>= 2.6.1), protocore (<< 3.0.0)` |
| RPM | `Requires: protoCore >= 2.6.1, protoCore < 3.0.0` |

`CPACK_DEBIAN_PACKAGE_SHLIBDEPS` is enabled, so `dpkg-shlibdeps` adds the
shared libraries `protoclj` links to the DEB's `Depends`: `libc6`,
`libstdc++6`, `libgcc-s1`, `libreadline8t64` and, since input and output
landed, `libssl3t64` (OpenSSL, pulled in by the statically linked protoIO).
protoIO itself is not a dependency: `protoio-dev` is needed to build, never
to run. Verified 2026-09-30 with `cpack -G DEB`: `Depends: protocore (>=
2.6.1), protocore (<< 3.0.0), libc6 (>= 2.38), libgcc-s1 (>= 3.0),
libreadline8t64 (>= 6.0), libssl3t64 (>= 3.0.0), libstdc++6 (>= 13),
protocore (>= 2.6.2)`.

### Platform verification status

Last verified 2026-09-27 against protoClojure 0.0.1 and protoCore 2.5.0
(`PROTOCORE_ABI_SOVERSION 3`), built with `-DPROTOCORE_REQUIRE_PACKAGE=ON` so the
sibling developer fallback was a hard error.

| Platform | Packaging | Status |
|----------|-----------|--------|
| Linux / Debian-Ubuntu | TGZ, DEB | **VERIFIED.** Installed with `dpkg -i` as root in a throwaway `ubuntu:24.04` container and run there from `/usr/bin/protoclj`, outside any repository, with no `LD_LIBRARY_PATH` set. |
| Linux / Fedora-RHEL | TGZ, RPM | **VERIFIED.** `cpack -G RPM` executed in a throwaway `fedora:41` container (glibc 2.40, `rpm` 4.20.1); the RPM installed with `rpm -i` and `protoclj` ran correctly there. This closes the gap left by decision D-I2. |
| macOS | DragNDrop | **UNVERIFIED.** Configured and reviewed only; there is no macOS host here. Review is not verification. |
| Windows | NSIS, ZIP | **UNVERIFIED.** Configured and reviewed only; there is no Windows host here. |

### Known defect: the DEB dependency floor does not encode the ABI

The `Depends` field is a *version range*, and on its own that range is not an ABI
check. `PROTOCORE_ABI_SOVERSION` went from `2` to `3` in protoCore **2.2.0**, so
protoCore 2.0.0 and 2.1.0 carry `libprotoCore.so.2` while 2.2.0 and later carry
`libprotoCore.so.3`. A floor of ``2.1.0`` therefore admits a protoCore whose
SONAME this package was not linked against.

This was demonstrated, not argued. A decoy `protocore` 2.1.0 package providing
only `libprotoCore.so.2` was installed in a container; `dpkg -i` then accepted
this package, and the installed binary failed to start with
`libprotoCore.so.3: cannot open shared object file`. The install succeeded and
the program did not run.

Two things limit the damage, and one closes it:

- At **build** time the failure is loud, not silent. `find_package(protoCore …)`
  alone does accept a SOVERSION-2 protoCore, but `CMakeLists.txt` follows it with
  an explicit `protoCore_SOVERSION` assertion against `PROTOCORE_ABI_SOVERSION`,
  which stops configuration with a `FATAL_ERROR` naming both numbers. Verified by
  configuring against a complete forged 2.1.0 / SOVERSION 2 prefix.
- The **RPM** does not have this hole. `rpm` generates
  `Requires: libprotoCore.so.3()(64bit)` automatically from the linked binary, and
  that requirement is on the SONAME rather than the version. Verified: the decoy
  protoCore 2.1.0 does not satisfy it and `rpm -i` refuses.
- Raising the DEB floor to `2.2.0`, the first protoCore that shipped SOVERSION 3,
  would make the DEB range agree with the ABI. That is a packaging change for the
  maintainer to take, and it is not made here.

### Known defect: the DEB does not refresh the shared-library cache

Neither this package nor protoCore's carries a `postinst` or an `ldconfig`
trigger, so `ldconfig -p` does not list `libprotoCore.so.3` after `dpkg -i`.
Programs still start, because each binary carries
`RUNPATH $ORIGIN/../${CMAKE_INSTALL_LIBDIR}` and because the library lands in a
directory the dynamic loader searches by default, but the cache is misleading.
Run `ldconfig` after installing. The RPM has no such defect.
