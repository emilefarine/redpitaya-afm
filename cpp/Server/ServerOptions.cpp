#include "ServerOptions.h"

#include <charconv>

namespace AFM
{

namespace
{
bool parsePortNumber(const std::string& text, int& value)
{
  if (text.empty())
  {
    return false;
  }
  auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  return ec == std::errc{} && ptr == text.data() + text.size();
}
} // namespace

OptionsParseResult parseServerOptions(const std::vector<std::string>& args,
                                      ServerOptions& options,
                                      std::string& errorMessage)
{
  for (size_t i = 0; i < args.size(); ++i)
  {
    const std::string& arg = args[i];

    if (arg == "-h" || arg == "--help")
    {
      return OptionsParseResult::Help;
    }
    if (arg == "-v" || arg == "--version")
    {
      return OptionsParseResult::Version;
    }
    if (arg == "-p" || arg == "--port")
    {
      if (i + 1 >= args.size())
      {
        errorMessage = "--port requires a value";
        return OptionsParseResult::Error;
      }
      const std::string& value = args[++i];
      int portVal = 0;
      if (!parsePortNumber(value, portVal) || portVal <= 0 || portVal > 65535)
      {
        errorMessage = "Invalid port number: " + value;
        return OptionsParseResult::Error;
      }
      options.port = static_cast<uint16_t>(portVal);
      continue;
    }
    if (arg == "-b" || arg == "--bitstream")
    {
      if (i + 1 >= args.size())
      {
        errorMessage = "--bitstream requires a path";
        return OptionsParseResult::Error;
      }
      options.bitstreamPath = args[++i];
      continue;
    }
    if (arg == "--no-board")
    {
      options.noBoard = true;
      continue;
    }

    // Legacy format: bare port number
    int portVal = 0;
    if (parsePortNumber(arg, portVal) && portVal > 0 && portVal <= 65535)
    {
      options.port = static_cast<uint16_t>(portVal);
      continue;
    }

    errorMessage = "Unknown option: " + arg;
    return OptionsParseResult::Error;
  }

  return OptionsParseResult::Ok;
}

std::vector<std::string> buildFpgautilArgs(const std::string& bitstreamPath)
{
  return {"-b", bitstreamPath};
}

} // namespace AFM
