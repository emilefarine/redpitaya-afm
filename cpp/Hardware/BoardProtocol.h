#pragma once

#include <cstdint>
#include <functional>
#include <string>

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

  const std::string& getLastError() const;

private:
  SendFn m_send;
  ReadLineFn m_readLine;
  std::string& m_lastError;
};
