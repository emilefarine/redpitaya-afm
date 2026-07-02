#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace AFM
{

/**
 * @brief Server configuration constants
 */
struct ServerConfig
{
  static constexpr uint16_t DEFAULT_PORT = 5025; // IANA-assigned SCPI-raw port
  static constexpr int MAX_CLIENTS = 1;
  static constexpr int RECV_BUFFER_SIZE = 4096;
  static constexpr int SEND_BUFFER_SIZE = 65536;
  static constexpr int IDLE_TIMEOUT_SEC = 3600; // 1 hour idle before disconnect
  static constexpr int SELECT_POLL_MS = 1000;   // Check for shutdown every 1 second
  static constexpr int SOCKET_TIMEOUT_SEC = 30;

  // FPGA constraint: decimation must be power-of-two in [16, 1024]
  static constexpr uint16_t MIN_DECIMATION = 16;
  static constexpr uint16_t MAX_DECIMATION = 1024;
};

// clang-format off
/**
 * @brief SCPI command definitions.
 *
 * X(enum_name, scpi_keyword), maps an enum value to its SCPI wire-format
 * keyword.  The parser strips a trailing '?' (marking a query) before
 * matching; the isQuery flag in ParsedCommand distinguishes set from query.
 */
#define AFM_COMMAND_LIST \
    X(IDN,                  "*IDN")                   /* IEEE 488.2: Identify */ \
    X(RST,                  "*RST")                   /* IEEE 488.2: Reset */ \
    X(OPC,                  "*OPC")                   /* IEEE 488.2: Operation complete */ \
    X(SYST_PING,            "SYSTEM:PING")            /* Test connection */ \
    X(SYST_VERSION,         "SYSTEM:VERSION")         /* Get server version */ \
    X(SYST_STATUS,          "SYSTEM:STATUS")          /* Get system status */ \
    X(SYST_INIT,            "SYSTEM:INIT")            /* Initialize all hardware */ \
    X(SYST_DEINIT,          "SYSTEM:DEINIT")          /* Deinitialize hardware */ \
    X(SYST_SHUTDOWN,        "SYSTEM:SHUTDOWN")        /* Shutdown server */ \
    X(BOARD_MUX_ROUTE,      "BOARD:MUX:ROUTE")        /* Route input to output */ \
    X(BOARD_MUX_DISCONNECT, "BOARD:MUX:DISCONNECT")   /* Disconnect MUX output */ \
    X(BOARD_GAIN,           "BOARD:GAIN")             /* Set channel gain */ \
    X(BOARD_RESET,          "BOARD:RESET")            /* Reset electronic board */ \
    X(BOARD_STATUS,         "BOARD:STATUS")           /* Get electronic board status */ \
    X(MEAS_SINC,            "MEASURE:SINC")           /* Broadband sinc measurement + FFT */ \
    X(MEAS_SWEEP,           "MEASURE:SWEEP")          /* Frequency sweep measurement */ \
    X(CAL_RUN,              "CALIBRATE:RUN")           /* Run loopback calibration */   \
    X(CAL_STATUS,           "CALIBRATE:STATUS")        /* Query calibration state */    \
    X(CAL_CLEAR,            "CALIBRATE:CLEAR")         /* Clear stored calibration */
// clang-format on

/**
 * @brief Command identifiers (auto-generated from AFM_COMMAND_LIST)
 */
enum class Command : uint8_t
{
#define X(name, scpi) name,
  AFM_COMMAND_LIST
#undef X
      UNKNOWN
};

/**
 * @brief Response status codes
 */
enum class ResponseStatus : uint8_t
{
  OK,           // Command succeeded
  ERR_SYNTAX,   // Invalid command syntax
  ERR_PARAM,    // Invalid parameter value
  ERR_HARDWARE, // Hardware error
  ERR_BUSY,     // System busy (measurement in progress)
  ERR_NOT_INIT, // Hardware not initialized
  ERR_TIMEOUT,  // Operation timed out
  ERR_UNKNOWN   // Unknown error
};

/**
 * @brief Parse a SCPI command keyword into Command enum (case-insensitive).
 *
 * The caller must strip any trailing '?' before calling this function.
 */
inline Command parseCommand(const std::string& cmdStr)
{
  std::string upper = cmdStr;
  std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

#define X(name, scpi)                                                                              \
  if (upper == scpi)                                                                               \
    return Command::name;
  AFM_COMMAND_LIST
#undef X
  return Command::UNKNOWN;
}

/**
 * @brief Get SCPI command string for a Command enum value
 */
inline const char* commandToString(Command cmd)
{
  switch (cmd)
  {
#define X(name, scpi)                                                                              \
  case Command::name:                                                                              \
    return scpi;
    AFM_COMMAND_LIST
#undef X
  default:
    return "UNKNOWN";
  }
}

/**
 * @brief Get status string
 */
inline const char* statusToString(ResponseStatus status)
{
  switch (status)
  {
  case ResponseStatus::OK:
    return "OK";
  case ResponseStatus::ERR_SYNTAX:
    return "ERR_SYNTAX";
  case ResponseStatus::ERR_PARAM:
    return "ERR_PARAM";
  case ResponseStatus::ERR_HARDWARE:
    return "ERR_HARDWARE";
  case ResponseStatus::ERR_BUSY:
    return "ERR_BUSY";
  case ResponseStatus::ERR_NOT_INIT:
    return "ERR_NOT_INIT";
  case ResponseStatus::ERR_TIMEOUT:
    return "ERR_TIMEOUT";
  default:
    return "ERR_UNKNOWN";
  }
}

/**
 * @brief Parsed SCPI command structure
 */
struct ParsedCommand
{
  Command command;
  bool isQuery;                  ///< True when the command keyword ended with '?'
  std::vector<std::string> args; ///< Comma-separated arguments
  std::string rawLine;

  ParsedCommand()
      : command(Command::UNKNOWN)
      , isQuery(false)
  {
  }

  /**
   * @brief Get argument as integer
   */
  bool getArgInt(size_t index, int& value) const
  {
    if (index >= args.size())
      return false;
    const std::string& s = args[index];
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    return ec == std::errc{} && ptr == s.data() + s.size();
  }

  /**
   * @brief Get argument as float
   * Note: std::from_chars for float may not be available on all compilers,
   * so we fall back to strtof which is still faster than exceptions.
   */
  bool getArgFloat(size_t index, float& value) const
  {
    if (index >= args.size())
    {
      return false;
    }
    const std::string& s = args[index];
    char* end = nullptr;
    value = std::strtof(s.c_str(), &end);
    return end == s.c_str() + s.size();
  }

  /**
   * @brief Get argument as string
   */
  bool getArgString(size_t index, std::string& value) const
  {
    if (index >= args.size())
    {
      return false;
    }
    value = args[index];
    return true;
  }
};

/**
 * @brief Trim leading and trailing whitespace
 */
inline std::string trim(const std::string& s)
{
  size_t start = s.find_first_not_of(" \t");
  if (start == std::string::npos)
  {
    return "";
  }
  size_t end = s.find_last_not_of(" \t");
  return s.substr(start, end - start + 1);
}

/**
 * @brief Parse a SCPI command line into a ParsedCommand structure.
 *
 * SCPI format:  KEYWORD[:SUBKEY]...[?] [arg1,arg2,...]\n
 * - Commands are case-insensitive.
 * - Trailing '?' on the keyword marks a query.
 * - Arguments are comma-separated; whitespace around commas is ignored.
 */
inline ParsedCommand parseLine(const std::string& line)
{
  ParsedCommand result;
  result.rawLine = line;

  // Split into keyword and argument portion at first space
  std::string keyword;
  std::string argPortion;

  size_t spacePos = line.find(' ');
  if (spacePos != std::string::npos)
  {
    keyword = line.substr(0, spacePos);
    argPortion = line.substr(spacePos + 1);
  }
  else
  {
    keyword = line;
  }

  // Detect and strip trailing '?' (query indicator)
  if (!keyword.empty() && keyword.back() == '?')
  {
    result.isQuery = true;
    keyword.pop_back();
  }

  result.command = parseCommand(keyword);

  // Parse comma-separated arguments, trimming whitespace around each
  if (!argPortion.empty())
  {
    std::istringstream iss(argPortion);
    std::string token;
    while (std::getline(iss, token, ','))
    {
      std::string trimmed = trim(token);
      if (!trimmed.empty())
      {
        result.args.push_back(trimmed);
      }
    }
  }

  return result;
}

inline std::string buildOkResponse(const std::string& data = "")
{
  if (data.empty())
  {
    return "OK\n";
  }
  return "OK " + data + "\n";
}

inline std::string buildErrorResponse(ResponseStatus status, const std::string& message = "")
{
  std::string response = statusToString(status);
  if (!message.empty())
  {
    response += ": " + message;
  }
  return response + "\n";
}

/**
 * @brief Spectrum data point for MEASURE responses
 */
struct SpectrumPoint
{
  float freqKHz;
  float magnitude;
  float phaseRad;
};

/**
 * @brief Build a multi-line spectrum response.
 *
 * Format:
 *   OK DATA <N> <BYTES>\n
 *   freq_kHz,magnitude,phase_rad\n
 *   ...  (N lines)
 *
 * <N>     : number of data rows that follow.
 * <BYTES> : exact number of bytes of the payload that follows the header
 *           line's newline (all N rows, including separators and the trailing
 *           '\n' of each row). A client can read the header line, then read
 *           exactly <BYTES> bytes instead of scanning for newlines.
 */
inline std::string buildSpectrumResponse(const std::vector<SpectrumPoint>& spectrum)
{
  std::ostringstream body;
  for (const auto& pt : spectrum)
  {
    body << std::fixed << std::setprecision(3) << pt.freqKHz << "," << std::scientific
         << std::setprecision(6) << pt.magnitude << "," << std::fixed << std::setprecision(6)
         << pt.phaseRad << "\n";
  }

  std::string payload = body.str();
  return "OK DATA " + std::to_string(spectrum.size()) + " " + std::to_string(payload.size()) +
         "\n" + payload;
}

/**
 * @brief Server version information
 */
struct VersionInfo
{
  static constexpr int MAJOR = 2;
  static constexpr int MINOR = 2;
  static constexpr int PATCH = 0;

  static std::string toString()
  {
    return std::to_string(MAJOR) + "." + std::to_string(MINOR) + "." + std::to_string(PATCH);
  }
};

} // namespace AFM
