#include "BoardProtocol.h"

BoardProtocol::BoardProtocol(SendFn send, ReadLineFn readLine, std::string& lastError)
    : m_send(std::move(send))
    , m_readLine(std::move(readLine))
    , m_lastError(lastError)
{
}

bool BoardProtocol::sendCommand(const std::string& command,
                                std::string* response,
                                uint32_t timeoutMs)
{
  if (!m_send(command + "\n"))
  {
    return false;
  }

  const bool isStatusCmd = (command == "STATUS");
  std::string statusAccum;
  bool statusHasData = false;

  std::string line;
  while (m_readLine(line, timeoutMs))
  {
    if (line.find("OK") == 0)
    {
      if (response)
      {
        *response = (isStatusCmd && statusHasData) ? statusAccum : line;
      }
      return true;
    }

    if (line.find("ERR:") == 0)
    {
      m_lastError = line.substr(4);
      return false;
    }

    if (isStatusCmd)
    {
      if (statusHasData)
      {
        statusAccum += "\n" + line;
      }
      else
      {
        statusAccum = line;
        statusHasData = true;
      }
      timeoutMs = 200;
      continue;
    }

    timeoutMs = 500;
  }

  if (m_lastError.empty())
  {
    m_lastError = "No response from board";
  }
  return false;
}

const std::string& BoardProtocol::getLastError() const
{
  return m_lastError;
}
