# Tests for the C++ header-only SavedModelAPI.
load("//tensorflow:tensorflow.bzl", "tf_cc_test")

package(
    # copybara:uncomment default_applicable_licenses = ["//tensorflow:license"],
    licenses = ["notice"],
)

tf_cc_test(
    name = "saved_model_api_test",
    srcs = [
        "saved_model_api_test.cc",
    ],
    data = [
        "//tensorflow/cc/saved_model:saved_model_half_plus_two",
    ],
    deps = [
        "//tensorflow/c:tf_status_headers",
        "//tensorflow/cc/experimental/base/public:runtime",
        "//tensorflow/cc/experimental/base/public:runtime_builder",
        "//tensorflow/cc/experimental/base/public:status",
        "//tensorflow/cc/saved_model/experimental/public:saved_model_api",
        "//tensorflow/core:lib",
        "//tensorflow/core:test",
        "//tensorflow/core:test_main",
    ],
)
