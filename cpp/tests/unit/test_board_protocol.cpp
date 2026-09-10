#include "BoardProtocol.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{

class FakeBoard
{
public:
  std::vector<std::string> lines;
  std::vector<uint32_t> timeouts;
  std::string sent;
  bool sendOk = true;

  bool send(const std::string& data)
  {
    if (!sendOk)
    {
      return false;
    }
    sent += data;
    return true;
  }

  bool readLine(std::string& line, uint32_t timeoutMs)
  {
    timeouts.push_back(timeoutMs);
    if (m_nextLine >= lines.size())
    {
      return false;
    }
    line = lines[m_nextLine++];
    return true;
  }

private:
  size_t m_nextLine = 0;
};

class BoardProtocolTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    m_protocol = std::make_unique<BoardProtocol>(
        [this](const std::string& data) { return m_board.send(data); },
        [this](std::string& line, uint32_t timeoutMs) { return m_board.readLine(line, timeoutMs); },
        m_error);
  }

  FakeBoard m_board;
  std::string m_error;
  std::unique_ptr<BoardProtocol> m_protocol;
};

TEST_F(BoardProtocolTest, SendsCommandWithNewlineTerminator)
{
  m_board.lines = {"OK"};

  std::string response;
  EXPECT_TRUE(m_protocol->sendCommand("GAIN1,3", &response));
  EXPECT_EQ(m_board.sent, "GAIN1,3\n");
  EXPECT_EQ(response, "OK");
}

TEST_F(BoardProtocolTest, SkipsEchoBeforeOk)
{
  m_board.lines = {"GAIN1,3", "OK"};

  std::string response;
  EXPECT_TRUE(m_protocol->sendCommand("GAIN1,3", &response));
  EXPECT_EQ(response, "OK");
}

TEST_F(BoardProtocolTest, ErrorResponseSetsLastError)
{
  m_board.lines = {"ERR:bad channel"};

  EXPECT_FALSE(m_protocol->sendCommand("GAIN1,9"));
  EXPECT_EQ(m_protocol->getLastError(), "bad channel");
}

TEST_F(BoardProtocolTest, StatusCollectsDataLinesUntilOk)
{
  m_board.lines = {"OUT1->IN1", "GAIN x2", "OK"};

  std::string response;
  EXPECT_TRUE(m_protocol->sendCommand("STATUS", &response));
  EXPECT_EQ(response, "OUT1->IN1\nGAIN x2");
}

TEST_F(BoardProtocolTest, StatusWithNoDataReturnsOkLine)
{
  m_board.lines = {"OK"};

  std::string response;
  EXPECT_TRUE(m_protocol->sendCommand("STATUS", &response));
  EXPECT_EQ(response, "OK");
}

TEST_F(BoardProtocolTest, ShortensTimeoutAfterEcho)
{
  m_board.lines = {"echo", "OK"};

  EXPECT_TRUE(m_protocol->sendCommand("GAIN1,3", nullptr, 1000));
  ASSERT_EQ(m_board.timeouts.size(), 2u);
  EXPECT_EQ(m_board.timeouts[0], 1000u);
  EXPECT_EQ(m_board.timeouts[1], 500u);
}

TEST_F(BoardProtocolTest, ShortensTimeoutForStatusContinuation)
{
  m_board.lines = {"data", "OK"};

  EXPECT_TRUE(m_protocol->sendCommand("STATUS", nullptr, 1000));
  ASSERT_EQ(m_board.timeouts.size(), 2u);
  EXPECT_EQ(m_board.timeouts[0], 1000u);
  EXPECT_EQ(m_board.timeouts[1], 200u);
}

TEST_F(BoardProtocolTest, SendFailureReturnsFalseAndKeepsError)
{
  m_board.sendOk = false;
  m_error = "Write failed: boom";

  EXPECT_FALSE(m_protocol->sendCommand("RESET"));
  EXPECT_EQ(m_protocol->getLastError(), "Write failed: boom");
}

TEST_F(BoardProtocolTest, NoResponseSetsGenericError)
{
  EXPECT_FALSE(m_protocol->sendCommand("RESET"));
  EXPECT_EQ(m_protocol->getLastError(), "No response from board");
}

} // namespace
