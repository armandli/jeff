#include "router_common.h"

#include <string>

#include <gtest/gtest.h>

namespace {

jeff::json::Value infer_line(std::string_view text) {
    auto r = jeff::json::parse(text);
    EXPECT_TRUE(r) << r.error;
    return std::move(r.value);
}

TEST(RouterCommon, ShellQuoteWrapsAndEscapesSingleQuotes) {
    EXPECT_EQ(router::shell_quote("ls -la"), "'ls -la'");
    EXPECT_EQ(router::shell_quote("it's"), "'it'\"'\"'s'");
    EXPECT_EQ(router::shell_quote(""), "''");
}

TEST(RouterCommon, ShellQuoteRoundTripsThroughSh) {
    const std::string tricky = "a'b \"c\" $HOME `x` \\ \n end";
    auto r = router::popen_read("printf %s " + router::shell_quote(tricky));
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.out, tricky);
}

TEST(RouterCommon, LastLineSkipsEarlierOutputAndBlankLines) {
    EXPECT_EQ(router::last_line("warning\n{\"a\":1}\n"), "{\"a\":1}");
    EXPECT_EQ(router::last_line("x\r\ny\r\n\n\n"), "y");
    EXPECT_EQ(router::last_line("no newline"), "no newline");
    EXPECT_EQ(router::last_line(""), "");
}

// infer.py prints json.dumps output: ", " / ": " separators, null for absent values.
TEST(RouterCommon, ReadsInferPyFields) {
    auto json = infer_line(
        R"({"decision": "AGENT", "reason": "guard: \"rm -rf\"", "p_run": 0.12, )"
        R"("p_agent": 0.88, "threshold": 0.5, "parses": true, "critical": null})");
    EXPECT_EQ(router::json_str(json, "decision"), "AGENT");
    EXPECT_EQ(router::json_str(json, "reason"), "guard: \"rm -rf\"");
    EXPECT_DOUBLE_EQ(router::json_num(json, "p_agent"), 0.88);
    EXPECT_DOUBLE_EQ(router::json_num(json, "threshold", 0.7), 0.5);
    EXPECT_EQ(router::json_bool(json, "parses"), 1);
}

TEST(RouterCommon, MissingOrNullFieldsUseDefaults) {
    auto json = infer_line(R"({"decision": "RUN", "reason": null, "critical": null, "parses": false})");
    EXPECT_EQ(router::json_str(json, "reason"), "");
    EXPECT_EQ(router::json_str(json, "model_name"), "");
    EXPECT_DOUBLE_EQ(router::json_num(json, "threshold", 0.5), 0.5);
    EXPECT_EQ(router::json_bool(json, "critical"), -1);
    EXPECT_EQ(router::json_bool(json, "missing"), -1);
    EXPECT_EQ(router::json_bool(json, "parses"), 0);
}

TEST(RouterCommon, BuildCommandQuotesEveryArgumentAndSkipsEmptyValues) {
    std::string cmd = router::build_command(
        "python3", "infer.py", {{"text", "rm -rf '/'"}, {"threshold", ""}}, true, false);
    EXPECT_EQ(cmd, "'python3' 'infer.py' '--text=rm -rf '\"'\"'/'\"'\"'' --raw");
}

}  // namespace
