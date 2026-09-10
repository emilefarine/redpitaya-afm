#include "Protocol.h"
#include "ServerOptions.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace AFM;

namespace
{

constexpr const char* c_DefaultBitstream = "/root/master/tm/fpga/red_pitaya_top.bit.bin";

ServerOptions parseOk(const std::vector<std::string>& args)
{
  ServerOptions options;
  options.bitstreamPath = c_DefaultBitstream;

  std::string error;
  EXPECT_EQ(parseServerOptions(args, options, error), OptionsParseResult::Ok) << error;
  return options;
}

} // namespace

TEST(ServerOptionsTest, DefaultsArePreserved)
{
  ServerOptions options = parseOk({});
  EXPECT_EQ(options.port, ServerConfig::DEFAULT_PORT);
  EXPECT_EQ(options.bitstreamPath, c_DefaultBitstream);
}

TEST(ServerOptionsTest, ParsesShortAndLongPort)
{
  EXPECT_EQ(parseOk({"-p", "1234"}).port, 1234);
  EXPECT_EQ(parseOk({"--port", "1234"}).port, 1234);
}

TEST(ServerOptionsTest, ParsesLegacyBarePort)
{
  EXPECT_EQ(parseOk({"6000"}).port, 6000);
}

TEST(ServerOptionsTest, ParsesBitstreamPath)
{
  EXPECT_EQ(parseOk({"-b", "/tmp/fpga.bit.bin"}).bitstreamPath, "/tmp/fpga.bit.bin");
  EXPECT_EQ(parseOk({"--bitstream", "/tmp/fpga.bit.bin"}).bitstreamPath,
            "/tmp/fpga.bit.bin");
}

TEST(ServerOptionsTest, RejectsInvalidPorts)
{
  const std::vector<std::vector<std::string>> cases = {
      {"-p", "0"}, {"-p", "65536"}, {"-p", "-1"}, {"-p", "abc"}};

  for (const auto& args : cases)
  {
    ServerOptions options;
    std::string error;
    EXPECT_EQ(parseServerOptions(args, options, error), OptionsParseResult::Error) << error;
    EXPECT_FALSE(error.empty());
  }
}

TEST(ServerOptionsTest, RejectsMissingValues)
{
  ServerOptions options;
  std::string error;

  EXPECT_EQ(parseServerOptions({"-p"}, options, error), OptionsParseResult::Error);
  EXPECT_EQ(parseServerOptions({"--port"}, options, error), OptionsParseResult::Error);
  EXPECT_EQ(parseServerOptions({"-b"}, options, error), OptionsParseResult::Error);
  EXPECT_EQ(parseServerOptions({"--bitstream"}, options, error), OptionsParseResult::Error);
}

TEST(ServerOptionsTest, RejectsUnknownOption)
{
  ServerOptions options;
  std::string error;

  EXPECT_EQ(parseServerOptions({"--bogus"}, options, error), OptionsParseResult::Error);
  EXPECT_NE(error.find("--bogus"), std::string::npos);
}

TEST(ServerOptionsTest, DetectsHelpAndVersion)
{
  ServerOptions options;
  std::string error;

  EXPECT_EQ(parseServerOptions({"-h"}, options, error), OptionsParseResult::Help);
  EXPECT_EQ(parseServerOptions({"--help"}, options, error), OptionsParseResult::Help);
  EXPECT_EQ(parseServerOptions({"-v"}, options, error), OptionsParseResult::Version);
  EXPECT_EQ(parseServerOptions({"--version"}, options, error), OptionsParseResult::Version);
}

TEST(ServerOptionsTest, LaterOptionsOverrideEarlierOnes)
{
  ServerOptions options = parseOk({"-p", "1111", "-p", "2222", "-b", "/a", "-b", "/b"});
  EXPECT_EQ(options.port, 2222);
  EXPECT_EQ(options.bitstreamPath, "/b");
}

TEST(ServerOptionsTest, BuildFpgautilArgs)
{
  std::vector<std::string> args = buildFpgautilArgs("/root/fpga.bit.bin");
  ASSERT_EQ(args.size(), 2u);
  EXPECT_EQ(args[0], "-b");
  EXPECT_EQ(args[1], "/root/fpga.bit.bin");
}
