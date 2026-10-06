# Bedrock as a Nix package: a static library per directory, their headers,
# and a BUILD file that presents them to Bazel.
#
# This is how other projects use bedrock. They call this file with their own
# compiler and their own Abseil, so that everything in one program is built
# the same way (see the README).
#
# It compiles the same sources as `bazel build //...` does in development.
# What the two builds must agree on is the compiler flags, below and in
# .bazelrc. Source files need no upkeep: every non-test .cc file is compiled.
{
  lib,
  stdenv,
  abseil-cpp,
  liburing,
  openssl,
  sqlite,
  gtest,
  gbenchmark,
}:

stdenv.mkDerivation {
  pname = "bedrock";
  version = "0.1.0";

  # Only the sources, so that editing a README or a BUILD file does not
  # rebuild the package.
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ./installed.BUILD
      ../async
      ../discord
      ../http
      ../json
      ../net
      ../os
      ../sqlite
      ../testing
      ../websocket
    ];
  };

  # Propagated, because the headers installed here include theirs.
  propagatedBuildInputs = [
    abseil-cpp
    liburing
    openssl
    sqlite
  ];

  # Only testing/ uses these, and only a test would link it.
  buildInputs = [
    gtest
    gbenchmark
  ];

  buildPhase = ''
    runHook preBuild

    for library in async discord http json net os sqlite testing websocket; do
      mkdir -p objects/$library
      for source in $(find $library -name '*.cc' ! -name '*_test.cc'); do
        # The flags mirror .bazelrc: C++20, no exceptions.
        $CXX -std=c++20 -O2 -fno-exceptions \
          -Wno-coroutine-missing-unhandled-exception \
          -I. -c "$source" -o "objects/$library/$(basename "$source" .cc).o"
      done
      $AR rcs lib$library.a objects/$library/*.o
    done

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    for library in async discord http json net os sqlite testing websocket; do
      install -Dm644 lib$library.a $out/lib/lib$library.a
      for header in $library/*.h; do
        [ -e "$header" ] && install -Dm644 "$header" "$out/include/$header"
      done
    done
    install -Dm644 nix/installed.BUILD $out/BUILD.bazel

    runHook postInstall
  '';

  meta = {
    description = "C++20 libraries for coroutine programs: async, os, net, http, websocket, json, sqlite, discord";
    homepage = "https://github.com/lukeyeh/bedrock";
    platforms = lib.platforms.linux;
  };
}
