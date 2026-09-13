#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "IElectronicBoard.h"

/**
 * @brief Framing and response handling for the LPC1114 electronic board.
 *
 * Portable protocol logic extracted from ElectronicBoardUART:
 * - commands are newline-terminated ASCII, written through an injected callback
 * - the board may echo the command before replying with OK or ERR:<message>
 * - STATUS replies may span several data lines before the final OK
 *
 * The error string is owned by the caller so both the callbacks and the
 * protocol can report into the same storage.
 */
class BoardProtocol
{
public:
  using SendFn = std::function<bool(const std::string&)>;
  using ReadLineFn = std::function<bool(std::string&, uint32_t)>;

  BoardProtocol(SendFn send, ReadLineFn readLine, std::string& lastError);

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

  const std::string& getLastError() const;

private:
  SendFn m_send;
  ReadLineFn m_readLine;
  std::string& m_lastError;
};
