#pragma once

#include <cstddef>
#include <string>

/**
 * @brief Accumulates raw chunks and extracts newline-terminated lines.
 *
 * Portable line framing helper shared by TCPServer and ElectronicBoardUART.
 * A line is complete when it contains '\n'; a trailing '\r' is stripped.
 * Overflow is reported only when the buffered content exceeds the configured
 * limit while containing no line terminator. A chunk that adds a terminator
 * is always accepted; callers must pop pending lines before appending again,
 * so the buffer is bounded by the limit plus one chunk.
 */
class LineBuffer
{
public:
  enum class AppendResult
  {
    Ok,
    Overflow
  };

  explicit LineBuffer(size_t maxLength)
      : m_maxLength(maxLength)
  {
  }

  AppendResult append(const char* data, size_t length)
  {
    m_buffer.append(data, length);
    if (m_buffer.size() > m_maxLength && m_buffer.find('\n') == std::string::npos)
    {
      return AppendResult::Overflow;
    }
    return AppendResult::Ok;
  }

  bool popLine(std::string& line)
  {
    size_t newlinePos = m_buffer.find('\n');
    if (newlinePos == std::string::npos)
    {
      return false;
    }

    line = m_buffer.substr(0, newlinePos);
    m_buffer.erase(0, newlinePos + 1);

    if (!line.empty() && line.back() == '\r')
    {
      line.pop_back();
    }
    return true;
  }

  void clear()
  {
    m_buffer.clear();
  }

  size_t size() const
  {
    return m_buffer.size();
  }

  bool empty() const
  {
    return m_buffer.empty();
  }

private:
  std::string m_buffer;
  size_t m_maxLength;
};
