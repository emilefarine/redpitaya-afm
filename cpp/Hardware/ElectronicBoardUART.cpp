/**
 * @file ElectronicBoardUART.cpp
 * @brief Implementation of UART communication with the LPC1114 Electronic Board
 */

#include "ElectronicBoardUART.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

ElectronicBoardUART::ElectronicBoardUART(const std::string& devicePath, uint32_t baudRate)
    : m_devicePath(devicePath)
    , m_baudRate(baudRate)
    , m_fd(-1)
{
}

ElectronicBoardUART::~ElectronicBoardUART()
{
  close();
}

bool ElectronicBoardUART::initialize()
{
  // Open UART device
  m_fd = open(m_devicePath.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (m_fd < 0)
  {
    m_lastError = "Failed to open " + m_devicePath + ": " + std::strerror(errno);
    return false;
  }

  // Configure port settings
  if (!_configurePort())
  {
    ::close(m_fd);
    m_fd = -1;
    return false;
  }

  // Clear any pending data
  tcflush(m_fd, TCIOFLUSH);
  m_recvBuffer.clear();

  // Probe the board: send a command and check for a response.
  std::string probeResponse;
  if (!_sendCommand("STATUS", &probeResponse, 500))
  {
    std::cout << "Electronic Board not responding on " << m_devicePath << std::endl;
    ::close(m_fd);
    m_fd = -1;
    m_lastError = "Board not responding on " + m_devicePath;
    return false;
  }

  std::cout << "Electronic Board UART initialized on " << m_devicePath << " at " << m_baudRate
            << " baud" << std::endl;

  return true;
}

void ElectronicBoardUART::close()
{
  if (m_fd >= 0)
  {
    ::close(m_fd);
    m_fd = -1;
  }
  m_recvBuffer.clear();
}

bool ElectronicBoardUART::isConnected() const
{
  return m_fd >= 0;
}

bool ElectronicBoardUART::setMuxRoute(uint8_t output, uint8_t input)
{
  if (output >= NUM_CHANNELS || input >= NUM_CHANNELS)
  {
    m_lastError = "Invalid channel number (must be 0-3 internally)";
    return false;
  }

  // Translate to 1-based indices for the LPC firmware (connector labels 1-4)
  std::string command = "MUX" + std::to_string(output + 1) + "," + std::to_string(input + 1);
  return _sendCommand(command);
}

bool ElectronicBoardUART::disconnectMux(uint8_t output)
{
  if (output >= NUM_CHANNELS)
  {
    m_lastError = "Invalid output channel (must be 0-3 internally)";
    return false;
  }

  // Translate to 1-based index for the LPC firmware (connector label 1-4)
  std::string command = "MUX" + std::to_string(output + 1) + ",X";
  return _sendCommand(command);
}

bool ElectronicBoardUART::setGain(uint8_t channel, GainSetting gain)
{
  if (channel >= NUM_CHANNELS)
  {
    m_lastError = "Invalid channel number (must be 0-3 internally)";
    return false;
  }

  // Validate against known enum members
  switch (gain)
  {
  case GainSetting::GAIN_1_8:
  case GainSetting::GAIN_1_4:
  case GainSetting::GAIN_1_2:
  case GainSetting::GAIN_1:
  case GainSetting::GAIN_2:
  case GainSetting::GAIN_4:
  case GainSetting::GAIN_8:
  case GainSetting::GAIN_16:
    break; // Valid
  default:
    m_lastError = "Invalid gain setting";
    return false;
  }

  // Translate to 1-based channel index for the LPC firmware (connector label 1-4)
  std::string command =
      "GAIN" + std::to_string(channel + 1) + "," + std::to_string(static_cast<int>(gain));
  return _sendCommand(command);
}

bool ElectronicBoardUART::getStatus(std::string& statusOut)
{
  return _sendCommand("STATUS", &statusOut);
}

bool ElectronicBoardUART::reset()
{
  return _sendCommand("RESET");
}

const char* ElectronicBoardUART::gainToString(GainSetting gain)
{
  switch (gain)
  {
  case GainSetting::GAIN_1_8:
    return "1/8";
  case GainSetting::GAIN_1_4:
    return "1/4";
  case GainSetting::GAIN_1_2:
    return "1/2";
  case GainSetting::GAIN_1:
    return "1";
  case GainSetting::GAIN_2:
    return "2";
  case GainSetting::GAIN_4:
    return "4";
  case GainSetting::GAIN_8:
    return "8";
  case GainSetting::GAIN_16:
    return "16";

  default:
    return "?";
  }
}

const std::string& ElectronicBoardUART::getLastError() const
{
  return m_lastError;
}

bool ElectronicBoardUART::_configurePort()
{
  struct termios tty;

  if (tcgetattr(m_fd, &tty) != 0)
  {
    m_lastError = "tcgetattr failed: " + std::string(std::strerror(errno));
    return false;
  }

  // Set baud rate
  speed_t speed;
  switch (m_baudRate)
  {
  case 9600:
    speed = B9600;
    break;
  case 115200:
    speed = B115200;
    break;
  default:
    m_lastError = "Unsupported baud rate: " + std::to_string(m_baudRate);
    return false;
  }

  cfsetispeed(&tty, speed);
  cfsetospeed(&tty, speed);

  // 8N1 configuration (8 data bits, no parity, 1 stop bit)
  tty.c_cflag &= ~PARENB;        // No parity
  tty.c_cflag &= ~CSTOPB;        // 1 stop bit
  tty.c_cflag &= ~CSIZE;         // Clear size bits
  tty.c_cflag |= CS8;            // 8 data bits
  tty.c_cflag &= ~CRTSCTS;       // No hardware flow control
  tty.c_cflag |= CREAD | CLOCAL; // Enable receiver, ignore modem control

  // Raw input mode
  tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

  // Disable software flow control
  tty.c_iflag &= ~(IXON | IXOFF | IXANY);

  // Raw output mode
  tty.c_oflag &= ~OPOST;

  // Read settings
  tty.c_cc[VMIN] = 0;  // Non-blocking read
  tty.c_cc[VTIME] = 1; // 0.1 second timeout

  if (tcsetattr(m_fd, TCSANOW, &tty) != 0)
  {
    m_lastError = "tcsetattr failed: " + std::string(std::strerror(errno));
    return false;
  }

  return true;
}

bool ElectronicBoardUART::_sendCommand(const std::string& command,
                                       std::string* response,
                                       uint32_t timeoutMs)
{
  if (!isConnected())
  {
    m_lastError = "UART not connected";
    return false;
  }

  // NOTE: We intentionally do NOT call tcflush() here.
  // The persistent m_recvBuffer may contain valid data from a previous read
  // that would be lost if we flushed the OS buffer.

  // Send command with newline terminator
  std::string cmdWithTerminator = command + "\n";
  ssize_t written = write(m_fd, cmdWithTerminator.c_str(), cmdWithTerminator.length());

  if (written != static_cast<ssize_t>(cmdWithTerminator.length()))
  {
    m_lastError = "Write failed: " + std::string(std::strerror(errno));
    return false;
  }

  // Wait for data to be transmitted
  tcdrain(m_fd);

  // Read response lines, skipping any echo from the LPC1114.
  // The board may echo the sent command before replying with OK or ERR:.
  // For the STATUS command, collect multi-line data before the final OK.
  bool isStatusCmd = (command == "STATUS");
  std::string statusAccum;
  bool statusHasData = false;

  std::string line;
  while (_readLine(line, timeoutMs))
  {
    // Success response
    if (line.find("OK") == 0)
    {
      if (response)
      {
        if (isStatusCmd && statusHasData)
        {
          *response = statusAccum;
        }
        else
        {
          *response = line;
        }
      }
      return true;
    }

    // Error response
    if (line.find("ERR:") == 0)
    {
      m_lastError = line.substr(4);
      return false;
    }

    // For STATUS command, accumulate non-OK/non-ERR lines as data
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
      // Use shorter timeout for continuation lines
      timeoutMs = 200;
      continue;
    }

    // Otherwise, this is likely an echo of the sent command, skip it
    // and use a shorter timeout for the actual response
    timeoutMs = 500;
  }

  // If we get here, we timed out without receiving OK or ERR:
  if (m_lastError.empty())
  {
    m_lastError = "No response from board";
  }
  return false;
}

bool ElectronicBoardUART::_readLine(std::string& line, uint32_t timeoutMs)
{
  line.clear();

  auto startTime = std::chrono::steady_clock::now();
  auto timeout = std::chrono::milliseconds(timeoutMs);

  while (true)
  {
    // First, check if we already have a complete line in the persistent buffer
    size_t newlinePos = m_recvBuffer.find('\n');
    if (newlinePos != std::string::npos)
    {
      line = m_recvBuffer.substr(0, newlinePos);
      m_recvBuffer.erase(0, newlinePos + 1);

      // Remove trailing \r if present
      if (!line.empty() && line.back() == '\r')
      {
        line.pop_back();
      }

      // Skip empty lines (e.g., from \r\n sequences)
      if (!line.empty())
      {
        return true;
      }
      continue; // Empty line, look for next
    }

    // Check timeout
    auto elapsed = std::chrono::steady_clock::now() - startTime;
    if (elapsed > timeout)
    {
      m_lastError = "Read timeout";
      return false;
    }

    // Poll for more data from UART
    struct pollfd pfd;
    pfd.fd = m_fd;
    pfd.events = POLLIN;

    int remainingTimeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(timeout - elapsed).count();
    if (remainingTimeMs <= 0)
    {
      m_lastError = "Read timeout";
      return false;
    }

    int pollResult = poll(&pfd, 1, remainingTimeMs);

    if (pollResult < 0)
    {
      m_lastError = "Poll failed: " + std::string(std::strerror(errno));
      return false;
    }
    else if (pollResult == 0)
    {
      m_lastError = "Read timeout";
      return false;
    }

    char buffer[64];
    ssize_t bytesRead = read(m_fd, buffer, sizeof(buffer));

    if (bytesRead < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        continue;
      }
      m_lastError = "Read failed: " + std::string(std::strerror(errno));
      return false;
    }
    else if (bytesRead == 0)
    {
      continue;
    }

    // Append to persistent buffer
    m_recvBuffer.append(buffer, static_cast<size_t>(bytesRead));
  }
}