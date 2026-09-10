#include "Protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

using namespace AFM;

TEST(ProtocolTest, CommandStringRoundTripCoversAllCommands)
{
  for (uint8_t v = 0; v < static_cast<uint8_t>(Command::UNKNOWN); ++v)
  {
    Command cmd = static_cast<Command>(v);
    EXPECT_EQ(parseCommand(commandToString(cmd)), cmd) << commandToString(cmd);
  }
}

TEST(ProtocolTest, ParseLineBasicCommands)
{
  auto cmd = parseLine("MEASURE:SINC");
  EXPECT_EQ(cmd.command, Command::MEAS_SINC);
  EXPECT_FALSE(cmd.isQuery);
  EXPECT_TRUE(cmd.args.empty());
  EXPECT_EQ(cmd.rawLine, "MEASURE:SINC");

  EXPECT_EQ(parseLine("*IDN").command, Command::IDN);
  EXPECT_EQ(parseLine("SYSTEM:PING").command, Command::SYST_PING);
}

TEST(ProtocolTest, ParseLineIsCaseInsensitive)
{
  EXPECT_EQ(parseLine("measure:sinc").command, Command::MEAS_SINC);
  EXPECT_EQ(parseLine("System:Version").command, Command::SYST_VERSION);
  EXPECT_EQ(parseLine("*idn").command, Command::IDN);
}

TEST(ProtocolTest, ParseLineDetectsQuerySuffix)
{
  auto cmd = parseLine("SYSTEM:STATUS?");
  EXPECT_EQ(cmd.command, Command::SYST_STATUS);
  EXPECT_TRUE(cmd.isQuery);

  auto idn = parseLine("*IDN?");
  EXPECT_EQ(idn.command, Command::IDN);
  EXPECT_TRUE(idn.isQuery);

  auto set = parseLine("BOARD:GAIN 1,3");
  EXPECT_FALSE(set.isQuery);
}

TEST(ProtocolTest, ParseLineSplitsAndTrimsArguments)
{
  auto cmd = parseLine("MEASURE:SINC 1024, 200 ,100,0.5");
  ASSERT_EQ(cmd.args.size(), 4u);
  EXPECT_EQ(cmd.args[0], "1024");
  EXPECT_EQ(cmd.args[1], "200");
  EXPECT_EQ(cmd.args[2], "100");
  EXPECT_EQ(cmd.args[3], "0.5");

  auto gain = parseLine("BOARD:GAIN 1 , 3");
  ASSERT_EQ(gain.args.size(), 2u);
  EXPECT_EQ(gain.args[0], "1");
  EXPECT_EQ(gain.args[1], "3");
}

TEST(ProtocolTest, ParseLineUnknownCommands)
{
  EXPECT_EQ(parseLine("GARBAGE").command, Command::UNKNOWN);
  EXPECT_EQ(parseLine("").command, Command::UNKNOWN);
  EXPECT_EQ(parseLine("MEASURE").command, Command::UNKNOWN);
}

TEST(ProtocolTest, GetArgIntParsesValidValues)
{
  ParsedCommand cmd;
  cmd.args = {"42", "-7", "0"};

  int value = 0;
  EXPECT_TRUE(cmd.getArgInt(0, value));
  EXPECT_EQ(value, 42);

  EXPECT_TRUE(cmd.getArgInt(1, value));
  EXPECT_EQ(value, -7);

  EXPECT_TRUE(cmd.getArgInt(2, value));
  EXPECT_EQ(value, 0);
}

TEST(ProtocolTest, GetArgIntRejectsInvalidValues)
{
  ParsedCommand cmd;
  cmd.args = {"abc", "12x", "", "99999999999999999999"};

  int value = -1;
  for (size_t i = 0; i < cmd.args.size(); ++i)
    EXPECT_FALSE(cmd.getArgInt(i, value)) << "arg " << i;

  EXPECT_FALSE(cmd.getArgInt(10, value)); // out of range index
}

TEST(ProtocolTest, GetArgFloatParsesValidValues)
{
  ParsedCommand cmd;
  cmd.args = {"3.14", "-1.5e2", "0.5"};

  float value = 0.0f;
  EXPECT_TRUE(cmd.getArgFloat(0, value));
  EXPECT_NEAR(value, 3.14f, 1e-6f);

  EXPECT_TRUE(cmd.getArgFloat(1, value));
  EXPECT_NEAR(value, -150.0f, 1e-4f);

  EXPECT_TRUE(cmd.getArgFloat(2, value));
  EXPECT_FLOAT_EQ(value, 0.5f);
}

TEST(ProtocolTest, GetArgFloatRejectsInvalidValues)
{
  ParsedCommand cmd;
  cmd.args = {"xyz", "1.2.3", "3f"};

  float value = -1.0f;
  for (size_t i = 0; i < cmd.args.size(); ++i)
    EXPECT_FALSE(cmd.getArgFloat(i, value)) << "arg " << i;

  EXPECT_FALSE(cmd.getArgFloat(10, value)); // out of range index
}

TEST(ProtocolTest, GetArgFloatRejectsNonFiniteValues)
{
  ParsedCommand cmd;
  cmd.args = {"nan", "NaN", "inf", "-inf", "infinity", "1e9999", "-1e9999"};

  float value = -1.0f;
  for (size_t i = 0; i < cmd.args.size(); ++i)
    EXPECT_FALSE(cmd.getArgFloat(i, value)) << "arg " << i << ": " << cmd.args[i];
}

TEST(ProtocolTest, GetArgStringReturnsArgumentOrFails)
{
  ParsedCommand cmd;
  cmd.args = {"hello"};

  std::string value;
  EXPECT_TRUE(cmd.getArgString(0, value));
  EXPECT_EQ(value, "hello");

  EXPECT_FALSE(cmd.getArgString(1, value));
}

TEST(ProtocolTest, TrimRemovesSurroundingWhitespace)
{
  EXPECT_EQ(trim("  hello  "), "hello");
  EXPECT_EQ(trim("\thello\t"), "hello");
  EXPECT_EQ(trim("hello"), "hello");
  EXPECT_EQ(trim(""), "");
  EXPECT_EQ(trim("   "), "");
  EXPECT_EQ(trim(" a b "), "a b");
}

TEST(ProtocolTest, BuildOkResponseFormatsCorrectly)
{
  EXPECT_EQ(buildOkResponse(), "OK\n");
  EXPECT_EQ(buildOkResponse("v2.2.0"), "OK v2.2.0\n");
}

TEST(ProtocolTest, BuildErrorResponseFormatsCorrectly)
{
  EXPECT_EQ(buildErrorResponse(ResponseStatus::ERR_PARAM), "ERR_PARAM\n");
  EXPECT_EQ(buildErrorResponse(ResponseStatus::ERR_PARAM, "bad arg"),
            "ERR_PARAM: bad arg\n");
}

TEST(ProtocolTest, StatusToStringCoversStatuses)
{
  EXPECT_STREQ(statusToString(ResponseStatus::OK), "OK");
  EXPECT_STREQ(statusToString(ResponseStatus::ERR_SYNTAX), "ERR_SYNTAX");
  EXPECT_STREQ(statusToString(ResponseStatus::ERR_HARDWARE), "ERR_HARDWARE");
  EXPECT_STREQ(statusToString(ResponseStatus::ERR_BUSY), "ERR_BUSY");
}

TEST(ProtocolTest, BuildSpectrumResponseHeaderMatchesPayload)
{
  std::vector<SpectrumPoint> pts = {
      {100.0f, 1e-3f, 0.5f}, {100.244141f, 2.5e-4f, -1.25f}};

  std::string resp = buildSpectrumResponse(pts);

  std::istringstream header(resp.substr(0, resp.find('\n')));
  std::string okToken, dataToken;
  size_t count = 0;
  size_t bytes = 0;
  header >> okToken >> dataToken >> count >> bytes;

  EXPECT_EQ(okToken, "OK");
  EXPECT_EQ(dataToken, "DATA");
  EXPECT_EQ(count, pts.size());
  EXPECT_EQ(bytes, resp.size() - resp.find('\n') - 1);
}

TEST(ProtocolTest, BuildSpectrumResponsePayloadFormat)
{
  std::vector<SpectrumPoint> pts = {{100.0f, 1e-3f, 0.5f}};
  std::string resp = buildSpectrumResponse(pts);

  std::string payload = resp.substr(resp.find('\n') + 1);
  EXPECT_EQ(payload, "100.000,1.000000e-03,0.500000\n");
}

TEST(ProtocolTest, ServerConfigConstants)
{
  EXPECT_EQ(ServerConfig::DEFAULT_PORT, 5025);
  EXPECT_EQ(ServerConfig::MIN_DECIMATION, 16);
  EXPECT_EQ(ServerConfig::MAX_DECIMATION, 1024);
}

TEST(ProtocolTest, VersionInfoToString)
{
  EXPECT_EQ(VersionInfo::toString(), "2.2.0");
}
