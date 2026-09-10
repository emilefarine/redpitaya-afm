#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace AFM
{

struct ServerOptions
{
  uint16_t port = ServerConfig::DEFAULT_PORT;
  std::string bitstreamPath;
};

enum class OptionsParseResult
{
  Ok,
  Help,
  Version,
  Error
};

/**
 * @brief Parse command line arguments (without the program name).
 *
 * Supported: -p/--port, -b/--bitstream, -h/--help, -v/--version and the
 * legacy bare port number. Returns Help/Version so the caller decides how
 * to report them instead of terminating the process.
 */
OptionsParseResult parseServerOptions(const std::vector<std::string>& args,
                                      ServerOptions& options,
                                      std::string& errorMessage);

/** @brief Arguments passed to fpgautil after the executable path. */
std::vector<std::string> buildFpgautilArgs(const std::string& bitstreamPath);

} // namespace AFM
