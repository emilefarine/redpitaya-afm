#pragma once

#include <cstdint>
#include <string>

/**
 * @brief Minimal SSD1306 OLED display driver over Linux I2C (/dev/i2c-*)
 *
 * Supports 128x32 monochrome OLED displays with SSD1306 controller.
 * Uses raw Linux I2C device interface (open/ioctl/write).
 */
class SSD1306
{
public:
  static constexpr int WIDTH = 128;
  static constexpr int HEIGHT = 32;
  static constexpr int PAGES = HEIGHT / 8;          // 4 pages for 32-pixel height
  static constexpr int BUFFER_SIZE = WIDTH * PAGES; // 512 bytes

  /**
   * @brief Construct the SSD1306 driver
   * @param i2cBus  Path to the I2C device (e.g. "/dev/i2c-0")
   * @param address I2C slave address (typically 0x3C)
   */
  SSD1306(const std::string& i2cBus = "/dev/i2c-0", uint8_t address = 0x3C);

  ~SSD1306();

  SSD1306(const SSD1306&) = delete;
  SSD1306& operator=(const SSD1306&) = delete;

  /**
   * @brief Initialize the display (open I2C bus, send init sequence)
   * @return true on success, false on failure
   */
  bool init();

  /**
   * @brief Close the I2C file descriptor
   */
  void close();

  /**
   * @brief Clear the framebuffer (does NOT flush to display)
   */
  void clear();

  /**
   * @brief Set a single pixel in the framebuffer
   * @param x  X coordinate (0..127)
   * @param y  Y coordinate (0..31)
   * @param on true = pixel on, false = pixel off
   */
  void setPixel(int x, int y, bool on = true);

  /**
   * @brief Draw a character from the embedded 5x7 font
   * @param x  X origin (top-left of character)
   * @param y  Y origin (top-left of character)
   * @param c  ASCII character to draw (32..126)
   */
  void drawChar(int x, int y, char c);

  /**
   * @brief Draw a null-terminated string
   * @param x  X origin
   * @param y  Y origin
   * @param str  String to draw
   */
  void drawString(int x, int y, const char* str);

  /**
   * @brief Flush the framebuffer to the display over I2C
   * @return true on success
   */
  bool display();

private:
  bool _sendCommand(uint8_t cmd);
  bool _sendCommandList(const uint8_t* cmds, int count);
  bool _sendData(const uint8_t* data, int length);

  std::string m_i2cBus;
  uint8_t m_address;
  int m_fd;
  uint8_t m_buffer[BUFFER_SIZE];
};
