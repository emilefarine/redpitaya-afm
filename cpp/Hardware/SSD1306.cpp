#include "SSD1306.h"

#include "Font5x7.h"

#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

// SSD1306 initialization sequence for 128x32
static const uint8_t SSD1306_INIT_128x32[] = {
    0xAE,       // Display OFF
    0xD5, 0x80, // Set display clock divide ratio / oscillator frequency
    0xA8, 0x1F, // Set multiplex ratio (1 to 32) → 0x1F = 31
    0xD3, 0x00, // Set display offset = 0
    0x40,       // Set start line = 0
    0x8D, 0x14, // Charge pump: enable (0x14) - required for most SSD1306 modules
    0x20, 0x00, // Memory addressing mode: horizontal
    0xA1,       // Segment re-map: column 127 mapped to SEG0
    0xC8,       // COM output scan direction: remapped (bottom-to-top)
    0xDA, 0x02, // COM pins hardware config: sequential, no L/R remap (for 128x32)
    0x81, 0x8F, // Set contrast = 0x8F
    0xD9, 0xF1, // Set pre-charge period
    0xDB, 0x40, // Set VCOMH deselect level
    0xA4,       // Entire display ON (follows RAM content)
    0xA6,       // Normal display (not inverted)
    0xAF        // Display ON
};

SSD1306::SSD1306(const std::string& i2cBus, uint8_t address)
    : m_i2cBus(i2cBus)
    , m_address(address)
    , m_fd(-1)
{
  std::memset(m_buffer, 0, BUFFER_SIZE);
}

SSD1306::~SSD1306()
{
  close();
}

bool SSD1306::init()
{
  m_fd = ::open(m_i2cBus.c_str(), O_RDWR);
  if (m_fd < 0)
  {
    std::cerr << "[SSD1306] Failed to open I2C bus: " << m_i2cBus << std::endl;
    return false;
  }

  if (ioctl(m_fd, I2C_SLAVE, m_address) < 0)
  {
    std::cerr << "[SSD1306] Failed to set I2C address 0x" << std::hex << (int)m_address << std::dec
              << std::endl;
    close();
    return false;
  }

  if (!_sendCommandList(SSD1306_INIT_128x32, sizeof(SSD1306_INIT_128x32)))
  {
    std::cerr << "[SSD1306] Failed to send init sequence" << std::endl;
    close();
    return false;
  }

  // Clear and show
  clear();
  if (!display())
  {
    std::cerr << "[SSD1306] Failed to flush initial clear" << std::endl;
    close();
    return false;
  }

  return true;
}

void SSD1306::close()
{
  if (m_fd >= 0)
  {
    ::close(m_fd);
    m_fd = -1;
  }
}

void SSD1306::clear()
{
  std::memset(m_buffer, 0, BUFFER_SIZE);
}

void SSD1306::setPixel(int x, int y, bool on)
{
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT)
  {
    return;
  }

  // The framebuffer is organized in pages (horizontal bands of 8 pixels tall).
  // Each byte represents 8 vertical pixels in a column, with bit 0 at top.
  int page = y / 8;
  int bit = y % 8;

  if (on)
  {
    m_buffer[page * WIDTH + x] |= (1 << bit);
  }
  else
  {
    m_buffer[page * WIDTH + x] &= ~(1 << bit);
  }
}

void SSD1306::drawChar(int x, int y, char c)
{
  if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR)
  {
    return;
  }

  int index = c - FONT_FIRST_CHAR;
  for (int col = 0; col < FONT_WIDTH; ++col)
  {
    uint8_t columnData = FONT_5x7[index][col];
    for (int row = 0; row < FONT_HEIGHT; ++row)
    {
      if (columnData & (1 << row))
      {
        setPixel(x + col, y + row, true);
      }
    }
  }
}

void SSD1306::drawString(int x, int y, const char* str)
{
  int curX = x;
  while (*str)
  {
    drawChar(curX, y, *str);
    curX += FONT_PITCH;
    ++str;
  }
}

bool SSD1306::display()
{
  // Set column address range: 0..127
  uint8_t colCmd[] = {0x21, 0x00, 0x7F};
  if (!_sendCommandList(colCmd, sizeof(colCmd)))
    return false;

  // Set page address range: 0..3 (for 32px height)
  uint8_t pageCmd[] = {0x22, 0x00, 0x03};
  if (!_sendCommandList(pageCmd, sizeof(pageCmd)))
    return false;

  // Send framebuffer data
  return _sendData(m_buffer, BUFFER_SIZE);
}

bool SSD1306::_sendCommand(uint8_t cmd)
{
  // I2C write: control byte 0x00 (Co=0, D/C#=0 -> command), then command byte
  uint8_t buf[2] = {0x00, cmd};
  if (::write(m_fd, buf, 2) != 2)
  {
    std::cerr << "[SSD1306] I2C command write failed" << std::endl;
    return false;
  }
  return true;
}

bool SSD1306::_sendCommandList(const uint8_t* cmds, int count)
{
  for (int i = 0; i < count; ++i)
  {
    if (!_sendCommand(cmds[i]))
      return false;
  }
  return true;
}

bool SSD1306::_sendData(const uint8_t* data, int length)
{
  // Each chunk: 0x40 (Co=0, D/C#=1 -> data), then up to CHUNK_SIZE bytes.
  constexpr int CHUNK_SIZE = 16; // Conservative chunk size
  uint8_t buf[CHUNK_SIZE + 1];
  buf[0] = 0x40; // Data control byte

  int offset = 0;
  while (offset < length)
  {
    int remaining = length - offset;
    int chunkLen = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;

    std::memcpy(buf + 1, data + offset, chunkLen);
    if (::write(m_fd, buf, chunkLen + 1) != chunkLen + 1)
    {
      std::cerr << "[SSD1306] I2C data write failed at offset " << offset << std::endl;
      return false;
    }
    offset += chunkLen;
  }
  return true;
}
