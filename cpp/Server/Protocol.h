#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
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

  // ADC sample rate of the Red Pitaya STEMlab 125-14
  static constexpr double ADC_SAMPLE_RATE_HZ = 125000000.0;

  // Upper bound on the number of points in a MEASURE:SWEEP
  static constexpr size_t MAX_SWEEP_POINTS = 4096;
};

// clang-format off
/**
 * @brief SCPI command definitions.
 *
 * X(enum_name, scpi_keyword, max_args), maps an enum value to its SCPI
 * wire-format keyword and the largest argument count it accepts. The parser
 * strips a trailing '?' (marking a query) before matching; the isQuery flag
 * in ParsedCommand distinguishes set from query.
 *
 * ADC input protection (see InputSafety.h):
 *   The server predicts the peak at the +/-1 V ADC input from the excitation
 *   amplitude and the gains of the excitation and return channels.
 *
 *   BOARD:ADC:LOOP AUTO (default)     excitation on IN1, return = input routed to OUT1
 *   BOARD:ADC:LOOP <excite>,<return>  channels named by hand
 *   BOARD:ADC:LOOP OFF                monitor disabled
 *   BOARD:ADC:PROTECT ON (default)    MEASURE predicted above full scale -> ERR_PARAM
 *   BOARD:ADC:PROTECT OFF             override, the measurement runs
 *   BOARD:ADC:SAMPLEGAIN <factor>     expected return / excitation ratio of the AFM (default 1)
 *
 *   Other BOARD replies get a "WARN: ..." suffix instead of being rejected.
 *   SYSTEM:STATUS reports LOOP (effective pair; return "X" = OUT1 open,
 *   "?" = unknown), OVERDRIVE, SAT, PROTECT and SAMPLEGAIN. Unknown board
 *   state counts as the worst case.
 */
#define AFM_COMMAND_LIST \
    X(IDN,                  "*IDN",                 0) /* IEEE 488.2: Identify */ \
    X(RST,                  "*RST",                 0) /* IEEE 488.2: Reset */ \
    X(OPC,                  "*OPC",                 0) /* IEEE 488.2: Operation complete */ \
    X(SYST_PING,            "SYSTEM:PING",          0) /* Test connection */ \
    X(SYST_VERSION,         "SYSTEM:VERSION",       0) /* Get server version */ \
    X(SYST_STATUS,          "SYSTEM:STATUS",        0) /* Get system status */ \
    X(SYST_INIT,            "SYSTEM:INIT",          0) /* Initialize all hardware */ \
    X(SYST_DEINIT,          "SYSTEM:DEINIT",        0) /* Deinitialize hardware */ \
    X(SYST_MODE,            "SYSTEM:MODE",          0) /* Get operating mode */ \
    X(SYST_SHUTDOWN,        "SYSTEM:SHUTDOWN",      0) /* Shutdown server */ \
    X(BOARD_MUX_ROUTE,      "BOARD:MUX:ROUTE",      2) /* Route input to output */ \
    X(BOARD_MUX_DISCONNECT, "BOARD:MUX:DISCONNECT", 1) /* Disconnect MUX output */ \
    X(BOARD_GAIN,           "BOARD:GAIN",           2) /* Set channel gain */ \
    X(BOARD_ADC_LOOP,       "BOARD:ADC:LOOP",       2) /* ADC overdrive monitor loop */ \
    X(BOARD_ADC_PROTECT,    "BOARD:ADC:PROTECT",    1) /* Refuse or allow ADC overdrive */ \
    X(BOARD_ADC_SAMPLEGAIN, "BOARD:ADC:SAMPLEGAIN", 1) /* Expected AFM return ratio */ \
    X(BOARD_RESET,          "BOARD:RESET",          0) /* Reset electronic board */ \
    X(BOARD_STATUS,         "BOARD:STATUS",         0) /* Get electronic board status */ \
    X(MEAS_SINC,            "MEASURE:SINC",         5) /* Broadband sinc measurement + FFT */ \
    X(MEAS_SWEEP,           "MEASURE:SWEEP",        5) /* Frequency sweep measurement */
// clang-format on

/**
 * @brief Command identifiers (auto-generated from AFM_COMMAND_LIST)
 */
enum class Command : uint8_t
{
#define X(name, scpi, maxArgs) name,
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
  ERR_NOT_INIT, // Hardware not initialized
  ERR_TIMEOUT,  // Operation timed out
  ERR_ABORTED,  // Cancelled: client gone or server stopping
  ERR_UNKNOWN   // Unknown error
};

/**
 * @brief Operating mode of the server
 */
enum class OperatingMode : uint8_t
{
  FULL,   // Red Pitaya with optional electronic board
  RP_ONLY // Red Pitaya only, electronic board never probed
};

inline const char* operatingModeToString(OperatingMode mode)
{
  switch (mode)
  {
  case OperatingMode::RP_ONLY:
    return "RP_ONLY";
  case OperatingMode::FULL:
  default:
    return "FULL";
  }
}

/**
 * @brief Parse a SCPI command keyword into Command enum (case-insensitive).
 *
 * The caller must strip any trailing '?' before calling this function.
 */
inline Command parseCommand(const std::string& cmdStr)
{
  std::string upper = cmdStr;
  std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

#define X(name, scpi, maxArgs)                                                                     \
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
#define X(name, scpi, maxArgs)                                                                     \
  case Command::name:                                                                              \
    return scpi;
    AFM_COMMAND_LIST
#undef X
  default:
    return "UNKNOWN";
  }
}

/**
 * @brief Largest number of arguments a command accepts
 */
inline size_t maxArgCount(Command cmd)
{
  switch (cmd)
  {
#define X(name, scpi, maxArgs)                                                                     \
  case Command::name:                                                                              \
    return maxArgs;
    AFM_COMMAND_LIST
#undef X
  default:
    return 0;
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
  case ResponseStatus::ERR_NOT_INIT:
    return "ERR_NOT_INIT";
  case ResponseStatus::ERR_TIMEOUT:
    return "ERR_TIMEOUT";
  case ResponseStatus::ERR_ABORTED:
    return "ERR_ABORTED";
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
   * @brief Get argument as a finite decimal float
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
    // strtof also reads hex floats such as 0x1p7
    if (s.empty() || s.find_first_of("xX") != std::string::npos)
    {
      return false;
    }
    char* end = nullptr;
    value = std::strtof(s.c_str(), &end);
    return end == s.c_str() + s.size() && std::isfinite(value);
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
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos)
  {
    return "";
  }
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

/**
 * @brief Parse a SCPI command line into a ParsedCommand structure.
 *
 * SCPI format:  KEYWORD[:SUBKEY]...[?] [arg1,arg2,...]\n
 * - Commands are case-insensitive.
 * - Trailing '?' on the keyword marks a query.
 * - The keyword ends at the first space or tab.
 * - Arguments are comma-separated; whitespace around commas is ignored.
 */
inline ParsedCommand parseLine(const std::string& rawLine)
{
  ParsedCommand result;
  result.rawLine = rawLine;
  const std::string line = trim(rawLine);

  std::string keyword;
  std::string argPortion;

  size_t spacePos = line.find_first_of(" \t");
  if (spacePos != std::string::npos)
  {
    keyword = line.substr(0, spacePos);
    argPortion = trim(line.substr(spacePos + 1));
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

  // Parse comma-separated arguments, trimming whitespace around each.
  // Empty tokens are preserved so argument positions never shift.
  if (!argPortion.empty())
  {
    size_t start = 0;
    while (true)
    {
      size_t commaPos = argPortion.find(',', start);
      if (commaPos == std::string::npos)
      {
        result.args.push_back(trim(argPortion.substr(start)));
        break;
      }
      result.args.push_back(trim(argPortion.substr(start, commaPos - start)));
      start = commaPos + 1;
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
 * @brief Build a multi-line text response: "OK LINES <N>" then N lines
 *
 * Carriage returns are dropped so every line ends with a single '\n'.
 */
inline std::string buildTextResponse(const std::string& text)
{
  std::string body;
  size_t count = 0;
  size_t pos = 0;
  while (pos <= text.size())
  {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos)
    {
      end = text.size();
    }
    std::string line = text.substr(pos, end - pos);
    line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
    body += line + "\n";
    ++count;
    pos = end + 1;
  }
  return "OK LINES " + std::to_string(count) + "\n" + body;
}

/**
 * @brief Server version information
 */
struct VersionInfo
{
  static constexpr int MAJOR = 2;
  static constexpr int MINOR = 10;
  static constexpr int PATCH = 0;

  static std::string toString()
  {
    return std::to_string(MAJOR) + "." + std::to_string(MINOR) + "." + std::to_string(PATCH);
  }
};

} // namespace AFM
