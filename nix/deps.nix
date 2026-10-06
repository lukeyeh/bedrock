# The libraries bedrock, its tests and its benchmarks are built against.
#
# This is the one place they are chosen. The Nix package (package.nix) and
# the Bazel build (through bazel.nix) both take them from here, at the
# versions flake.lock pins, so that what is developed and tested is built
# from the same libraries as what is installed.
pkgs: {
  # Built as C++20 like bedrock, so that the two agree on which standard
  # library types Abseil's own types stand for.
  abseil-cpp = pkgs.abseil-cpp.override { cxxStandard = "20"; };

  # io_uring, the faster of the two ways os/ does I/O.
  liburing = pkgs.liburing;

  # TLS in net/, and the hashing of the WebSocket handshake.
  openssl = pkgs.openssl;

  # What sqlite/ presents in C++ terms.
  sqlite = pkgs.sqlite;

  # For tests and benchmarks, and for testing/, which is their main function.
  gtest = pkgs.gtest;
  gbenchmark = pkgs.gbenchmark;
}
