load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# Link into every cc_test in place of GoogleTest's own main. Provides a
# main that runs the file's tests, or its benchmarks when asked.
cc_library(
    name = "main",
    testonly = True,
    srcs = ["main.cc"],
    deps = [
        "@abseil-cpp",
        "@gbenchmark",
        "@gtest",
    ],
)

cc_test(
    name = "main_test",
    size = "small",
    srcs = ["main_test.cc"],
    deps = [
        ":main",
        "@gbenchmark",
        "@gtest",
    ],
)
