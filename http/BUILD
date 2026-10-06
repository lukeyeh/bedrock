load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# HTTP/1.1, from both ends: a client for calling a web API, and a server for
# being one.
package(default_visibility = ["//visibility:public"])

cc_library(
    name = "http",
    srcs = [
        "client.cc",
        "cookie.cc",
        "event_stream.cc",
        "form.cc",
        "head.cc",
        "server.cc",
    ],
    hdrs = [
        "client.h",
        "cookie.h",
        "event_stream.h",
        "form.h",
        "head.h",
        "server.h",
    ],
    deps = [
        "//async:status_macros",
        "//async:task",
        "//net",
        "//net:event_loop",
        "@abseil-cpp",
    ],
)

# A Client for tests of code that makes HTTP requests.
cc_library(
    name = "fake_client",
    testonly = True,
    hdrs = ["fake_client.h"],
    deps = [
        ":http",
        "//async:task",
        "//net",
        "@abseil-cpp",
    ],
)

[
    cc_test(
        name = name,
        size = "small",
        srcs = [name + ".cc"],
        deps = [
            ":fake_client",
            ":http",
            "//async:task",
            "//async:task_scope",
            "//net",
            "//net:event_loop",
            "//testing:main",
            "@abseil-cpp",
            "@gbenchmark",
            "@gtest",
        ],
    )
    for name in [
        "client_test",
        "cookie_test",
        "event_stream_test",
        "fake_client_test",
        "form_test",
        "head_test",
        "server_test",
    ]
]
