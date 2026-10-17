#include "BoardProtocol.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{

// Scripted board with a fake clock: each line read takes lineDelayMs
class FakeBoard
{
public:
  std::vector<std::string> lines;
  std::vector<uint32_t> timeouts;
  std::string sent;
  std::string readError;
  bool sendOk = true;
  bool endless = false;
  int lineDelayMs = 0;
  std::chrono::steady_clock::time_point now{};

  bool send(const std::string& data)
  {
    if (!sendOk)
    {
      return false;
    }
    sent += data;
    return true;
  }

  bool readLine(std::string& line, uint32_t timeoutMs, std::string& error)
  {
    timeouts.push_back(timeoutMs);
    if (lineDelayMs > static_cast<int>(timeoutMs) || (!endless && m_nextLine >= lines.size()))
    {
      now += std::chrono::milliseconds(timeoutMs);
      error = readError;
      return false;
    }
    now += std::chrono::milliseconds(lineDelayMs);
    line = endless ? "spam" : lines[m_nextLine++];
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
        [this](std::string& line, uint32_t timeoutMs)
        { return m_board.readLine(line, timeoutMs, m_error); },
        m_error, [this] { return m_board.now; });
  }

  FakeBoard m_board;
  std::string m_error;
  std::unique_ptr<BoardProtocol> m_protocol;
};

TEST_F(BoardProtocolTest, SendsCommandWithNewlineTerminator)
{
  m_board.lines = {"GAIN1,3", "OK"};

  std::string response;
  EXPECT_TRUE(m_protocol->sendCommand("GAIN1,3", &response));
  EXPECT_EQ(m_board.sent, "GAIN1,3\n");
  EXPECT_EQ(response, "OK");
}

TEST_F(BoardProtocolTest, AcceptsEchoAfterPrompt)
{
  m_board.lines = {"> GAIN1,3", "OK"};

  EXPECT_TRUE(m_protocol->sendCommand("GAIN1,3"));
}

TEST_F(BoardProtocolTest, ErrorResponseSetsLastError)
{
  m_board.lines = {"> GAIN1,9", "ERR:Gain must be 0-7"};

  EXPECT_FALSE(m_protocol->sendCommand("GAIN1,9"));
  EXPECT_EQ(m_protocol->getLastError(), "Gain must be 0-7");
}

TEST_F(BoardProtocolTest, StatusReturnsTheBlockWithoutEcho)
{
  m_board.lines = {"> STATUS",     "=== MUX Status ===", "Routing:", "  OUT1 <- IN1",
                   "Gains:",       "  IN1: x2",          "==================", "OK"};

  std::string response;
  EXPECT_TRUE(m_protocol->sendCommand("STATUS", &response));
  EXPECT_EQ(response, "=== MUX Status ===\nRouting:\n  OUT1 <- IN1\nGains:\n  IN1: x2\n"
                      "==================");
}

TEST_F(BoardProtocolTest, StatusWithNoDataReturnsOkLine)
{
  m_board.lines = {"STATUS", "OK"};

  std::string response;
  EXPECT_TRUE(m_protocol->sendCommand("STATUS", &response));
  EXPECT_EQ(response, "OK");
}

TEST_F(BoardProtocolTest, LateReplyOfEarlierCommandIsSkipped)
{
  // The OK of a timed-out command arrives before the echo of the next one
  m_board.lines = {"> GAIN1,3", "OK", "> GAIN2,9", "ERR:Gain must be 0-7"};

  EXPECT_FALSE(m_protocol->sendCommand("GAIN2,9"));
  EXPECT_EQ(m_protocol->getLastError(), "Gain must be 0-7");
}

TEST_F(BoardProtocolTest, ReplyWithoutEchoIsRejected)
{
  m_board.lines = {"OK"};

  EXPECT_FALSE(m_protocol->sendCommand("GAIN1,3"));
  EXPECT_EQ(m_protocol->getLastError(), "Board did not echo GAIN1,3");
}

TEST_F(BoardProtocolTest, EchoOfAnotherCommandIsRejected)
{
  // Leftover characters in the board buffer merge with the command
  m_board.lines = {"> xGAIN1,3", "ERR:Unknown command. Type HELP for help."};

  EXPECT_FALSE(m_protocol->sendCommand("GAIN1,3"));
  EXPECT_EQ(m_protocol->getLastError(), "Board did not echo GAIN1,3");
}

TEST_F(BoardProtocolTest, DeadlineCoversTheWholeReply)
{
  m_board.lineDelayMs = 300;
  m_board.lines = {"> STATUS", "a", "b", "c", "d", "OK"};

  EXPECT_FALSE(m_protocol->sendCommand("STATUS", nullptr, 1000));
  EXPECT_EQ(m_board.timeouts, (std::vector<uint32_t>{1000, 700, 400, 100}));
  EXPECT_EQ(m_protocol->getLastError(), "No OK or ERR from board after STATUS");
}

TEST_F(BoardProtocolTest, EndlessOutputIsCapped)
{
  m_board.endless = true;

  EXPECT_FALSE(m_protocol->sendCommand("STATUS"));
  EXPECT_EQ(m_board.timeouts.size(), BoardProtocol::MAX_REPLY_LINES);
  EXPECT_EQ(m_protocol->getLastError(), "Board reply exceeds 64 lines");
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

TEST_F(BoardProtocolTest, ReadErrorIsKeptInTheMessage)
{
  m_board.readError = "Read timeout";

  EXPECT_FALSE(m_protocol->sendCommand("RESET"));
  EXPECT_EQ(m_protocol->getLastError(), "No response from board (Read timeout)");
}

TEST_F(BoardProtocolTest, StaleErrorIsNotReported)
{
  m_error = "Gain must be 0-7";
  m_board.lines = {"> RESET", "Reset complete"};

  EXPECT_FALSE(m_protocol->sendCommand("RESET"));
  EXPECT_EQ(m_protocol->getLastError(), "No OK or ERR from board after RESET");
}

TEST(BoardGainStatusParseTest, ParsesRoutingAndGainSection)
{
  GainSetting gains[4] = {GainSetting::GAIN_1, GainSetting::GAIN_1,
                          GainSetting::GAIN_1, GainSetting::GAIN_1};
  bool valid[4] = {false, false, false, false};

  const std::string status = "\r\n=== MUX Status ===\r\nRouting:\r\n"
                             "  OUT1 <- IN2\r\n  OUT2 <- X (disconnected)\r\n"
                             "Gains:\r\n  IN1: x2\r\n  IN2: x1\r\n  IN3: x1/8\r\n"
                             "  IN4: x16\r\n==================\r\n";
  ASSERT_TRUE(BoardProtocol::parseGainStatus(status, gains, valid, 4));
  EXPECT_TRUE(valid[0]);
  EXPECT_TRUE(valid[1]);
  EXPECT_TRUE(valid[2]);
  EXPECT_TRUE(valid[3]);
  EXPECT_EQ(gains[0], GainSetting::GAIN_2);
  EXPECT_EQ(gains[1], GainSetting::GAIN_1);
  EXPECT_EQ(gains[2], GainSetting::GAIN_1_8);
  EXPECT_EQ(gains[3], GainSetting::GAIN_16);
}

TEST(BoardGainStatusParseTest, IgnoresRoutingLinesAndForeignText)
{
  GainSetting gains[4] = {GainSetting::GAIN_1, GainSetting::GAIN_1,
                          GainSetting::GAIN_1, GainSetting::GAIN_1};
  bool valid[4] = {false, false, false, false};

  // "OUT1 <- IN2" and "PIN1: x2" must not be mistaken for gain lines
  const std::string status = "Gains:\r\n  OUT1 <- IN2\r\n  PIN1: x2\r\n"
                             "  IN3: x4\r\n";
  ASSERT_TRUE(BoardProtocol::parseGainStatus(status, gains, valid, 4));
  EXPECT_FALSE(valid[0]);
  EXPECT_FALSE(valid[1]);
  EXPECT_TRUE(valid[2]);
  EXPECT_FALSE(valid[3]);
  EXPECT_EQ(gains[2], GainSetting::GAIN_4);
}

TEST(BoardGainStatusParseTest, UnknownLabelLeavesChannelInvalid)
{
  GainSetting gains[4] = {GainSetting::GAIN_1, GainSetting::GAIN_1,
                          GainSetting::GAIN_1, GainSetting::GAIN_1};
  bool valid[4] = {false, false, false, false};

  const std::string status = "Gains:\n  IN1: x12\n  IN2: x\n  IN3: x8\n";
  ASSERT_TRUE(BoardProtocol::parseGainStatus(status, gains, valid, 4));
  EXPECT_FALSE(valid[0]);
  EXPECT_FALSE(valid[1]);
  EXPECT_TRUE(valid[2]);
  EXPECT_EQ(gains[2], GainSetting::GAIN_8);
}

TEST(BoardGainStatusParseTest, ReturnsFalseWithoutGainSection)
{
  GainSetting gains[4] = {};
  bool valid[4] = {true, true, true, true};

  EXPECT_FALSE(BoardProtocol::parseGainStatus("no gains here", gains, valid, 4));
  EXPECT_FALSE(valid[0]);
  EXPECT_FALSE(valid[1]);
  EXPECT_FALSE(valid[2]);
  EXPECT_FALSE(valid[3]);
}

TEST(BoardGainStatusParseTest, RespectsNumChannelsBound)
{
  GainSetting gains[4] = {};
  bool valid[4] = {};

  const std::string status = "Gains:\n  IN1: x16\n  IN4: x2\n";
  ASSERT_TRUE(BoardProtocol::parseGainStatus(status, gains, valid, 2));
  EXPECT_TRUE(valid[0]);
  EXPECT_EQ(gains[0], GainSetting::GAIN_16);
  // Channel 4 is outside the declared bound and must not be written
  EXPECT_FALSE(valid[3]);
}

TEST(BoardRoutingStatusParseTest, ParsesFirmwareStatusBlock)
{
  uint8_t routing[4] = {7, 7, 7, 7};
  bool valid[4] = {false, false, false, false};

  const std::string status = "\r\n=== MUX Status ===\r\nRouting:\r\n"
                             "  OUT1 <- IN3\r\n  OUT2 <- X (disconnected)\r\n"
                             "  OUT3 <- IN1\r\n  OUT4 <- IN4\r\n"
                             "Gains:\r\n  IN1: x2\r\n  IN2: x1\r\n  IN3: x1/8\r\n"
                             "  IN4: x16\r\n==================\r\n";
  ASSERT_TRUE(BoardProtocol::parseRoutingStatus(status, routing, valid, 4));
  EXPECT_TRUE(valid[0]);
  EXPECT_TRUE(valid[1]);
  EXPECT_TRUE(valid[2]);
  EXPECT_TRUE(valid[3]);
  EXPECT_EQ(routing[0], 2);
  EXPECT_EQ(routing[1], IElectronicBoard::DISCONNECTED);
  EXPECT_EQ(routing[2], 0);
  EXPECT_EQ(routing[3], 3);
}

TEST(BoardRoutingStatusParseTest, ParsesLinesWithoutCarriageReturn)
{
  uint8_t routing[4] = {7, 7, 7, 7};
  bool valid[4] = {false, false, false, false};

  // As forwarded by sendCommand, echo line included
  const std::string status = "> STATUS\n=== MUX Status ===\nRouting:\n  OUT1 <- IN2\n"
                             "  OUT2 <- X (disconnected)\nGains:\n  IN1: x1";
  ASSERT_TRUE(BoardProtocol::parseRoutingStatus(status, routing, valid, 4));
  EXPECT_TRUE(valid[0]);
  EXPECT_EQ(routing[0], 1);
  EXPECT_TRUE(valid[1]);
  EXPECT_EQ(routing[1], IElectronicBoard::DISCONNECTED);
  EXPECT_FALSE(valid[2]);
  EXPECT_FALSE(valid[3]);
}

TEST(BoardRoutingStatusParseTest, IgnoresGainSectionAndForeignText)
{
  uint8_t routing[4] = {7, 7, 7, 7};
  bool valid[4] = {false, false, false, false};

  // Gains section and malformed lines are ignored
  const std::string status = "Routing:\n  OUT1 <- IN9\n  OUT2 IN1\n  XOUT3 <- IN1\n"
                             "  OUT4 <- IN2\nGains:\n  OUT1 <- IN3\n";
  ASSERT_TRUE(BoardProtocol::parseRoutingStatus(status, routing, valid, 4));
  EXPECT_FALSE(valid[0]);
  EXPECT_FALSE(valid[1]);
  EXPECT_FALSE(valid[2]);
  EXPECT_TRUE(valid[3]);
  EXPECT_EQ(routing[3], 1);
  EXPECT_EQ(routing[0], 7);
}

TEST(BoardRoutingStatusParseTest, ReturnsFalseWithoutRoutingSection)
{
  uint8_t routing[4] = {7, 7, 7, 7};
  bool valid[4] = {true, true, true, true};

  EXPECT_FALSE(BoardProtocol::parseRoutingStatus("OUT1->IN1 GAIN x2", routing, valid, 4));
  EXPECT_FALSE(valid[0]);
  EXPECT_FALSE(valid[3]);
  EXPECT_EQ(routing[0], 7);
}

TEST(BoardRoutingStatusParseTest, RespectsNumChannelsBound)
{
  uint8_t routing[4] = {7, 7, 7, 7};
  bool valid[4] = {false, false, false, false};

  const std::string status = "Routing:\n  OUT1 <- IN4\n  OUT2 <- IN1\n  OUT4 <- IN1\n";
  ASSERT_TRUE(BoardProtocol::parseRoutingStatus(status, routing, valid, 2));
  // IN4 and OUT4 are out of bounds: not written
  EXPECT_FALSE(valid[0]);
  EXPECT_TRUE(valid[1]);
  EXPECT_EQ(routing[1], 0);
  EXPECT_FALSE(valid[3]);
  EXPECT_EQ(routing[3], 7);
}

} // namespace
