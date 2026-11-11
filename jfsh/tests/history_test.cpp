#include <gtest/gtest.h>

#include <jfsh/history/history.hh>

namespace jfsh {
namespace {

std::vector<std::string> texts(const HistoryManager& h) {
  std::vector<std::string> out;
  for (const auto& e : h.entries()) out.push_back(e.text);
  return out;
}

TEST(History, AddAssignsIncreasingNumbers) {
  HistoryManager h{HistOptions{}};
  EXPECT_EQ(h.add("ls"), 1u);
  EXPECT_EQ(h.add("pwd"), 2u);
  EXPECT_EQ(h.last_num(), 2u);
  EXPECT_EQ(h.size(), 2u);
}

TEST(History, EmptyLinesAreNotStored) {
  HistoryManager h{HistOptions{}};
  EXPECT_EQ(h.add(""), 0u);
  EXPECT_EQ(h.size(), 0u);
}

TEST(History, IgnoreSpaceDropsLeadingSpaceLines) {
  HistOptions opts;
  opts.ignore_space = true;
  HistoryManager h{opts};
  EXPECT_EQ(h.add(" secret --token=x"), 0u);
  EXPECT_NE(h.add("ls"), 0u);
  EXPECT_EQ(texts(h), std::vector<std::string>{"ls"});
}

TEST(History, IgnoreDupsOnlyDropsConsecutiveRepeats) {
  HistOptions opts;
  opts.ignore_dups = true;
  HistoryManager h{opts};
  h.add("ls");
  h.add("ls");
  h.add("pwd");
  h.add("ls");
  EXPECT_EQ(texts(h), (std::vector<std::string>{"ls", "pwd", "ls"}));
}

TEST(History, IgnoreAllDupsSuppressesAnyRepeat) {
  HistOptions opts;
  opts.ignore_all = true;
  HistoryManager h{opts};
  h.add("ls");
  h.add("pwd");
  EXPECT_EQ(h.add("ls"), 0u);
  EXPECT_EQ(texts(h), (std::vector<std::string>{"ls", "pwd"}));
}

TEST(History, ReduceBlanksCollapsesWhitespaceRuns) {
  HistOptions opts;
  opts.reduce_blanks = true;
  HistoryManager h{opts};
  h.add("ls    -la\t\t/tmp");
  EXPECT_EQ(texts(h), std::vector<std::string>{"ls -la\t/tmp"});
}

TEST(History, HistsizeKeepsNewestEntries) {
  HistOptions opts;
  opts.histsize = 2;
  HistoryManager h{opts};
  h.add("a");
  h.add("b");
  h.add("c");
  EXPECT_EQ(texts(h), (std::vector<std::string>{"b", "c"}));
  EXPECT_EQ(h.last_num(), 3u);
}

TEST(HistoryExpand, NonBangLinesPassThrough) {
  HistoryManager h{HistOptions{}};
  auto r = h.expand("echo hi");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, "echo hi");
}

TEST(HistoryExpand, BangBangWithoutHistoryIsAnError) {
  HistoryManager h{HistOptions{}};
  EXPECT_FALSE(h.expand("!!").has_value());
}

TEST(HistoryExpand, BangBangRepeatsLastCommand) {
  HistoryManager h{HistOptions{}};
  h.add("make test");
  auto r = h.expand("!!");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, "make test");
}

}  // namespace
}  // namespace jfsh
