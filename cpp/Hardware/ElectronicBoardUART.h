/**
 * @file ElectronicBoardUART.h
 * @brief UART communication with the LPC1114 Electronic Board
 *
 * This class provides an interface to communicate with the external
 * electronic board via UART. The board handles multiplexer routing
 * and variable gain amplifier control.
 *
 * Protocol:
 *   Commands are ASCII text terminated by '\n' (LF)
 *   Responses: "OK\r\n" on success, "ERR:<message>\r\n" on error
 *
 * Commands (wire format uses 1-based channel numbers matching board connector labels):
 *   MUX<out>,<in>   - Route input to output (out=1-4, in=1-4 or X)
 *   GAIN<ch>,<val>  - Set gain for channel (ch=1-4, val=0-5)
 *   STATUS          - Print current configuration
 *   RESET           - Reset to defaults (all disconnected, gain=1)
 *
 * Note: The C++ API (setMuxRoute, disconnectMux, setGain) uses 0-indexed
 * channel parameters (0-3). The translation to 1-based wire format is
 * handled internally.
 */

#ifndef ELECTRONIC_BOARD_UART_H
#define ELECTRONIC_BOARD_UART_H

#include <cstdint>
#include <string>

/**
 * @brief Gain settings for PGA849 amplifier
 */
enum class GainSetting : uint8_t
{
  GAIN_1_8 = 0,
  GAIN_1_4 = 1,
  GAIN_1_2 = 2,
  GAIN_1 = 3,
  GAIN_2 = 4,
  GAIN_4 = 5,
  GAIN_8 = 6,
  GAIN_16 = 7
};

class ElectronicBoardUART
{
public:
  // Number of MUX channels
  static constexpr uint8_t NUM_CHANNELS = 4;
  static constexpr uint8_t DISCONNECTED = 0xFF;

  /**
   * @brief Constructor
   * @param devicePath Path to UART device (default: /dev/ttyPS1)
   * @param baudRate UART baud rate (default: 9600)
   */
  explicit ElectronicBoardUART(const std::string& devicePath = "/dev/ttyPS1",
                               uint32_t baudRate = 9600);

  ~ElectronicBoardUART();

  ElectronicBoardUART(const ElectronicBoardUART&) = delete;
  ElectronicBoardUART& operator=(const ElectronicBoardUART&) = delete;

  bool initialize();
  void close();
  bool isConnected() const;

  /**
   * @brief Set multiplexer routing
   * Routes an input channel to an output channel
   * @param output Output channel (0-3, maps to board connector OUT1-OUT4)
   * @param input Input channel (0-3, maps to board connector IN1-IN4)
   * @return true if successful
   */
  bool setMuxRoute(uint8_t output, uint8_t input);

  /**
   * @brief Disconnect a multiplexer output
   * @param output Output channel (0-3, maps to board connector OUT1-OUT4)
   * @return true if successful
   */
  bool disconnectMux(uint8_t output);

  /**
   * @brief Set amplifier gain for a channel
   * @param channel Input channel (0-3, maps to board connector IN1-IN4)
   * @param gain Gain setting
   * @return true if successful
   */
  bool setGain(uint8_t channel, GainSetting gain);

  /**
   * @brief Request status from the board
   * @param statusOut String to receive status output
   * @return true if successful
   */
  bool getStatus(std::string& statusOut);

  /**
   * @brief Reset board to defaults
   * All outputs disconnected, all gains set to 1
   * @return true if successful
   */
  bool reset();

  /**
   * @brief Get gain value as a human-readable string
   * @param gain Gain setting
   * @return String representation (e.g., "1/4", "1", "8")
   */
  static const char* gainToString(GainSetting gain);

  /**
   * @brief Get last error message
   * @return Error message string
   */
  const std::string& getLastError() const;

private:
  /**
   * @brief Send a command and wait for response
   * @param command Command string (without terminator)
   * @param response String to receive response (optional)
   * @param timeoutMs Timeout in milliseconds
   * @return true if command succeeded (received "OK")
   */
  bool _sendCommand(const std::string& command,
                    std::string* response = nullptr,
                    uint32_t timeoutMs = 1000);

  /**
   * @brief Read a line from UART
   * @param line String to receive line (without terminators)
   * @param timeoutMs Timeout in milliseconds
   * @return true if line was read successfully
   */
  bool _readLine(std::string& line, uint32_t timeoutMs);

  /**
   * @brief Configure UART port settings
   * @return true if successful
   */
  bool _configurePort();

  std::string m_devicePath;
  uint32_t m_baudRate;
  int m_fd; // File descriptor for UART
  std::string m_lastError;
  std::string m_recvBuffer; // Persistent buffer for partial UART reads
};

#endif // ELECTRONIC_BOARD_UART_H
