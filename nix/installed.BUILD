# Bedrock as a project that uses it sees it: one library per directory of the
# repository, under the directory's name. Depend on "@bedrock//:net" and
# include "net/stream.h".
#
# This file is installed with the package (see package.nix), and is the BUILD
# file of the repository that rules_nixpkgs makes of it. It is evaluated
# there, where other repositories are not known by their short names, so it
# names them canonically. That fixes what the using project must call the
# third-party libraries in its MODULE.bazel: "abseil-cpp", "liburing",
# "openssl", "sqlite", "gtest" and "gbenchmark", each holding one library of
# the same name.
load("@@rules_cc+//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

ABSEIL = "@@rules_nixpkgs_core++nix_pkg+abseil-cpp//:abseil-cpp"

# Task, Awaitable, Sequence, TaskScope, and the CO_ status macros.
cc_library(
    name = "async",
    srcs = ["lib/libasync.a"],
    hdrs = glob(["include/async/*.h"]),
    includes = ["include"],
    deps = [ABSEIL],
)

# Sockets and asynchronous I/O over io_uring or epoll.
cc_library(
    name = "os",
    srcs = ["lib/libos.a"],
    hdrs = glob(["include/os/*.h"]),
    includes = ["include"],
    deps = [
        ":async",
        ABSEIL,
        "@@rules_nixpkgs_core++nix_pkg+liburing//:liburing",
    ],
)

# The event loop, and byte streams: TCP, TLS, buffered reading, URLs.
cc_library(
    name = "net",
    srcs = ["lib/libnet.a"],
    hdrs = glob(["include/net/*.h"]),
    includes = ["include"],
    deps = [
        ":async",
        ":os",
        ABSEIL,
        "@@rules_nixpkgs_core++nix_pkg+openssl//:openssl",
    ],
)

# HTTP/1.1: a client, a server, forms and cookies, and http::FakeClient for
# tests.
cc_library(
    name = "http",
    srcs = ["lib/libhttp.a"],
    hdrs = glob(["include/http/*.h"]),
    includes = ["include"],
    deps = [
        ":async",
        ":net",
        ABSEIL,
    ],
)

# WebSocket connections, and websocket::FakeServer for tests.
cc_library(
    name = "websocket",
    srcs = ["lib/libwebsocket.a"],
    hdrs = glob(["include/websocket/*.h"]),
    includes = ["include"],
    deps = [
        ":async",
        ":http",
        ":net",
        ABSEIL,
        "@@rules_nixpkgs_core++nix_pkg+openssl//:openssl",
    ],
)

# JSON values, parsing and serializing.
cc_library(
    name = "json",
    srcs = ["lib/libjson.a"],
    hdrs = glob(["include/json/*.h"]),
    includes = ["include"],
    deps = [ABSEIL],
)

# SQLite databases.
cc_library(
    name = "sqlite",
    srcs = ["lib/libsqlite.a"],
    hdrs = glob(["include/sqlite/*.h"]),
    includes = ["include"],
    deps = [
        ABSEIL,
        "@@rules_nixpkgs_core++nix_pkg+sqlite//:sqlite",
    ],
)

# A Discord bot's client: gateway events, REST calls, slash commands.
cc_library(
    name = "discord",
    srcs = ["lib/libdiscord.a"],
    hdrs = glob(["include/discord/*.h"]),
    includes = ["include"],
    deps = [
        ":async",
        ":http",
        ":json",
        ":net",
        ":websocket",
        ABSEIL,
    ],
)

# The main() of a test binary that holds tests and benchmarks.
cc_library(
    name = "testing_main",
    testonly = True,
    srcs = ["lib/libtesting.a"],
    deps = [
        ABSEIL,
        "@@rules_nixpkgs_core++nix_pkg+gbenchmark//:gbenchmark",
        "@@rules_nixpkgs_core++nix_pkg+gtest//:gtest",
    ],
    alwayslink = True,
)
