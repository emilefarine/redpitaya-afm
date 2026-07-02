/**
 * @file electronic_board_test.cpp
 * @brief Test program for Electronic Board UART communication
 *
 * This program tests the UART communication with the LPC1114
 * electronic board that handles multiplexer routing and gain control.
 *
 * Usage:
 *   ./electronic_board_test [uart_device] [baud_rate]
 *
 * Default UART device: /dev/ttyPS1
 * Default baud rate: 9600
 *
 * Debug modes:
 *   ./electronic_board_test /dev/ttyPS1 9600 --loopback   (test TX/RX short)
 *   ./electronic_board_test /dev/ttyPS1 9600 --listen     (just listen for data)
 *   ./electronic_board_test /dev/ttyPS1 9600 --send-only  (send without waiting)
 *
 * @date November 2025
 */

#include "ElectronicBoardUART.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>

// Test configuration
constexpr const char* DEFAULT_UART_DEVICE = "/dev/ttyPS1";
constexpr uint32_t DEFAULT_BAUD_RATE = 9600;

void printHeader(const std::string& title)
{
  std::cout << "\n" << std::string(60, '=') << std::endl;
  std::cout << "  " << title << std::endl;
  std::cout << std::string(60, '=') << std::endl;
}

void printResult(const std::string& test, bool success, const std::string& error = "")
{
  std::cout << "  " << std::setw(40) << std::left << test;
  if (success)
  {
    std::cout << "[ OK ]" << std::endl;
  }
  else
  {
    std::cout << "[FAIL]" << std::endl;
    if (!error.empty())
    {
      std::cout << "    Error: " << error << std::endl;
    }
  }
}

void printUsage(const char* progName)
{
  std::cout << "Usage: " << progName << " [uart_device] [baud_rate] [mode]\n\n";
  std::cout << "Arguments:\n";
  std::cout << "  uart_device  UART device path (default: /dev/ttyPS1)\n";
  std::cout << "  baud_rate    Baud rate (default: 9600)\n";
  std::cout << "               Common values: 9600, 19200, 38400, 57600, 115200\n\n";
  std::cout << "Debug modes:\n";
  std::cout << "  --loopback   Loopback test (short TX to RX)\n";
  std::cout << "  --listen     Listen mode (only receive, don't send)\n";
  std::cout << "  --send-only  Send commands without waiting for response\n";
  std::cout << "  --raw        Send raw text and show raw response\n";
  std::cout << "  --hw-test    Hardware verification (test amp outputs with oscilloscope)\n\n";
  std::cout << "Examples:\n";
  std::cout << "  " << progName << "                           # Default test\n";
  std::cout << "  " << progName << " /dev/ttyPS1 115200        # Try 115200 baud\n";
  std::cout << "  " << progName << " /dev/ttyPS1 9600 --listen # Listen for data\n";
  std::cout << "  " << progName << " /dev/ttyPS1 9600 --hw-test # Hardware verification\n";
}

// Raw UART functions for debugging
int openUartRaw(const char* device, uint32_t baudRate)
{
  int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0)
  {
    perror("open");
    return -1;
  }

  struct termios tty;
  memset(&tty, 0, sizeof(tty));

  if (tcgetattr(fd, &tty) != 0)
  {
    perror("tcgetattr");
    close(fd);
    return -1;
  }

  // Set baud rate
  speed_t speed;
  switch (baudRate)
  {
  case 9600:
    speed = B9600;
    break;
  case 19200:
    speed = B19200;
    break;
  case 38400:
    speed = B38400;
    break;
  case 57600:
    speed = B57600;
    break;
  case 115200:
    speed = B115200;
    break;
  default:
    std::cerr << "Unsupported baud rate: " << baudRate << std::endl;
    close(fd);
    return -1;
  }

  cfsetospeed(&tty, speed);
  cfsetispeed(&tty, speed);

  // 8N1, no flow control
  tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
  tty.c_cflag &= ~(PARENB | PARODD);
  tty.c_cflag &= ~CSTOPB;
  tty.c_cflag &= ~CRTSCTS;
  tty.c_cflag |= (CLOCAL | CREAD);

  // Raw mode
  tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
  tty.c_iflag &= ~(IXON | IXOFF | IXANY);
  tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
  tty.c_oflag &= ~OPOST;

  // Read settings
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 1; // 100ms timeout

  if (tcsetattr(fd, TCSANOW, &tty) != 0)
  {
    perror("tcsetattr");
    close(fd);
    return -1;
  }

  // Flush buffers
  tcflush(fd, TCIOFLUSH);

  return fd;
}

void runLoopbackTest(const char* device, uint32_t baudRate)
{
  printHeader("Loopback Test");
  std::cout << "\n  Instructions:\n";
  std::cout << "  1. Disconnect the LPC1114 board\n";
  std::cout << "  2. Connect a wire between TX and RX on the Red Pitaya\n";
  std::cout << "  3. Press Enter to start the test...\n";
  std::cin.get();

  int fd = openUartRaw(device, baudRate);
  if (fd < 0)
    return;

  const char* testMsg = "LOOPBACK_TEST\n";
  char buffer[256];

  std::cout << "\n  Sending: " << testMsg;
  write(fd, testMsg, strlen(testMsg));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  int n = read(fd, buffer, sizeof(buffer) - 1);
  if (n > 0)
  {
    buffer[n] = '\0';
    std::cout << "  Received: " << buffer << std::endl;
    std::cout << "\n  ✓ Loopback works! TX and RX are functional.\n";
    std::cout << "    If normal test fails, check:\n";
    std::cout << "    - TX/RX wires to LPC1114 (may need to swap)\n";
    std::cout << "    - LPC1114 is powered and running\n";
    std::cout << "    - Baud rate matches LPC1114 firmware\n";
  }
  else
  {
    std::cout << "  Received: (nothing)\n";
    std::cout << "\n  ✗ Loopback failed! Check:\n";
    std::cout << "    - Wire connection between TX and RX\n";
    std::cout << "    - UART device path is correct\n";
    std::cout << "    - Permissions on " << device << "\n";
  }

  close(fd);
}

void runListenMode(const char* device, uint32_t baudRate)
{
  printHeader("Listen Mode");
  std::cout << "\n  Listening for incoming data...\n";
  std::cout << "  (Press Ctrl+C to stop)\n\n";

  int fd = openUartRaw(device, baudRate);
  if (fd < 0)
    return;

  char buffer[256];
  int totalBytes = 0;

  while (true)
  {
    int n = read(fd, buffer, sizeof(buffer) - 1);
    if (n > 0)
    {
      buffer[n] = '\0';
      totalBytes += n;

      // Print hex and ASCII
      std::cout << "  [" << totalBytes << " bytes] ";
      for (int i = 0; i < n; i++)
      {
        if (buffer[i] >= 32 && buffer[i] < 127)
        {
          std::cout << buffer[i];
        }
        else if (buffer[i] == '\r')
        {
          std::cout << "\\r";
        }
        else if (buffer[i] == '\n')
        {
          std::cout << "\\n";
        }
        else
        {
          std::cout << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                    << (int)(unsigned char)buffer[i] << std::dec;
        }
      }
      std::cout << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  close(fd);
}

void runSendOnlyMode(const char* device, uint32_t baudRate)
{
  printHeader("Send-Only Mode");
  std::cout << "\n  Sending commands without waiting for response.\n";
  std::cout << "  Use a logic analyzer or oscilloscope on the TX line.\n\n";

  int fd = openUartRaw(device, baudRate);
  if (fd < 0)
    return;

  const char* commands[] = {"HELP\n", "STATUS\n", "RESET\n", "MUX0,0\n", "GAIN0,2\n"};

  for (const char* cmd : commands)
  {
    std::cout << "  Sending: " << cmd;
    write(fd, cmd, strlen(cmd));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  std::cout << "\n  Done. Check TX line with analyzer.\n";
  close(fd);
}

void runRawMode(const char* device, uint32_t baudRate)
{
  printHeader("Raw Mode - Interactive");
  std::cout << "\n  Type commands and see raw responses.\n";
  std::cout << "  Type 'quit' to exit.\n\n";

  int fd = openUartRaw(device, baudRate);
  if (fd < 0)
    return;

  std::string input;
  char buffer[256];

  while (true)
  {
    std::cout << "  > ";
    std::getline(std::cin, input);

    if (input == "quit")
      break;

    // Add newline and send
    input += "\n";
    write(fd, input.c_str(), input.length());

    // Wait and read response
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int n = read(fd, buffer, sizeof(buffer) - 1);
    if (n > 0)
    {
      buffer[n] = '\0';
      std::cout << "  Response (" << n << " bytes): ";
      for (int i = 0; i < n; i++)
      {
        if (buffer[i] >= 32 && buffer[i] < 127)
        {
          std::cout << buffer[i];
        }
        else if (buffer[i] == '\r')
        {
          std::cout << "\\r";
        }
        else if (buffer[i] == '\n')
        {
          std::cout << "\\n";
        }
        else
        {
          std::cout << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                    << (int)(unsigned char)buffer[i] << std::dec;
        }
      }
      std::cout << std::endl;
    }
    else
    {
      std::cout << "  Response: (timeout - no data)\n";
    }
  }

  close(fd);
}

/**
 * @brief Hardware verification test
 *
 * This mode allows step-by-step testing of the electronic board
 * with an oscilloscope or multimeter to verify amp_out signals.
 *
 * Available connections on current board:
 * - IN0, IN1 (inputs)
 * - OUT0, OUT1 (outputs)
 * - SUM (OUT0 + OUT1)
 */
void runHardwareTest(const std::string& uartDevice, uint32_t baudRate)
{
  printHeader("Hardware Verification Test");

  std::cout << "\n";
  std::cout << "  ╔══════════════════════════════════════════════════════════╗\n";
  std::cout << "  ║              ELECTRONIC BOARD HARDWARE TEST              ║\n";
  std::cout << "  ╚══════════════════════════════════════════════════════════╝\n";
  std::cout << "\n";
  std::cout << "  AVAILABLE CONNECTIONS ON YOUR BOARD:\n";
  std::cout << "  ─────────────────────────────────────\n";
  std::cout << "  Inputs:  IN0, IN1\n";
  std::cout << "  Outputs: OUT0, OUT1, SUM (OUT0+OUT1)\n";
  std::cout << "\n";
  std::cout << "  SUGGESTED SETUP:\n";
  std::cout << "  ────────────────\n";
  std::cout << "  - Red Pitaya OUT1 -> Electronic Board IN0\n";
  std::cout << "  - Red Pitaya OUT2 -> Electronic Board IN1 (optional)\n";
  std::cout << "  - Oscilloscope CH1 -> amp_out0\n";
  std::cout << "  - Oscilloscope CH2 -> amp_out1 or sum_out\n";
  std::cout << "\n";
  std::cout << "  GAIN REFERENCE (input ±1V -> output ±10V max):\n";
  std::cout << "  ──────────────────────────────────────────────\n";
  std::cout << "  GAIN0 (x1/4): 0.5V in -> 0.125V out\n";
  std::cout << "  GAIN1 (x1/2): 0.5V in -> 0.25V out\n";
  std::cout << "  GAIN2 (x1)  : 0.5V in -> 0.5V out\n";
  std::cout << "  GAIN3 (x2)  : 0.5V in -> 1.0V out\n";
  std::cout << "  GAIN4 (x4)  : 0.5V in -> 2.0V out\n";
  std::cout << "  GAIN5 (x8)  : 0.5V in -> 4.0V out\n";
  std::cout << "\n";
  std::cout << "  Press Enter when ready to start...\n";
  std::cin.get();

  // Initialize board
  ElectronicBoardUART board(uartDevice, baudRate);

  if (!board.initialize())
  {
    std::cerr << "  ERROR: Failed to initialize UART: " << board.getLastError() << std::endl;
    return;
  }

  std::cout << "  Board connected!\n\n";

  // Reset to known state
  board.reset();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Test menu
  while (true)
  {
    std::cout << "\n";
    std::cout << "  ┌─────────────────────────────────────────────────┐\n";
    std::cout << "  │              TEST MENU                          │\n";
    std::cout << "  ├─────────────────────────────────────────────────┤\n";
    std::cout << "  │  BASIC ROUTING (single input -> single output)  │\n";
    std::cout << "  │  1. IN0 -> OUT0                                 │\n";
    std::cout << "  │  2. IN0 -> OUT1                                 │\n";
    std::cout << "  │  3. IN1 -> OUT0                                 │\n";
    std::cout << "  │  4. IN1 -> OUT1                                 │\n";
    std::cout << "  ├─────────────────────────────────────────────────┤\n";
    std::cout << "  │  GAIN TESTS                                     │\n";
    std::cout << "  │  5. Test all gains on IN0 -> OUT0               │\n";
    std::cout << "  │  6. Test all gains on IN1 -> OUT1               │\n";
    std::cout << "  ├─────────────────────────────────────────────────┤\n";
    std::cout << "  │  ADDITION TESTS (measure sum_out)               │\n";
    std::cout << "  │  7. IN0 -> OUT0 + IN0 -> OUT1 (same signal x2)  │\n";
    std::cout << "  │  8. IN0 -> OUT0 + IN1 -> OUT1 (two signals)     │\n";
    std::cout << "  ├─────────────────────────────────────────────────┤\n";
    std::cout << "  │  UTILITIES                                      │\n";
    std::cout << "  │  c. Custom routing                              │\n";
    std::cout << "  │  g. Set gain for specific input                 │\n";
    std::cout << "  │  s. Show current status                         │\n";
    std::cout << "  │  r. Reset board                                 │\n";
    std::cout << "  │  q. Quit                                        │\n";
    std::cout << "  └─────────────────────────────────────────────────┘\n";
    std::cout << "  Choice: ";

    std::string choice;
    std::getline(std::cin, choice);

    if (choice == "q" || choice == "Q")
    {
      break;
    }
    else if (choice == "r" || choice == "R")
    {
      board.reset();
      std::cout << "  -> Board reset to defaults\n";
    }
    else if (choice == "1")
    {
      board.reset();
      board.setMuxRoute(0, 0); // IN0 -> OUT0
      board.setGain(0, GainSetting::GAIN_1);
      std::cout << "\n  ┌─────────────────────────────────────────┐\n";
      std::cout << "  │  IN0 -> OUT0, gain x1                   │\n";
      std::cout << "  │  Measure: amp_out0                      │\n";
      std::cout << "  │  Expected: same as input signal         │\n";
      std::cout << "  └─────────────────────────────────────────┘\n";
      std::cout << "  Press Enter to continue...";
      std::cin.get();
    }
    else if (choice == "2")
    {
      board.reset();
      board.setMuxRoute(1, 0); // IN0 -> OUT1
      board.setGain(0, GainSetting::GAIN_1);
      std::cout << "\n  ┌─────────────────────────────────────────┐\n";
      std::cout << "  │  IN0 -> OUT1, gain x1                   │\n";
      std::cout << "  │  Measure: amp_out1                      │\n";
      std::cout << "  │  Expected: same as input signal         │\n";
      std::cout << "  └─────────────────────────────────────────┘\n";
      std::cout << "  Press Enter to continue...";
      std::cin.get();
    }
    else if (choice == "3")
    {
      board.reset();
      board.setMuxRoute(0, 1); // IN1 -> OUT0
      board.setGain(1, GainSetting::GAIN_1);
      std::cout << "\n  ┌─────────────────────────────────────────┐\n";
      std::cout << "  │  IN1 -> OUT0, gain x1                   │\n";
      std::cout << "  │  Measure: amp_out0                      │\n";
      std::cout << "  │  Expected: same as IN1 signal           │\n";
      std::cout << "  └─────────────────────────────────────────┘\n";
      std::cout << "  Press Enter to continue...";
      std::cin.get();
    }
    else if (choice == "4")
    {
      board.reset();
      board.setMuxRoute(1, 1); // IN1 -> OUT1
      board.setGain(1, GainSetting::GAIN_1);
      std::cout << "\n  ┌─────────────────────────────────────────┐\n";
      std::cout << "  │  IN1 -> OUT1, gain x1                   │\n";
      std::cout << "  │  Measure: amp_out1                      │\n";
      std::cout << "  │  Expected: same as IN1 signal           │\n";
      std::cout << "  └─────────────────────────────────────────┘\n";
      std::cout << "  Press Enter to continue...";
      std::cin.get();
    }
    else if (choice == "5")
    {
      // Test all gains on IN0 -> OUT0
      board.reset();
      board.setMuxRoute(0, 0); // IN0 -> OUT0

      struct GainInfo
      {
        GainSetting gain;
        const char* name;
        const char* multiplier;
        const char* expected;
      };

      GainInfo gains[] = {
          {GainSetting::GAIN_1_4, "GAIN 0", "x0.25", "0.125V"},
          {GainSetting::GAIN_1_2, "GAIN 1", "x0.5", "0.25V"},
          {GainSetting::GAIN_1, "GAIN 2", "x1", "0.5V"},
          {GainSetting::GAIN_2, "GAIN 3", "x2", "1.0V"},
          {GainSetting::GAIN_4, "GAIN 4", "x4", "2.0V"},
          {GainSetting::GAIN_8, "GAIN 5", "x8", "4.0V"},
      };

      std::cout << "\n  ╔═════════════════════════════════════════════════╗\n";
      std::cout << "  ║  GAIN TEST: IN0 -> OUT0                         ║\n";
      std::cout << "  ║  Measure: amp_out0                              ║\n";
      std::cout << "  ║  Input signal: 0.5V amplitude recommended       ║\n";
      std::cout << "  ╚═════════════════════════════════════════════════╝\n\n";

      for (const auto& g : gains)
      {
        board.setGain(0, g.gain);
        std::cout << "  ┌─────────────────────────────────────────┐\n";
        std::cout << "  │  " << g.name << " (" << g.multiplier << ")\n";
        std::cout << "  │  Expected output: " << g.expected << " (if input=0.5V)\n";
        std::cout << "  └─────────────────────────────────────────┘\n";
        std::cout << "  Press Enter for next gain...";
        std::cin.get();
      }
      std::cout << "\n  Gain test on IN0 complete!\n";
    }
    else if (choice == "6")
    {
      // Test all gains on IN1 -> OUT1
      board.reset();
      board.setMuxRoute(1, 1); // IN1 -> OUT1

      struct GainInfo
      {
        GainSetting gain;
        const char* name;
        const char* multiplier;
        const char* expected;
      };

      GainInfo gains[] = {
          {GainSetting::GAIN_1_4, "GAIN 0", "x0.25", "0.125V"},
          {GainSetting::GAIN_1_2, "GAIN 1", "x0.5", "0.25V"},
          {GainSetting::GAIN_1, "GAIN 2", "x1", "0.5V"},
          {GainSetting::GAIN_2, "GAIN 3", "x2", "1.0V"},
          {GainSetting::GAIN_4, "GAIN 4", "x4", "2.0V"},
          {GainSetting::GAIN_8, "GAIN 5", "x8", "4.0V"},
      };

      std::cout << "\n  ╔═════════════════════════════════════════════════╗\n";
      std::cout << "  ║  GAIN TEST: IN1 -> OUT1                         ║\n";
      std::cout << "  ║  Measure: amp_out1                              ║\n";
      std::cout << "  ║  Input signal: 0.5V amplitude recommended       ║\n";
      std::cout << "  ╚═════════════════════════════════════════════════╝\n\n";

      for (const auto& g : gains)
      {
        board.setGain(1, g.gain);
        std::cout << "  ┌─────────────────────────────────────────┐\n";
        std::cout << "  │  " << g.name << " (" << g.multiplier << ")\n";
        std::cout << "  │  Expected output: " << g.expected << " (if input=0.5V)\n";
        std::cout << "  └─────────────────────────────────────────┘\n";
        std::cout << "  Press Enter for next gain...";
        std::cin.get();
      }
      std::cout << "\n  Gain test on IN1 complete!\n";
    }
    else if (choice == "7")
    {
      // Test addition with same signal: sum_out = OUT0 + OUT1
      board.reset();
      board.setMuxRoute(0, 0); // IN0 -> OUT0
      board.setMuxRoute(1, 0); // IN0 -> OUT1
      board.setGain(0, GainSetting::GAIN_1);

      std::cout << "\n  ╔═════════════════════════════════════════════════╗\n";
      std::cout << "  ║  ADDITION TEST: Same signal                     ║\n";
      std::cout << "  ║  IN0 -> OUT0 AND IN0 -> OUT1                    ║\n";
      std::cout << "  ║  sum_out = OUT0 + OUT1 = 2 * IN0                ║\n";
      std::cout << "  ╠═════════════════════════════════════════════════╣\n";
      std::cout << "  ║  Measure: sum_out                               ║\n";
      std::cout << "  ║  Expected: 2x input amplitude                   ║\n";
      std::cout << "  ║  (0.5V in -> 1.0V on sum_out)                   ║\n";
      std::cout << "  ╚═════════════════════════════════════════════════╝\n";
      std::cout << "  Press Enter to continue...";
      std::cin.get();
    }
    else if (choice == "8")
    {
      // Test addition with two different signals
      board.reset();
      board.setMuxRoute(0, 0); // IN0 -> OUT0
      board.setMuxRoute(1, 1); // IN1 -> OUT1
      board.setGain(0, GainSetting::GAIN_1);
      board.setGain(1, GainSetting::GAIN_1);

      std::cout << "\n  ╔═════════════════════════════════════════════════╗\n";
      std::cout << "  ║  ADDITION TEST: Two different signals           ║\n";
      std::cout << "  ║  IN0 -> OUT0 AND IN1 -> OUT1                    ║\n";
      std::cout << "  ║  sum_out = OUT0 + OUT1 = IN0 + IN1              ║\n";
      std::cout << "  ╠═════════════════════════════════════════════════╣\n";
      std::cout << "  ║  Connect: Red Pitaya OUT1 -> IN0                ║\n";
      std::cout << "  ║           Red Pitaya OUT2 -> IN1                ║\n";
      std::cout << "  ║  Measure: sum_out                               ║\n";
      std::cout << "  ║  Expected: sum of both signals                  ║\n";
      std::cout << "  ╚═════════════════════════════════════════════════╝\n";
      std::cout << "  Press Enter to continue...";
      std::cin.get();
    }
    else if (choice == "c" || choice == "C")
    {
      // Custom routing
      std::cout << "\n  Custom routing:\n";
      std::cout << "  Enter output (0=OUT0, 1=OUT1): ";
      std::string outStr;
      std::getline(std::cin, outStr);
      int out = std::stoi(outStr);

      std::cout << "  Enter input (0=IN0, 1=IN1, X=disconnect): ";
      std::string inStr;
      std::getline(std::cin, inStr);

      if (inStr == "X" || inStr == "x")
      {
        board.disconnectMux(out);
        std::cout << "  -> OUT" << out << " disconnected\n";
      }
      else
      {
        int in = std::stoi(inStr);
        if (board.setMuxRoute(out, in))
        {
          std::cout << "  -> IN" << in << " routed to OUT" << out << "\n";
        }
        else
        {
          std::cout << "  -> ERROR: " << board.getLastError() << "\n";
        }
      }
    }
    else if (choice == "g" || choice == "G")
    {
      // Set gain
      std::cout << "\n  Set gain:\n";
      std::cout << "  Enter input channel (0=IN0, 1=IN1): ";
      std::string chStr;
      std::getline(std::cin, chStr);
      int ch = std::stoi(chStr);

      std::cout << "  Enter gain (0=x1/4, 1=x1/2, 2=x1, 3=x2, 4=x4, 5=x8): ";
      std::string gainStr;
      std::getline(std::cin, gainStr);
      int gainVal = std::stoi(gainStr);

      if (gainVal >= 0 && gainVal <= 5 && board.setGain(ch, static_cast<GainSetting>(gainVal)))
      {
        const char* gainNames[] = {"x1/4", "x1/2", "x1", "x2", "x4", "x8"};
        std::cout << "  -> IN" << ch << " gain set to " << gainNames[gainVal] << "\n";
      }
      else
      {
        std::cout << "  -> ERROR: " << board.getLastError() << "\n";
      }
    }
    else if (choice == "s" || choice == "S")
    {
      std::string status;
      if (board.getStatus(status))
      {
        std::cout << "\n" << status << std::endl;
      }
      else
      {
        std::cout << "  Failed to get status\n";
      }
    }
  }

  board.reset();
  board.close();
  std::cout << "\n  Hardware test complete. Board reset and disconnected.\n";
}

void runNormalTest(const std::string& uartDevice, uint32_t baudRate)
{
  // Create UART interface
  ElectronicBoardUART board(uartDevice, baudRate);

  // ========================================
  // Test 1: Initialization
  // ========================================
  printHeader("Test 1: UART Initialization");

  bool initOk = board.initialize();
  printResult("Open and configure UART", initOk, board.getLastError());

  if (!initOk)
  {
    std::cerr << "\nFailed to initialize UART. Exiting." << std::endl;
    std::cerr << "Make sure the device exists and you have permissions." << std::endl;
    std::cerr << "Try: sudo chmod 666 " << uartDevice << std::endl;
    return;
  }

  // Small delay to let board initialize
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // ========================================
  // Test 2: Reset Command
  // ========================================
  printHeader("Test 2: Reset Command");

  bool resetOk = board.reset();
  printResult("Reset board to defaults", resetOk, board.getLastError());

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // ========================================
  // Test 3: Status Command
  // ========================================
  printHeader("Test 3: Status Command");

  std::string status;
  bool statusOk = board.getStatus(status);
  printResult("Get board status", statusOk, board.getLastError());

  if (statusOk && !status.empty())
  {
    std::cout << "\n  Board Status:\n";
    std::cout << "  -------------\n";
    size_t pos = 0;
    while (pos < status.length())
    {
      size_t newline = status.find('\n', pos);
      if (newline == std::string::npos)
      {
        std::cout << "    " << status.substr(pos) << std::endl;
        break;
      }
      std::cout << "    " << status.substr(pos, newline - pos) << std::endl;
      pos = newline + 1;
    }
  }

  // ========================================
  // Test 4: MUX Routing
  // ========================================
  printHeader("Test 4: MUX Routing Commands");

  bool mux0Ok = board.setMuxRoute(0, 0);
  printResult("MUX0,0 (Route input 0 -> output 0)", mux0Ok, board.getLastError());

  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  bool mux1Ok = board.setMuxRoute(1, 1);
  printResult("MUX1,1 (Route input 1 -> output 1)", mux1Ok, board.getLastError());

  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  bool mux2Ok = board.setMuxRoute(2, 2);
  printResult("MUX2,2 (Route input 2 -> output 2)", mux2Ok, board.getLastError());

  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  bool mux3DisOk = board.disconnectMux(3);
  printResult("MUX3,X (Disconnect output 3)", mux3DisOk, board.getLastError());

  // ========================================
  // Test 5: Gain Control
  // ========================================
  printHeader("Test 5: Gain Control Commands");

  struct GainTest
  {
    uint8_t channel;
    GainSetting gain;
    const char* description;
  };

  GainTest gainTests[] = {
      {0, GainSetting::GAIN_1, "Channel 0, Gain x1"},
      {1, GainSetting::GAIN_2, "Channel 1, Gain x2"},
      {2, GainSetting::GAIN_4, "Channel 2, Gain x4"},
      {3, GainSetting::GAIN_8, "Channel 3, Gain x8"},
  };

  for (const auto& test : gainTests)
  {
    bool gainOk = board.setGain(test.channel, test.gain);
    std::string testName = std::string("GAIN") + std::to_string(test.channel) + "," +
                           std::to_string(static_cast<int>(test.gain)) + " (" + test.description +
                           ")";
    printResult(testName, gainOk, board.getLastError());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  // ========================================
  // Test 6: Final Status Check
  // ========================================
  printHeader("Test 6: Final Status Check");

  status.clear();
  statusOk = board.getStatus(status);
  printResult("Get final board status", statusOk, board.getLastError());

  if (statusOk && !status.empty())
  {
    std::cout << "\n  Final Board Status:\n";
    std::cout << "  -------------------\n";
    size_t pos = 0;
    while (pos < status.length())
    {
      size_t newline = status.find('\n', pos);
      if (newline == std::string::npos)
      {
        std::cout << "    " << status.substr(pos) << std::endl;
        break;
      }
      std::cout << "    " << status.substr(pos, newline - pos) << std::endl;
      pos = newline + 1;
    }
  }

  // ========================================
  // Test 7: Error Handling
  // ========================================
  printHeader("Test 7: Error Handling (Expected Failures)");

  bool invalidCh = board.setMuxRoute(5, 0);
  printResult("MUX5,0 (Invalid channel - should fail)", !invalidCh);

  bool invalidGain = board.setGain(0, static_cast<GainSetting>(10));
  printResult("GAIN0,10 (Invalid gain - should fail)", !invalidGain);

  // ========================================
  // Test 8: Reset and Cleanup
  // ========================================
  printHeader("Test 8: Reset and Cleanup");

  resetOk = board.reset();
  printResult("Final reset to defaults", resetOk, board.getLastError());

  board.close();
  printResult("Close UART connection", !board.isConnected());

  // ========================================
  // Summary
  // ========================================
  printHeader("Test Summary");

  std::cout << "\n  All tests completed!" << std::endl;
}

int main(int argc, char* argv[])
{
  std::cout << "Electronic Board UART Test" << std::endl;
  std::cout << "==========================" << std::endl;

  // Parse arguments
  std::string uartDevice = DEFAULT_UART_DEVICE;
  uint32_t baudRate = DEFAULT_BAUD_RATE;
  std::string mode = "";

  for (int i = 1; i < argc; i++)
  {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h")
    {
      printUsage(argv[0]);
      return 0;
    }
    else if (arg == "--loopback")
    {
      mode = "loopback";
    }
    else if (arg == "--listen")
    {
      mode = "listen";
    }
    else if (arg == "--send-only")
    {
      mode = "send-only";
    }
    else if (arg == "--raw")
    {
      mode = "raw";
    }
    else if (arg == "--hw-test")
    {
      mode = "hw-test";
    }
    else if (arg[0] == '/')
    {
      uartDevice = arg;
    }
    else if (arg[0] >= '0' && arg[0] <= '9')
    {
      baudRate = std::stoul(arg);
    }
  }

  std::cout << "\nUART Device: " << uartDevice << std::endl;
  std::cout << "Baud Rate:   " << baudRate << std::endl;

  // Run selected mode
  if (mode == "loopback")
  {
    runLoopbackTest(uartDevice.c_str(), baudRate);
  }
  else if (mode == "listen")
  {
    runListenMode(uartDevice.c_str(), baudRate);
  }
  else if (mode == "send-only")
  {
    runSendOnlyMode(uartDevice.c_str(), baudRate);
  }
  else if (mode == "raw")
  {
    runRawMode(uartDevice.c_str(), baudRate);
  }
  else if (mode == "hw-test")
  {
    runHardwareTest(uartDevice, baudRate);
  }
  else
  {
    runNormalTest(uartDevice, baudRate);
  }

  return 0;
}