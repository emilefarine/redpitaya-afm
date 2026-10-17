#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include "IElectronicBoard.h"

/**
 * @brief Framing and response handling for the LPC1114 electronic board.
 *
 * Portable protocol logic extracted from ElectronicBoardUART:
 * - commands are newline-terminated ASCII, written through an injected callback
 * - the board echoes each command after its "> " prompt, then prints data lines
 *   (STATUS) and OK or ERR:<message>
 * - lines before the echo are left over from earlier commands and are skipped
 *
 * The error string is owned by the caller so both the callbacks and the
 * protocol can report into the same storage.
 */
class BoardProtocol
{
public:
  using SendFn = std::function<bool(const std::string&)>;
  using ReadLineFn = std::function<bool(std::string&, uint32_t)>;
  using NowFn = std::function<std::chrono::steady_clock::time_point()>;

  /** Lines accepted in one reply, skipped ones included (STATUS is about 15) */
  static constexpr size_t MAX_REPLY_LINES = 64;

  BoardProtocol(SendFn send, ReadLineFn readLine, std::string& lastError,
                NowFn now = std::chrono::steady_clock::now);

  /**
   * @brief Send a command and wait for its OK or ERR within timeoutMs in total
   * @param response Receives the data lines between the echo and OK, or "OK"
   */
  bool sendCommand(const std::string& command,
                   std::string* response = nullptr,
                   uint32_t timeoutMs = 1000);

  /**
   * @brief Parse the gain section of the board STATUS text
   *
   * The board prints a "Gains:" section with lines of the form
   * "  IN<n>: x<label>" (label in 1/8, 1/4, 1/2, 1, 2, 4, 8, 16). Parsing is
   * line based and anchored on that section, so routing lines
   * ("OUT1 <- IN2") or unrelated text cannot produce false positives.
   *
   * @param statusText Multi-line STATUS reply from the board
   * @param gainsOut Receives parsed gains (only valid channels are written)
   * @param validOut Set false for every channel, then true for parsed ones
   * @param numChannels Size of the gainsOut/validOut arrays
   * @return true when the "Gains:" section was found (channels may still
   *         be individually invalid)
   */
  static bool parseGainStatus(const std::string& statusText,
                              GainSetting gainsOut[],
                              bool validOut[],
                              uint8_t numChannels);

  /**
   * @brief Parse the "Routing:" section of the board STATUS text
   *
   * Lines are "OUT<n> <- IN<m>" or "OUT<n> <- X". routingOut gets the 0-based
   * input or IElectronicBoard::DISCONNECTED; validOut marks the outputs read.
   *
   * @return true when the section was found
   */
  static bool parseRoutingStatus(const std::string& statusText,
                                 uint8_t routingOut[],
                                 bool validOut[],
                                 uint8_t numChannels);

  const std::string& getLastError() const;

private:
  static bool _isEcho(const std::string& line, const std::string& command);

  SendFn m_send;
  ReadLineFn m_readLine;
  std::string& m_lastError;
  NowFn m_now;
};
