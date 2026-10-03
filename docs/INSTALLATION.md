# Installing protoClojure

protoClojure is a Clojure-flavoured language on the protoCore runtime. It is a
consumer of protoCore, never a bundler of it: `bin/protoclj` links
`libprotoCore.so.2`, and every package protoClojure produces declares a runtime
dependency on protoCore's own package instead of shipping a copy.

---

## Prerequisites

- A **C++20** compiler (GCC or Clang; MSVC from Visual Studio 2022 on
  Windows, see [Windows (MSVC)](#windows-msvc)).
- **CMake** 3.20 or newer.
- **libreadline** (`libreadline-dev` on Debian/Ubuntu, `readline-devel` on
  Fedora/RHEL, `brew install readline` on macOS). It is a hard requirement: the
  REPL needs history, arrow-key editing and the multi-line continuation prompt,
  and configuration fails with a `FATAL_ERROR` when it is missing (except on
  Windows, where the REPL uses the console's own line editing).
- **protoCore 2.7.0 or newer** (where `proto::proto_long` first exists), installed, with its CMake package
  configuration. See protoCore's `docs/INSTALLATION.md`.
- **protoIO 0.2.2 or a later 0.2** (the input and output library shared by the protoCore
  runtimes), either installed (the `protoio-dev` package, or any prefix
  given with `-DCMAKE_PREFIX_PATH`, or a build tree given with
  `-DprotoIO_DIR=<path>/protoIO/build_release`) or checked out next to
  protoClojure as `../protoIO`, in which case it is built as part of
  protoClojure's build. It is linked statically: the installed `protoclj`
  does not need it. 0.2.2 is the first with `RunOptions` (`sh`'s `:dir` and
  `:env` without a shell) and a dual-stack listener for servers started
  without a host.
- **OpenSSL 3** development files (`libssl-dev` on Debian/Ubuntu,
  `openssl-devel` on Fedora/RHEL, `brew install openssl@3` on macOS), for
  TLS sockets and `https`. `libssl` becomes a runtime dependency of
  `protoclj`.
- **Running the test suite on macOS** needs GNU `timeout`, which macOS does
  not ship: `brew install coreutils` and put `$(brew --prefix coreutils)/libexec/gnubin`
  on `PATH` (the cross-platform CI job does exactly that).

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

## Windows (MSVC)

protoClojure builds and runs natively on Windows with Visual Studio 2022 (MSVC
19.44 verified, Windows 11), using the CMake and Ninja that ship with it. Build
protoCore first (its `docs/INSTALLATION.md`, "Windows (MSVC)") and install it
into a prefix: on Windows only an installed protoCore package is accepted, never
a sibling build tree. protoIO is compiled from the sibling `../protoIO` as on
Linux. From an "x64 Native Tools Command Prompt":

```bat
set PREFIX=%LOCALAPPDATA%\Programs\proto
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_PREFIX_PATH=%PREFIX% -DCMAKE_INSTALL_PREFIX=%PREFIX% ^
      "-DOPENSSL_ROOT_DIR=C:/Program Files/OpenSSL-Win64"
cmake --build build
ctest --test-dir build -j8
cmake --install build --component protoClojure
%PREFIX%\bin\protoclj --version
```

Any OpenSSL 3 for Windows with headers and import libraries works as
`OPENSSL_ROOT_DIR`; the one PostgreSQL ships (`C:/Program Files/PostgreSQL/17`)
was used for the verification. The build copies the DLLs protoClojure needs
(protoCore's, whatever its file name, taken from the installed package's
imported target, and OpenSSL's `libssl-3-x64.dll` and `libcrypto-3-x64.dll`)
into `build/bin/`, so `protoclj.exe` and the tests run in place. The OpenSSL
DLLs are named for the OpenSSL version that was found: configuring fails if
they are missing, and also if OpenSSL's licence file is not found under its
root (name it with `-DPROTOCLJ_OPENSSL_LICENSE=<file>` then).

`cmake --install` and the packages are self-contained: `<prefix>/bin` gets
`protoclj.exe`, protoCore's DLL, the OpenSSL DLLs and the MSVC runtime DLLs
(`InstallRequiredSystemLibraries`: `vcruntime140.dll`, `msvcp140.dll`, ...),
so no Visual C++ Redistributable is needed, and `share/doc/protoClojure`
gets OpenSSL's licence as `OpenSSL-LICENSE.txt`. `cpack -G ZIP` produces
`protoclojure-<version>-win64.zip`; `cpack -G NSIS` the installer
`protoclojure-<version>-win64.exe` (the NSIS generator is enabled only when
`makensis` is found). CI unpacks the ZIP into an empty directory, and
installs the installer silently (`/S /D=<dir>`), and runs `protoclj.exe`
from each with only the Windows system directories on `PATH`. With the
`bin` directory on `PATH`, `protoclj` runs scripts and the REPL from
`cmd.exe` or PowerShell.

How Windows differs, by design:

- **Same output bytes everywhere.** The standard streams are binary, so
  `println` writes `\n` as on Linux, and the console is switched to UTF-8.
  Scripts are read as bytes: a CRLF script reads as it would on Linux.
- **UTF-8 throughout.** `protoclj.exe` carries a manifest that makes UTF-8 the
  process code page (Windows 10 1903 or later), so arguments, environment
  variables and file names with non-ASCII characters work as on Linux.
- **`/dev/stdin`.** `protoclj /dev/stdin` reads the program from standard input,
  as on Linux, although Windows has no such file.
- **No readline.** The console edits the line and keeps a history itself, so
  the REPL reads plain lines and keeps no history file.
- **Deep recursion** still raises `StackOverflowError`, at the same depth as on
  Linux: the evaluator, futures, `pmap` and actor workers are all protoCore
  threads with a 32 MiB stack reservation (protoCore 2.9.0 and later size them
  itself, `ProtoSpace::setThreadStackBytes`; with an older protoCore they
  take the default `protoclj.exe` is linked with, `/STACK`, the counterpart
  of glibc's default thread attribute), and the limit comes from
  `GetCurrentThreadStackLimits`. The unit tests check that a protoCore thread
  gets 32 MiB on every platform.
- **Servers without a host** (`run-server`, `tcp-listen`) listen on every
  interface, IPv4 and IPv6, as on Linux and macOS (one dual-stack socket), so
  `http://localhost:<port>/` is answered at once although Windows resolves
  `localhost` to `::1` first, and they answer `127.0.0.1` too. A client's
  address reads as `127.0.0.1`, not `::ffff:127.0.0.1`.
- **`sh`** finds programs the way protoIO does on Windows: the application's
  directory, the system directories and `PATH` (`;`-separated), never the
  working directory, with an implied `.exe`; a `.bat` or `.cmd` target is
  refused (`cmd.exe` would reinterpret its arguments), so run
  `cmd /c ...` explicitly for those. `:dir` and `:env` are set by
  `CreateProcessW` itself and need no Unix tools; `:env` gains `SystemRoot`
  when it lacks it, as on the JVM. A child that crashes reports `:exit`
  128 + the matching POSIX signal (139 for an access violation).
- **Files** a script has open can still be deleted or renamed, as on Linux,
  and **TLS** (`https`, `tls-connect`) verifies certificates against the
  Windows certificate stores (both from protoIO 0.2.0).

Test harness. The script tests run through Git for Windows' `bash`, and some
conformance fixtures call Unix tools (`mktemp`, `rm`, `basename`, `sleep`,
`python3`) through `sh`, so Git's `usr/bin` must be on `PATH` (it is inside Git
Bash), and `python3` must be a real Python, not the Microsoft Store stub (CI
copies `python.exe` to `python3.exe`). With that, all 522 tests pass, none
skipped, in CI (`windows-2022`, MSVC, protoCore 2.10.2, and again against
protoCore 2.7.0, the declared minimum); protoClojure's own sources build
warning-free at `/W4`, and CI builds them with `/WX`.

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
| Windows | ZIP, NSIS | **VERIFIED in CI** (2026-10-03, run 37109197694, `windows-2022`, MSVC, protoCore 2.10.2 and 2.7.0). Built and tested (522/522, see [Windows (MSVC)](#windows-msvc)); the ZIP, unpacked into an empty directory, and the NSIS installer, installed silently, each run `protoclj.exe` with only the Windows system directories on `PATH` (it carries protoCore's and OpenSSL's DLLs, OpenSSL's licence and the MSVC runtime). Earlier, by hand (2026-10-01, Windows 11, MSVC 19.44): `cmake --install` into a user prefix, then `protoclj --version`, a script, an example and the REPL from `cmd.exe`. The installer's interactive pages have not been exercised. |

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
