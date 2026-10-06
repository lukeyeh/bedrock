load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# The only code that talks to the kernel. Everything else reaches it through
# //net.
package(default_visibility = ["//visibility:public"])

cc_library(
    name = "socket",
    srcs = ["socket.cc"],
    hdrs = ["socket.h"],
    deps = [
        "@abseil-cpp",
    ],
)

cc_library(
    name = "io",
    srcs = [
        "epoll_backend.cc",
        "epoll_backend.h",
        "io.cc",
        "io_uring_backend.cc",
        "io_uring_backend.h",
    ],
    hdrs = ["io.h"],
    deps = [
        ":socket",
        "//async:awaitable",
        "@abseil-cpp",
        "@liburing",
    ],
)

cc_library(
    name = "random",
    srcs = ["random.cc"],
    hdrs = ["random.h"],
    deps = [
        "@abseil-cpp",
    ],
)

cc_test(
    name = "random_test",
    size = "small",
    srcs = ["random_test.cc"],
    deps = [
        ":random",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_library(
    name = "process",
    srcs = ["process.cc"],
    hdrs = ["process.h"],
    deps = [
        ":io",
        ":socket",
        "//async:task",
        "@abseil-cpp",
    ],
)

cc_test(
    name = "process_test",
    size = "small",
    srcs = ["process_test.cc"],
    deps = [
        ":io",
        ":process",
        "//async:task",
        "//async:task_scope",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "socket_test",
    size = "small",
    srcs = ["socket_test.cc"],
    deps = [
        ":socket",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "io_test",
    size = "small",
    srcs = ["io_test.cc"],
    deps = [
        ":io",
        ":socket",
        "//async:task",
        "//async:task_scope",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "io_uring_backend_test",
    size = "small",
    srcs = [
        "io_uring_backend.h",
        "io_uring_backend_test.cc",
    ],
    deps = [
        ":io",
        ":socket",
        "//async:task",
        "//async:task_scope",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "epoll_backend_test",
    size = "small",
    srcs = [
        "epoll_backend.h",
        "epoll_backend_test.cc",
    ],
    deps = [
        ":io",
        ":socket",
        "//async:task",
        "//async:task_scope",
        "//testing:main",
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)
