#include "LineBuffer.h"

#include <gtest/gtest.h>

#include <string>

TEST(LineBufferTest, PartialLineIsNotReturned)
{
  LineBuffer buffer(64);
  EXPECT_EQ(buffer.append("hello", 5), LineBuffer::AppendResult::Ok);

  std::string line;
  EXPECT_FALSE(buffer.popLine(line));
  EXPECT_EQ(buffer.size(), 5u);
  EXPECT_FALSE(buffer.empty());
}

TEST(LineBufferTest, SplitAppendsAssembleLine)
{
  LineBuffer buffer(64);
  EXPECT_EQ(buffer.append("hel", 3), LineBuffer::AppendResult::Ok);
  EXPECT_EQ(buffer.append("lo\n", 3), LineBuffer::AppendResult::Ok);

  std::string line;
  ASSERT_TRUE(buffer.popLine(line));
  EXPECT_EQ(line, "hello");
  EXPECT_TRUE(buffer.empty());
}

TEST(LineBufferTest, MultipleLinesInOneAppend)
{
  LineBuffer buffer(64);
  EXPECT_EQ(buffer.append("a\nb\nc\n", 6), LineBuffer::AppendResult::Ok);

  std::string line;
  ASSERT_TRUE(buffer.popLine(line));
  EXPECT_EQ(line, "a");
  ASSERT_TRUE(buffer.popLine(line));
  EXPECT_EQ(line, "b");
  ASSERT_TRUE(buffer.popLine(line));
  EXPECT_EQ(line, "c");
  EXPECT_FALSE(buffer.popLine(line));
}

TEST(LineBufferTest, TrailingCarriageReturnStripped)
{
  LineBuffer buffer(64);
  EXPECT_EQ(buffer.append("hello\r\n", 7), LineBuffer::AppendResult::Ok);

  std::string line;
  ASSERT_TRUE(buffer.popLine(line));
  EXPECT_EQ(line, "hello");
}

TEST(LineBufferTest, EmptyLineIsReturned)
{
  LineBuffer buffer(64);
  EXPECT_EQ(buffer.append("\n", 1), LineBuffer::AppendResult::Ok);

  std::string line = "unchanged";
  ASSERT_TRUE(buffer.popLine(line));
  EXPECT_TRUE(line.empty());
}

TEST(LineBufferTest, OverflowOnlyWhenUnterminated)
{
  LineBuffer buffer(8);
  EXPECT_EQ(buffer.append("123456789", 9), LineBuffer::AppendResult::Overflow);
  EXPECT_EQ(buffer.size(), 9u);
}

TEST(LineBufferTest, TerminatedOverflowIsNotRejected)
{
  LineBuffer buffer(8);
  EXPECT_EQ(buffer.append("123456789\n", 10), LineBuffer::AppendResult::Ok);

  std::string line;
  ASSERT_TRUE(buffer.popLine(line));
  EXPECT_EQ(line, "123456789");
}

TEST(LineBufferTest, ClearResetsContent)
{
  LineBuffer buffer(8);
  buffer.append("abc", 3);
  buffer.clear();

  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ(buffer.size(), 0u);
}
