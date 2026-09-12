#include "BoardProtocol.h"

#include "IElectronicBoard.h"

namespace
{

/**
 * @brief Map a gain label printed by the board ("1/8".."16") to GainSetting
 */
bool gainLabelToSetting(const std::string& label, GainSetting& settingOut)
{
  struct LabelEntry
  {
    const char* text;
    GainSetting setting;
  };
  static const LabelEntry labels[] = {
      {"1/8", GainSetting::GAIN_1_8}, {"1/4", GainSetting::GAIN_1_4},
      {"1/2", GainSetting::GAIN_1_2}, {"1", GainSetting::GAIN_1},
      {"2", GainSetting::GAIN_2},     {"4", GainSetting::GAIN_4},
      {"8", GainSetting::GAIN_8},     {"16", GainSetting::GAIN_16},
  };
  for (const auto& entry : labels)
  {
    if (label == entry.text)
    {
      settingOut = entry.setting;
      return true;
    }
  }
  return false;
}

} // namespace

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
          if (gainLabelToSetting(statusText.substr(xPos + 1, end - xPos - 1), setting))
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
