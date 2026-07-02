/**
 * @file oled_ip.cpp
 * @brief Standalone tool to display the Red Pitaya's IP address on an SSD1306 OLED
 *
 * Usage: ./oled_ip [--bus /dev/i2c-0] [--addr 0x3C] [--retry N] [--persist] [--refresh N]
 *
 * Reads the IP address from the system's network interfaces and displays it
 * on a 128x32 SSD1306 OLED connected via I2C.
 *
 * --persist      Keep running and refresh the display every --refresh seconds.
 *                Use this as a systemd service (Type=simple) so the display
 *                never goes blank due to charge-pump drain on the SSD1306.
 * --refresh N    Refresh interval in seconds when --persist is active (default: 30).
 * --retry N      Number of retries if no IP found at startup (default: 1).
 */

#include "SSD1306.h"

#include <arpa/inet.h>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ifaddrs.h>
#include <iostream>
#include <net/if.h>
#include <string>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void onSignal(int)
{
  g_stop = 1;
}

static const char* DEFAULT_I2C_BUS = "/dev/i2c-0";
static const uint8_t DEFAULT_I2C_ADDR = 0x3C;
static const int DEFAULT_RETRIES = 1;
static const int DEFAULT_REFRESH_S = 30;

/**
 * @brief Get the IPv4 address of a usable network interface
 * @return IP address string, or empty if none found
 *
 * Priority: eth0 > wlan0 > any other non-loopback interface
 */
static std::string getIPAddress()
{
  struct ifaddrs* ifAddrList = nullptr;
  if (getifaddrs(&ifAddrList) != 0)
  {
    std::cerr << "[oled_ip] getifaddrs() failed" << std::endl;
    return "";
  }

  std::string ethIP;
  std::string wlanIP;
  std::string otherIP;

  for (struct ifaddrs* ifa = ifAddrList; ifa != nullptr; ifa = ifa->ifa_next)
  {
    if (ifa->ifa_addr == nullptr)
    {
      continue;
    }

    // Only IPv4
    if (ifa->ifa_addr->sa_family != AF_INET)
    {
      continue;
    }

    // Skip loopback
    if (ifa->ifa_flags & IFF_LOOPBACK)
    {
      continue;
    }

    // Must be up and running
    if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_RUNNING))
    {
      continue;
    }

    char addrBuf[INET_ADDRSTRLEN];
    struct sockaddr_in* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
    inet_ntop(AF_INET, &sa->sin_addr, addrBuf, sizeof(addrBuf));

    std::string name(ifa->ifa_name);
    std::string ip(addrBuf);

    if (name == "eth0")
    {
      ethIP = ip;
    }
    else if (name == "wlan0")
    {
      wlanIP = ip;
    }
    else if (otherIP.empty())
    {
      otherIP = ip;
    }
  }

  freeifaddrs(ifAddrList);

  if (!ethIP.empty())
  {
    return ethIP;
  }
  if (!wlanIP.empty())
  {
    return wlanIP;
  }
  return otherIP;
}

static void printUsage(const char* progName)
{
  std::cerr << "Usage: " << progName << " [options]" << std::endl;
  std::cerr << "  --bus <path>      I2C bus device (default: /dev/i2c-0)" << std::endl;
  std::cerr << "  --addr <hex>      I2C address (default: 0x3C)" << std::endl;
  std::cerr << "  --retry <N>       Number of retries if no IP found at startup (default: 1)"
            << std::endl;
  std::cerr << "  --persist         Keep running and refresh display every --refresh seconds"
            << std::endl;
  std::cerr << "  --refresh <N>     Refresh interval in seconds for --persist mode (default: 30)"
            << std::endl;
  std::cerr << "  --help            Show this help" << std::endl;
}

int main(int argc, char* argv[])
{
  std::string i2cBus = DEFAULT_I2C_BUS;
  uint8_t i2cAddr = DEFAULT_I2C_ADDR;
  int retries = DEFAULT_RETRIES;
  int refresh = DEFAULT_REFRESH_S;
  bool persist = false;

  for (int i = 1; i < argc; ++i)
  {
    std::string arg(argv[i]);

    if (arg == "--help" || arg == "-h")
    {
      printUsage(argv[0]);
      return 0;
    }
    else if (arg == "--bus" && i + 1 < argc)
    {
      i2cBus = argv[++i];
    }
    else if (arg == "--addr" && i + 1 < argc)
    {
      i2cAddr = static_cast<uint8_t>(std::strtol(argv[++i], nullptr, 0));
    }
    else if (arg == "--retry" && i + 1 < argc)
    {
      retries = std::atoi(argv[++i]);
      if (retries < 1)
      {
        retries = 1;
      }
    }
    else if (arg == "--persist")
    {
      persist = true;
    }
    else if (arg == "--refresh" && i + 1 < argc)
    {
      refresh = std::atoi(argv[++i]);
      if (refresh < 1)
      {
        refresh = 1;
      }
    }
    else
    {
      std::cerr << "Unknown argument: " << arg << std::endl;
      printUsage(argv[0]);
      return 1;
    }
  }

  // Get IP address with retry logic (for boot-time when network may not be ready)
  std::string ip;
  for (int attempt = 0; attempt < retries; ++attempt)
  {
    ip = getIPAddress();
    if (!ip.empty())
    {
      break;
    }

    if (attempt + 1 < retries)
    {
      std::cerr << "[oled_ip] No IP found, retrying in 2s... (" << (attempt + 1) << "/" << retries
                << ")" << std::endl;
      sleep(2);
    }
  }

  if (ip.empty())
  {
    std::cerr << "[oled_ip] No IP address found on any interface" << std::endl;
    ip = "No IP found";
  }

  std::cout << "[oled_ip] Displaying: " << ip << std::endl;

  // Initialize display, retry loop in case the I2C bus is not yet ready at boot
  constexpr int I2C_RETRIES = 5;
  constexpr int I2C_RETRY_DELAY_S = 2;
  SSD1306 oled(i2cBus, i2cAddr);
  bool initialized = false;
  for (int attempt = 0; attempt < I2C_RETRIES; ++attempt)
  {
    if (oled.init())
    {
      initialized = true;
      break;
    }
    if (attempt + 1 < I2C_RETRIES)
    {
      std::cerr << "[oled_ip] I2C init failed, retrying in " << I2C_RETRY_DELAY_S << "s... ("
                << (attempt + 1) << "/" << I2C_RETRIES << ")" << std::endl;
      oled.close(); // reset fd before next attempt
      sleep(I2C_RETRY_DELAY_S);
    }
  }
  if (!initialized)
  {
    std::cerr << "[oled_ip] Failed to initialize OLED display after " << I2C_RETRIES << " attempts"
              << std::endl;
    return 1;
  }

  // Helper lambda: render current IP to the display
  auto renderIP = [&](const std::string& displayIP) -> bool
  {
    oled.clear();
    oled.drawString(0, 0, "IP Address:");
    oled.drawString(0, 12, displayIP.c_str());
    return oled.display();
  };

  if (!renderIP(ip))
  {
    std::cerr << "[oled_ip] Failed to update display" << std::endl;
    oled.close();
    return 1;
  }

  if (!persist)
  {
    // One-shot mode: display and exit
    oled.close();
    std::cout << "[oled_ip] Done." << std::endl;
    return 0;
  }

  // Persist mode: keep running, refresh display every <refresh> seconds.
  // This prevents the SSD1306 charge pump from draining and blanking the screen.
  std::signal(SIGTERM, onSignal);
  std::signal(SIGINT, onSignal);
  std::cout << "[oled_ip] Persist mode: refreshing every " << refresh << "s. PID=" << getpid()
            << std::endl;

  while (!g_stop)
  {
    // Sleep in 1-second ticks so SIGTERM is handled promptly
    for (int t = 0; t < refresh && !g_stop; ++t)
    {
      sleep(1);
    }

    if (g_stop)
    {
      break;
    }
    // Re-read IP in case it changed (e.g. DHCP renewal)
    std::string newIP = getIPAddress();
    if (newIP.empty())
    {
      newIP = "No IP found";
    }

    if (newIP != ip)
    {
      ip = newIP;
      std::cout << "[oled_ip] IP changed: " << ip << std::endl;
    }

    renderIP(ip);
  }

  // Clear display on clean exit
  oled.clear();
  oled.display();
  oled.close();
  std::cout << "[oled_ip] Stopped." << std::endl;
  return 0;
}
