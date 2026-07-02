#pragma once

#include "Protocol.h"

#include <atomic>
#include <functional>
#include <future>
#include <string>
#include <thread>

namespace AFM
{

/**
 * @brief Command handler callback type
 * Takes a ParsedCommand and returns a response string
 */
using CommandCallback = std::function<std::string(const ParsedCommand&)>;

/**
 * @brief TCP Server for remote measurement control
 */
class TCPServer
{
public:
  /**
   * @brief Constructor
   * @param port Port to listen on (default: 5000)
   */
  explicit TCPServer(uint16_t port = ServerConfig::DEFAULT_PORT);

  ~TCPServer();

  TCPServer(const TCPServer&) = delete;
  TCPServer& operator=(const TCPServer&) = delete;

  /**
   * @brief Set the command handler callback
   * @param callback Function to handle incoming commands
   */
  void setCommandHandler(CommandCallback callback);

  /**
   * @brief Start the server (blocking)
   * @return true if server started successfully
   */
  bool start();

  /**
   * @brief Start the server in a background thread
   * @return true if server started successfully
   */
  bool startAsync();

  /**
   * @brief Stop the server
   */
  void stop();

  /**
   * @brief Check if server is running
   */
  bool isRunning() const
  {
    return m_running.load();
  }

  /**
   * @brief Get the port number
   */
  uint16_t getPort() const
  {
    return m_port;
  }

  /**
   * @brief Get last error message
   */
  const std::string& getLastError() const
  {
    return m_lastError;
  }

  /**
   * @brief Check if a client is currently connected
   */
  bool hasClient() const
  {
    return m_clientFd >= 0;
  }

  /**
   * @brief Send a message to the connected client
   * @param message Message to send
   * @return true if sent successfully
   */
  bool sendToClient(const std::string& message);

private:
  /**
   * @brief Initialize the server socket
   */
  bool _initSocket();

  /**
   * @brief Accept and handle a client connection
   */
  void _acceptClient();

  /**
   * @brief Handle client communication
   */
  void _handleClient();

  /**
   * @brief Read a line from the client (non-blocking with poll)
   * @param line Output string
   * @return >0 if line read, 0 if no data yet (poll again), <0 if error/disconnect
   */
  int _readLine(std::string& line);

  /**
   * @brief Close client connection
   */
  void _closeClient();

  /**
   * @brief Close server socket
   */
  void _closeServer();

  /**
   * @brief Log a message
   */
  void _log(const std::string& message);

  uint16_t m_port;
  int m_serverFd;
  int m_clientFd;
  std::atomic<bool> m_running;
  std::atomic<bool> m_shouldStop;
  std::string m_lastError;
  std::string m_recvBuffer;
  CommandCallback m_commandHandler;
  std::thread m_serverThread;
};

} // namespace AFM
