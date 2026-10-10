#include "BoardProtocol.h"

#include "IElectronicBoard.h"

BoardProtocol::BoardProtocol(SendFn send, ReadLineFn readLine, std::string& lastError)
    : m_send(std::move(send))
    , m_readLine(std::move(readLine))
    , m_lastError(lastError)
{
}

bool BoardProtocol::parseGainStatus(const std::string& statusText,
                                    GainSetting gainsOut[],
                                    bool validOut[],
                                    uint8_t numChannels)
{
  for (uint8_t i = 0; i < numChannels; ++i)
  {
    validOut[i] = false;
  }

  const size_t gainsPos = statusText.find("Gains:");
  if (gainsPos == std::string::npos)
  {
    return false;
  }

  // Line based scan of the section; only "IN<n>: x<label>" lines count
  size_t pos = gainsPos + 6;
  while (pos < statusText.size())
  {
    size_t lineEnd = statusText.find('\n', pos);
    if (lineEnd == std::string::npos)
    {
      lineEnd = statusText.size();
    }

    size_t begin = pos;
    size_t end = lineEnd;
    while (end > begin && (statusText[end - 1] == '\r' || statusText[end - 1] == ' '))
    {
      --end;
    }
    while (begin < end && (statusText[begin] == ' ' || statusText[begin] == '\t'))
    {
      ++begin;
    }

    if (end - begin >= 4 && statusText[begin] == 'I' && statusText[begin + 1] == 'N' &&
        statusText[begin + 2] >= '1' && statusText[begin + 2] <= '4' &&
        statusText[begin + 3] == ':')
    {
      const uint8_t channel = static_cast<uint8_t>(statusText[begin + 2] - '1');
      if (channel < numChannels)
      {
        const size_t xPos = statusText.find('x', begin + 4);
        if (xPos != std::string::npos && xPos < end)
        {
          GainSetting setting;
          if (IElectronicBoard::gainFromLabel(statusText.substr(xPos + 1, end - xPos - 1),
                                              setting))
          {
            gainsOut[channel] = setting;
            validOut[channel] = true;
          }
        }
      }
    }

    pos = lineEnd + 1;
  }
  return true;
}

bool BoardProtocol::parseRoutingStatus(const std::string& statusText,
                                       uint8_t routingOut[],
                                       bool validOut[],
                                       uint8_t numChannels)
{
  for (uint8_t i = 0; i < numChannels; ++i)
  {
    validOut[i] = false;
  }

  const size_t routingPos = statusText.find("Routing:");
  if (routingPos == std::string::npos)
  {
    return false;
  }

  size_t sectionEnd = statusText.find("Gains:", routingPos);
  if (sectionEnd == std::string::npos)
  {
    sectionEnd = statusText.size();
  }

  // One "OUT<n> <- IN<m>" or "OUT<n> <- X" line per output
  size_t pos = routingPos + 8;
  while (pos < sectionEnd)
  {
    size_t lineEnd = statusText.find('\n', pos);
    if (lineEnd == std::string::npos || lineEnd > sectionEnd)
    {
      lineEnd = sectionEnd;
    }

    size_t begin = pos;
    while (begin < lineEnd && (statusText[begin] == ' ' || statusText[begin] == '\t'))
    {
      ++begin;
    }

    if (lineEnd - begin >= 4 && statusText.compare(begin, 3, "OUT") == 0 &&
        statusText[begin + 3] >= '1' && statusText[begin + 3] <= '4')
    {
      const uint8_t output = static_cast<uint8_t>(statusText[begin + 3] - '1');
      const size_t arrowPos = statusText.find("<-", begin + 4);
      if (output < numChannels && arrowPos != std::string::npos && arrowPos < lineEnd)
      {
        size_t target = arrowPos + 2;
        while (target < lineEnd && statusText[target] == ' ')
        {
          ++target;
        }

        if (target < lineEnd && statusText[target] == 'X')
        {
          routingOut[output] = IElectronicBoard::DISCONNECTED;
          validOut[output] = true;
        }
        else if (lineEnd - target >= 3 && statusText.compare(target, 2, "IN") == 0 &&
                 statusText[target + 2] >= '1' && statusText[target + 2] <= '4')
        {
          const uint8_t input = static_cast<uint8_t>(statusText[target + 2] - '1');
          if (input < numChannels)
          {
            routingOut[output] = input;
            validOut[output] = true;
          }
        }
      }
    }

    pos = lineEnd + 1;
  }
  return true;
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
