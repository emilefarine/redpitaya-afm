#include "TCPServer.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace AFM
{

TCPServer::TCPServer(uint16_t port)
    : m_port(port)
    , m_serverFd(-1)
    , m_clientFd(-1)
    , m_running(false)
    , m_shouldStop(false)
{
}

TCPServer::~TCPServer()
{
  stop();
}

void TCPServer::setCommandHandler(CommandCallback callback)
{
  m_commandHandler = std::move(callback);
}

bool TCPServer::_initSocket()
{
  // Create socket
  m_serverFd = socket(AF_INET, SOCK_STREAM, 0);
  if (m_serverFd < 0)
  {
    m_lastError = "Failed to create socket: " + std::string(strerror(errno));
    return false;
  }

  // Allow address reuse (avoid "Address already in use" on restart)
  int optval = 1;
  if (setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
  {
    m_lastError = "Failed to set SO_REUSEADDR: " + std::string(strerror(errno));
    _closeServer();
    return false;
  }

  // Bind to port
  struct sockaddr_in serverAddr;
  std::memset(&serverAddr, 0, sizeof(serverAddr));
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_addr.s_addr = INADDR_ANY;
  serverAddr.sin_port = htons(m_port);

  if (bind(m_serverFd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0)
  {
    m_lastError =
        "Failed to bind to port " + std::to_string(m_port) + ": " + std::string(strerror(errno));
    _closeServer();
    return false;
  }

  // Listen for connections
  if (listen(m_serverFd, 1) < 0)
  {
    m_lastError = "Failed to listen: " + std::string(strerror(errno));
    _closeServer();
    return false;
  }

  _log("Server listening on port " + std::to_string(m_port));
  return true;
}

bool TCPServer::start()
{
  if (m_running.load())
  {
    m_lastError = "Server already running";
    return false;
  }

  if (!m_commandHandler)
  {
    m_lastError = "No command handler set";
    return false;
  }

  if (!_initSocket())
  {
    return false;
  }

  m_running.store(true);
  m_shouldStop.store(false);

  // Main server loop
  while (!m_shouldStop.load())
  {
    _acceptClient();

    if (m_clientFd >= 0)
    {
      _handleClient();
      _closeClient();
    }
  }

  _closeServer();
  m_running.store(false);
  return true;
}

bool TCPServer::startAsync()
{
  if (m_running.load())
  {
    m_lastError = "Server already running";
    return false;
  }

  if (!m_commandHandler)
  {
    m_lastError = "No command handler set";
    return false;
  }

  std::promise<bool> initPromise;
  std::future<bool> initFuture = initPromise.get_future();

  m_serverThread = std::thread(
      [this, p = std::move(initPromise)]() mutable
      {
        if (!_initSocket())
        {
          p.set_value(false);
          return;
        }

        m_running.store(true);
        m_shouldStop.store(false);
        p.set_value(true); // Signal: server is now listening

        // Main server loop
        while (!m_shouldStop.load())
        {
          _acceptClient();

          if (m_clientFd >= 0)
          {
            _handleClient();
            _closeClient();
          }
        }

        _closeServer();
        m_running.store(false);
      });

  return initFuture.get();
}

void TCPServer::stop()
{
  // 1. Signal stop - the thread will see this within its select() timeout
  m_shouldStop.store(true);

  // 2. Wait for thread to exit cleanly (it will close sockets itself)
  if (m_serverThread.joinable())
  {
    m_serverThread.join();
  }

  // 3. Defensive cleanup: these calls are idempotent (check fd >= 0).
  //    Normally redundant since start()/startAsync() clean up before returning,
  //    but ensures proper cleanup if stop() is called from destructor or
  //    in unexpected error paths.
  _closeClient();
  _closeServer();

  m_running.store(false);
}

void TCPServer::_acceptClient()
{
  // Use select with timeout to allow checking for shutdown
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(m_serverFd, &readfds);

  struct timeval timeout;
  timeout.tv_sec = 1;
  timeout.tv_usec = 0;

  int result = select(m_serverFd + 1, &readfds, nullptr, nullptr, &timeout);

  if (result < 0)
  {
    if (errno != EINTR)
    {
      _log("Select error: " + std::string(strerror(errno)));
    }
    return;
  }

  if (result == 0)
  {
    // Timeout, check if we should stop
    return;
  }

  // Accept connection
  struct sockaddr_in clientAddr;
  socklen_t clientLen = sizeof(clientAddr);

  m_clientFd = accept(m_serverFd, (struct sockaddr*)&clientAddr, &clientLen);
  if (m_clientFd < 0)
  {
    if (errno != EINTR && errno != EBADF)
    {
      _log("Accept error: " + std::string(strerror(errno)));
    }
    return;
  }

  // Set socket timeout
  struct timeval tv;
  tv.tv_sec = ServerConfig::SOCKET_TIMEOUT_SEC;
  tv.tv_usec = 0;
  setsockopt(m_clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(m_clientFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  char clientIP[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, sizeof(clientIP));
  _log("Client connected from " + std::string(clientIP));

  // Send welcome message
  std::string welcome = "AFM SCPI Server v" + VersionInfo::toString() + " Ready\n";
  sendToClient(welcome);
}

void TCPServer::_handleClient()
{
  m_recvBuffer.clear();
  auto lastActivityTime = std::chrono::steady_clock::now();

  while (!m_shouldStop.load() && m_clientFd >= 0)
  {
    std::string line;
    int readResult = _readLine(line);

    if (readResult < 0)
    {
      // Connection closed or error
      break;
    }

    if (readResult == 0)
    {
      // No data yet, check idle timeout
      auto idleTime = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now() - lastActivityTime)
                          .count();
      if (idleTime >= ServerConfig::IDLE_TIMEOUT_SEC)
      {
        _log("Client idle timeout (" + std::to_string(ServerConfig::IDLE_TIMEOUT_SEC) + "s)");
        break;
      }
      continue;
    }

    // Got a line (readResult > 0)
    lastActivityTime = std::chrono::steady_clock::now();

    // Skip empty lines
    if (line.empty())
    {
      continue;
    }

    _log("Received: " + line);

    // Parse and handle command
    ParsedCommand cmd = parseLine(line);

    // Check for shutdown command
    if (cmd.command == Command::SYST_SHUTDOWN)
    {
      sendToClient(buildOkResponse("Shutting down"));
      m_shouldStop.store(true);
      break;
    }

    // Dispatch to handler (catch any exception to prevent server thread crash)
    std::string response;
    try
    {
      response = m_commandHandler(cmd);
    }
    catch (const std::exception& e)
    {
      _log("Exception handling command: " + std::string(e.what()));
      response = buildErrorResponse(ResponseStatus::ERR_UNKNOWN,
                                    "Internal error: " + std::string(e.what()));
    }
    catch (...)
    {
      _log("Unknown exception handling command");
      response = buildErrorResponse(ResponseStatus::ERR_UNKNOWN, "Internal error");
    }

    if (!sendToClient(response))
    {
      break;
    }
  }
}

/**
 * @brief Read a line from the client (non-blocking with poll)
 * @param line Output string for the line read
 * @return >0 if line read, 0 if no data yet (poll again), <0 if error/disconnect
 */
int TCPServer::_readLine(std::string& line)
{
  line.clear();

  // Check if we already have a complete line in buffer
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
    return 1; // Line available
  }

  // Need to read more data - use short timeout to allow shutdown checks
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(m_clientFd, &readfds);

  struct timeval timeout;
  timeout.tv_sec = ServerConfig::SELECT_POLL_MS / 1000;
  timeout.tv_usec = (ServerConfig::SELECT_POLL_MS % 1000) * 1000;

  int result = select(m_clientFd + 1, &readfds, nullptr, nullptr, &timeout);

  if (result < 0)
  {
    if (errno == EINTR)
      return 0; // Interrupted, try again
    return -1;  // Error
  }

  if (result == 0)
  {
    // No data available yet
    return 0;
  }

  // Read available data
  char buffer[ServerConfig::RECV_BUFFER_SIZE];
  ssize_t bytesRead = recv(m_clientFd, buffer, sizeof(buffer) - 1, 0);

  if (bytesRead <= 0)
  {
    if (bytesRead == 0)
    {
      _log("Client disconnected");
    }
    return -1; // Connection closed or error
  }

  buffer[bytesRead] = '\0';

  // Buffer overflow protection: allow accumulating up to 4 recv() calls worth of data
  // before considering it a malformed input (no newline in sight)
  constexpr size_t MAX_LINE_LENGTH = ServerConfig::RECV_BUFFER_SIZE * 4;
  if (m_recvBuffer.size() + bytesRead > MAX_LINE_LENGTH)
  {
    _log("Line too long, rejecting client");
    m_recvBuffer.clear();
    return -1;
  }

  m_recvBuffer += buffer;

  // Check for complete line
  newlinePos = m_recvBuffer.find('\n');
  if (newlinePos != std::string::npos)
  {
    line = m_recvBuffer.substr(0, newlinePos);
    m_recvBuffer.erase(0, newlinePos + 1);

    if (!line.empty() && line.back() == '\r')
    {
      line.pop_back();
    }
    return 1; // Line available
  }

  return 0; // Partial data, need more
}

bool TCPServer::sendToClient(const std::string& message)
{
  if (m_clientFd < 0)
  {
    return false;
  }

  size_t totalSent = 0;
  while (totalSent < message.size())
  {
    ssize_t sent =
        send(m_clientFd, message.data() + totalSent, message.size() - totalSent, MSG_NOSIGNAL);

    if (sent < 0)
    {
      if (errno == EINTR)
        continue;
      _log("Send error: " + std::string(strerror(errno)));
      return false;
    }

    if (sent == 0)
    {
      return false;
    }

    totalSent += static_cast<size_t>(sent);
  }

  return true;
}

void TCPServer::_closeClient()
{
  if (m_clientFd >= 0)
  {
    close(m_clientFd);
    m_clientFd = -1;
    _log("Client connection closed");
  }
}

void TCPServer::_closeServer()
{
  if (m_serverFd >= 0)
  {
    close(m_serverFd);
    m_serverFd = -1;
  }
}

void TCPServer::_log(const std::string& message)
{
  std::cout << "[AFMServer] " << message << std::endl;
}

} // namespace AFM
