#include "CommandHandler.h"
#include "Protocol.h"

#include "mocks/MockElectronicBoard.h"
#include "mocks/MockRedPitayaHardware.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using ::testing::_;
using ::testing::AtLeast;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::Throw;
using ::testing::ReturnRef;
using ::testing::SizeIs;
using ::testing::WithArg;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace
{

class CommandHandlerTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    m_nextHw = std::make_unique<MockRedPitayaHardware>();
    m_nextBoard = std::make_unique<MockElectronicBoard>();
    m_rawHw = m_nextHw.get();
    m_rawBoard = m_nextBoard.get();

    createHandler();
  }

  void createHandler(AFM::OperatingMode mode = AFM::OperatingMode::FULL)
  {
    m_handler = std::make_unique<AFM::CommandHandler>(
        [this]() -> std::unique_ptr<IRedPitayaHardware>
        {
          if (!m_nextHw)
          {
            m_nextHw = std::make_unique<MockRedPitayaHardware>();
          }
          m_rawHw = m_nextHw.get();
          return std::move(m_nextHw);
        },
        [this]() -> std::unique_ptr<IElectronicBoard>
        {
          ++m_boardFactoryCalls;
          if (!m_nextBoard)
          {
            m_nextBoard = std::make_unique<MockElectronicBoard>();
          }
          m_rawBoard = m_nextBoard.get();
          return std::move(m_nextBoard);
        },
        50, mode);
  }

  MockRedPitayaHardware* stageHardware()
  {
    m_nextHw = std::make_unique<MockRedPitayaHardware>();
    return m_nextHw.get();
  }

  MockElectronicBoard* stageBoard()
  {
    m_nextBoard = std::make_unique<MockElectronicBoard>();
    return m_nextBoard.get();
  }

  void initHardware(bool boardConnected = true)
  {
    ON_CALL(*m_rawHw, initialize()).WillByDefault(Return(true));
    ON_CALL(*m_rawHw, getDecimation()).WillByDefault(Return(64));
    ON_CALL(*m_rawBoard, initialize()).WillByDefault(Return(boardConnected));
    // Default board state: all gains at x1 (matching BOARD:RESET defaults)
    ON_CALL(*m_rawBoard, queryGains(_, _))
        .WillByDefault(Invoke([](std::array<GainSetting, 4>& gains,
                                 std::array<bool, 4>& valid) {
          gains.fill(GainSetting::GAIN_1);
          valid.fill(true);
          return true;
        }));
    // Default board state: outputs disconnected
    ON_CALL(*m_rawBoard, queryRouting(_, _))
        .WillByDefault(Invoke([](std::array<uint8_t, 4>& routing,
                                 std::array<bool, 4>& valid) {
          routing.fill(IElectronicBoard::DISCONNECTED);
          valid.fill(true);
          return true;
        }));

    std::string resp = send("SYSTEM:INIT");
    EXPECT_EQ(resp.compare(0, 2, "OK"), 0) << resp;
  }

  void allowBoardCommands()
  {
    ON_CALL(*m_rawBoard, setMuxRoute(_, _)).WillByDefault(Return(true));
    ON_CALL(*m_rawBoard, disconnectMux(_)).WillByDefault(Return(true));
    ON_CALL(*m_rawBoard, setGain(_, _)).WillByDefault(Return(true));
    ON_CALL(*m_rawBoard, reset()).WillByDefault(Return(true));
  }

  void primeSuccessfulMeasurement(uint32_t numSamples)
  {
    ON_CALL(*m_rawHw, setDecimation(_)).WillByDefault(Return(true));
    ON_CALL(*m_rawHw, loadGenerationSignal(_)).WillByDefault(Return(true));
    ON_CALL(*m_rawHw, startMeasurement(_, _)).WillByDefault(Return(true));
    ON_CALL(*m_rawHw, isMeasurementComplete()).WillByDefault(Return(true));
    ON_CALL(*m_rawHw, resetMeasurement()).WillByDefault(Return(true));
    ON_CALL(*m_rawHw, getAcquiredSignal(_))
        .WillByDefault(WithArg<0>(Invoke([numSamples](std::vector<float>& s) {
          s.resize(numSamples);
          for (uint32_t i = 0; i < numSamples; ++i)
            s[i] =
                static_cast<float>(0.5 * sin(2.0 * M_PI * 50.0 * static_cast<double>(i) /
                                             numSamples));
          return true;
        })));
  }

  struct SpectrumRow
  {
    double freqKHz;
    double magnitude;
    double phaseRad;
  };

  std::vector<SpectrumRow> parseSpectrum(const std::string& resp)
  {
    std::vector<SpectrumRow> rows;
    std::istringstream stream(resp);
    std::string line;
    std::getline(stream, line); // header
    while (std::getline(stream, line))
    {
      SpectrumRow row{};
      char separator = 0;
      std::istringstream fields(line);
      if (fields >> row.freqKHz >> separator >> row.magnitude >> separator >> row.phaseRad)
      {
        rows.push_back(row);
      }
    }
    return rows;
  }

  // Acquisition returns amplitude * sin(2 pi f t + phase) at the decimated rate
  void respondWithSine(double freqHz, double amplitude, double phaseRad, uint16_t decimation = 64)
  {
    const double samplingFreq = 125000000.0 / decimation;
    ON_CALL(*m_rawHw, getAcquiredSignal(_))
        .WillByDefault(WithArg<0>(Invoke([=](std::vector<float>& s) {
          for (size_t i = 0; i < s.size(); ++i)
          {
            s[i] = static_cast<float>(
                amplitude * sin(2.0 * M_PI * freqHz * static_cast<double>(i) / samplingFreq +
                                phaseRad));
          }
          return true;
        })));
  }

  bool parseDataHeader(const std::string& resp, size_t& count, size_t& bytes)
  {
    std::istringstream header(resp.substr(0, resp.find('\n')));
    std::string okToken, dataToken;
    header >> okToken >> dataToken >> count >> bytes;
    return okToken == "OK" && dataToken == "DATA";
  }

  std::string send(const std::string& line)
  {
    return m_handler->handleCommand(AFM::parseLine(line));
  }

  std::unique_ptr<MockRedPitayaHardware> m_nextHw;
  std::unique_ptr<MockElectronicBoard> m_nextBoard;
  MockRedPitayaHardware* m_rawHw = nullptr;
  MockElectronicBoard* m_rawBoard = nullptr;
  int m_boardFactoryCalls = 0;
  std::unique_ptr<AFM::CommandHandler> m_handler;
};

TEST_F(CommandHandlerTest, UnknownCommandReturnsSyntaxError)
{
  std::string resp = send("GARBAGE:STUFF");
  EXPECT_NE(resp.find("ERR_SYNTAX"), std::string::npos);
  EXPECT_NE(resp.find("GARBAGE:STUFF"), std::string::npos);
}

TEST_F(CommandHandlerTest, ShutdownIsRejectedByHandler)
{
  // Shutdown is handled by TCPServer; the handler must not crash on it
  EXPECT_NE(send("SYSTEM:SHUTDOWN").find("ERR_SYNTAX"), std::string::npos);
}

TEST_F(CommandHandlerTest, IdnReportsServerIdentity)
{
  std::string resp = send("*IDN?");
  EXPECT_EQ(resp.compare(0, 3, "OK "), 0);
  EXPECT_NE(resp.find("RedPitaya"), std::string::npos);
  EXPECT_NE(resp.find("AFM_SERVER"), std::string::npos);
  EXPECT_NE(resp.find(AFM::VersionInfo::toString()), std::string::npos);
}

TEST_F(CommandHandlerTest, OpcReportsOperationComplete)
{
  EXPECT_EQ(send("*OPC?"), "OK 1\n");
}

TEST_F(CommandHandlerTest, PingRespondsPong)
{
  EXPECT_EQ(send("SYSTEM:PING"), "OK PONG\n");
}

TEST_F(CommandHandlerTest, VersionMatchesProtocolVersion)
{
  EXPECT_EQ(send("SYSTEM:VERSION?"), "OK AFM_SERVER " + AFM::VersionInfo::toString() + "\n");
}

TEST_F(CommandHandlerTest, CommandsBeforeInitReturnNotInitialized)
{
  EXPECT_NE(send("MEASURE:SINC 200,100").find("ERR_NOT_INIT"), std::string::npos);
  EXPECT_NE(send("BOARD:MUX:ROUTE 1,2").find("ERR_NOT_INIT"), std::string::npos);
  EXPECT_NE(send("MEASURE:SWEEP 200,10").find("ERR_NOT_INIT"), std::string::npos);
}

TEST_F(CommandHandlerTest, InitSuccessSetsStatusFlags)
{
  initHardware(true);

  EXPECT_TRUE(m_handler->getStatus().hardwareInitialized);
  EXPECT_TRUE(m_handler->getStatus().boardConnected);

  std::string status = send("SYSTEM:STATUS?");
  EXPECT_NE(status.find("HW_INIT=1"), std::string::npos);
  EXPECT_NE(status.find("BOARD=1"), std::string::npos);
  EXPECT_NE(status.find("DEC=64"), std::string::npos);
  EXPECT_NE(status.find("MODE=FULL"), std::string::npos);
}

TEST_F(CommandHandlerTest, InitWithoutBoardStillSucceeds)
{
  ON_CALL(*m_rawHw, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, getDecimation()).WillByDefault(Return(64));
  ON_CALL(*m_rawBoard, initialize()).WillByDefault(Return(false));

  std::string resp = send("SYSTEM:INIT");
  EXPECT_EQ(resp.compare(0, 2, "OK"), 0) << resp;
  EXPECT_NE(resp.find("NOT CONNECTED"), std::string::npos);

  EXPECT_TRUE(m_handler->getStatus().hardwareInitialized);
  EXPECT_FALSE(m_handler->getStatus().boardConnected);
}

TEST_F(CommandHandlerTest, RpOnlyModeNeverProbesBoard)
{
  createHandler(AFM::OperatingMode::RP_ONLY);
  ON_CALL(*m_rawHw, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, getDecimation()).WillByDefault(Return(64));

  std::string resp = send("SYSTEM:INIT");
  EXPECT_EQ(resp.compare(0, 2, "OK"), 0) << resp;
  EXPECT_NE(resp.find("DISABLED"), std::string::npos);
  EXPECT_EQ(m_boardFactoryCalls, 0);
  EXPECT_TRUE(m_handler->getStatus().hardwareInitialized);
  EXPECT_FALSE(m_handler->getStatus().boardConnected);

  EXPECT_EQ(send("SYSTEM:MODE?"), "OK RP_ONLY\n");
  EXPECT_NE(send("SYSTEM:STATUS?").find("MODE=RP_ONLY"), std::string::npos);
}

TEST_F(CommandHandlerTest, ModeQueryReportsFullMode)
{
  initHardware(true);
  EXPECT_EQ(send("SYSTEM:MODE?"), "OK FULL\n");
  EXPECT_NE(send("SYSTEM:STATUS?").find("MODE=FULL"), std::string::npos);
}

TEST_F(CommandHandlerTest, RpOnlyModeBoardCommandsReportDisabled)
{
  createHandler(AFM::OperatingMode::RP_ONLY);
  ON_CALL(*m_rawHw, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, getDecimation()).WillByDefault(Return(64));
  ASSERT_EQ(send("SYSTEM:INIT").compare(0, 2, "OK"), 0);

  EXPECT_NE(send("BOARD:MUX:ROUTE 1,2").find("RP-only mode"), std::string::npos);
  EXPECT_NE(send("BOARD:MUX:DISCONNECT 1").find("RP-only mode"), std::string::npos);
  EXPECT_NE(send("BOARD:GAIN 1,3").find("RP-only mode"), std::string::npos);
  EXPECT_NE(send("BOARD:RESET").find("RP-only mode"), std::string::npos);
  EXPECT_NE(send("BOARD:STATUS?").find("RP-only mode"), std::string::npos);
}

TEST_F(CommandHandlerTest, RpOnlyModeStillRunsMeasurements)
{
  createHandler(AFM::OperatingMode::RP_ONLY);
  ON_CALL(*m_rawHw, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, getDecimation()).WillByDefault(Return(64));
  ASSERT_EQ(send("SYSTEM:INIT").compare(0, 2, "OK"), 0);
  primeSuccessfulMeasurement(1024);

  std::string resp = send("MEASURE:SINC 200,100,1024,64,1");
  EXPECT_EQ(resp.compare(0, 3, "OK "), 0) << resp;
}

TEST_F(CommandHandlerTest, RstPreservesOperatingMode)
{
  createHandler(AFM::OperatingMode::RP_ONLY);
  EXPECT_EQ(send("SYSTEM:MODE?"), "OK RP_ONLY\n");

  EXPECT_EQ(send("*RST"), "OK\n");
  EXPECT_EQ(send("SYSTEM:MODE?"), "OK RP_ONLY\n");
}

TEST_F(CommandHandlerTest, InitFailureReturnsHardwareError)
{
  ON_CALL(*m_rawHw, initialize()).WillByDefault(Return(false));

  EXPECT_NE(send("SYSTEM:INIT").find("ERR_HARDWARE"), std::string::npos);
  EXPECT_FALSE(m_handler->getStatus().hardwareInitialized);
}

TEST_F(CommandHandlerTest, DeinitResetsState)
{
  initHardware(true);

  EXPECT_EQ(send("SYSTEM:DEINIT").compare(0, 2, "OK"), 0);
  EXPECT_FALSE(m_handler->getStatus().hardwareInitialized);
  EXPECT_NE(send("MEASURE:SINC 200,100").find("ERR_NOT_INIT"), std::string::npos);
}

TEST_F(CommandHandlerTest, RstClearsState)
{
  initHardware(true);

  EXPECT_EQ(send("*RST"), "OK\n");
  EXPECT_FALSE(m_handler->getStatus().hardwareInitialized);
  EXPECT_NE(send("MEASURE:SINC 200,100").find("ERR_NOT_INIT"), std::string::npos);
}

TEST_F(CommandHandlerTest, InitTwiceReportsAlreadyInitialized)
{
  initHardware(true);

  EXPECT_EQ(send("SYSTEM:INIT"), "OK Already initialized\n");
}

TEST_F(CommandHandlerTest, DeinitWhenNotInitializedReportsNotInitialized)
{
  EXPECT_EQ(send("SYSTEM:DEINIT"), "OK Not initialized\n");
}

TEST_F(CommandHandlerTest, DeinitCallsCleanupAndClose)
{
  initHardware(true);

  EXPECT_CALL(*m_rawHw, cleanup()).Times(1);
  EXPECT_CALL(*m_rawBoard, close()).Times(1);

  EXPECT_EQ(send("SYSTEM:DEINIT").compare(0, 2, "OK"), 0);
}

TEST_F(CommandHandlerTest, RstCallsCleanupAndClose)
{
  initHardware(true);

  EXPECT_CALL(*m_rawHw, cleanup()).Times(1);
  EXPECT_CALL(*m_rawBoard, close()).Times(1);

  EXPECT_EQ(send("*RST"), "OK\n");
}

TEST_F(CommandHandlerTest, ReinitAfterDeinitCreatesFreshHardware)
{
  initHardware(true);
  EXPECT_EQ(send("SYSTEM:DEINIT").compare(0, 2, "OK"), 0);

  MockRedPitayaHardware* hw2 = stageHardware();
  MockElectronicBoard* board2 = stageBoard();
  ON_CALL(*hw2, initialize()).WillByDefault(Return(true));
  ON_CALL(*hw2, getDecimation()).WillByDefault(Return(128));
  ON_CALL(*board2, initialize()).WillByDefault(Return(true));

  std::string resp = send("SYSTEM:INIT");
  EXPECT_EQ(resp.compare(0, 3, "OK "), 0) << resp;
  EXPECT_EQ(m_rawHw, hw2);
  EXPECT_EQ(m_rawBoard, board2);
  EXPECT_NE(send("SYSTEM:STATUS?").find("DEC=128"), std::string::npos);
}

TEST_F(CommandHandlerTest, MeasSincRequiresArguments)
{
  initHardware(true);
  EXPECT_NE(send("MEASURE:SINC").find("ERR_SYNTAX"), std::string::npos);
}

TEST_F(CommandHandlerTest, MeasSincValidatesSampleCount)
{
  initHardware(true);
  EXPECT_NE(send("MEASURE:SINC 200,100,0").find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(send("MEASURE:SINC 200,100,999999").find("ERR_PARAM"), std::string::npos);
}

TEST_F(CommandHandlerTest, MeasSincValidatesDecimation)
{
  initHardware(true);
  EXPECT_NE(send("MEASURE:SINC 200,100,8192,100").find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(send("MEASURE:SINC 200,100,8192,8").find("ERR_PARAM"), std::string::npos);
}

TEST_F(CommandHandlerTest, MeasSincValidatesCenterBandwidthAmplitude)
{
  initHardware(true);
  EXPECT_NE(send("MEASURE:SINC 0,100").find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(send("MEASURE:SINC 200,0").find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(send("MEASURE:SINC 200,100,8192,64,1.5").find("ERR_PARAM"), std::string::npos);
}

TEST_F(CommandHandlerTest, MeasSincRejectsEmptyArgumentWithoutShifting)
{
  initHardware(true);

  EXPECT_CALL(*m_rawHw, setDecimation(_)).Times(0);

  // Missing bandwidth must not shift 100 into the bandwidth position
  std::string resp = send("MEASURE:SINC 200,,100");
  EXPECT_NE(resp.find("ERR_SYNTAX"), std::string::npos);
}

TEST_F(CommandHandlerTest, MeasSincRejectsSubHertzBandwidth)
{
  initHardware(true);

  EXPECT_CALL(*m_rawHw, setDecimation(_)).Times(0);

  // 0.0001 kHz = 0.1 Hz would truncate to 0 in the generator API
  std::string resp = send("MEASURE:SINC 200,0.0001");
  EXPECT_NE(resp.find("ERR_PARAM"), std::string::npos);
  EXPECT_EQ(resp.find("Internal error"), std::string::npos);
}

TEST_F(CommandHandlerTest, MeasSincSampleCountBoundaries)
{
  initHardware(true);
  primeSuccessfulMeasurement(65536);

  EXPECT_NE(send("MEASURE:SINC 200,100,65537").find("ERR_PARAM"), std::string::npos);

  std::string resp = send("MEASURE:SINC 200,100,65536");
  EXPECT_EQ(resp.compare(0, 3, "OK "), 0) << resp;
}

TEST_F(CommandHandlerTest, MeasSincDecimationBoundaries)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);

  EXPECT_NE(send("MEASURE:SINC 200,100,8192,15").find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(send("MEASURE:SINC 200,100,8192,2048").find("ERR_PARAM"), std::string::npos);

  // dec 16 -> Nyquist 3.9 MHz, dec 1024 -> 61 kHz; both bands stay below Nyquist
  EXPECT_EQ(send("MEASURE:SINC 200,100,8192,16").compare(0, 3, "OK "), 0);
  EXPECT_EQ(send("MEASURE:SINC 50,10,8192,1024").compare(0, 3, "OK "), 0);
}

TEST_F(CommandHandlerTest, MeasSincRejectsBandEdgeAtNyquist)
{
  initHardware(true);

  EXPECT_CALL(*m_rawHw, setDecimation(_)).Times(0);

  // dec 16 -> Nyquist exactly 3906.25 kHz; band edge 3900 + 12.5/2 = 3906.25
  EXPECT_NE(send("MEASURE:SINC 3900,12.5,8192,16").find("ERR_PARAM"), std::string::npos);
}

TEST_F(CommandHandlerTest, MeasSincSingleSampleReturnsParamError)
{
  initHardware(true);
  primeSuccessfulMeasurement(1);

  std::string resp = send("MEASURE:SINC 200,100,1");
  EXPECT_NE(resp.find("ERR_PARAM"), std::string::npos);
  EXPECT_EQ(resp.find("Internal error"), std::string::npos);
  EXPECT_FALSE(m_handler->getStatus().measurementInProgress);
}

TEST_F(CommandHandlerTest, MeasSincNoBinsInRangeReturnsParamError)
{
  initHardware(true);
  primeSuccessfulMeasurement(2);

  // N=2, dec 16 -> only bins at 0 and 3.9 MHz; the 100 kHz band contains none
  std::string resp = send("MEASURE:SINC 100,1,2,16");
  EXPECT_NE(resp.find("ERR_PARAM"), std::string::npos);
  EXPECT_EQ(resp.find("Internal error"), std::string::npos);
}

TEST_F(CommandHandlerTest, MeasSincIgnoresExtraArguments)
{
  initHardware(true);
  primeSuccessfulMeasurement(1024);

  EXPECT_EQ(send("MEASURE:SINC 200,100,1024,64,1,99").compare(0, 3, "OK "), 0);
}

TEST_F(CommandHandlerTest, MeasSincHappyPathReturnsConsistentSpectrum)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);

  EXPECT_CALL(*m_rawHw, setDecimation(64)).WillOnce(Return(true));
  EXPECT_CALL(*m_rawHw, loadGenerationSignal(SizeIs(8192u))).WillOnce(Return(true));
  EXPECT_CALL(*m_rawHw, startMeasurement(8192u, 0u)).WillOnce(Return(true));

  std::string resp = send("MEASURE:SINC 200,100");
  ASSERT_EQ(resp.compare(0, 3, "OK "), 0) << resp;

  size_t count = 0;
  size_t bytes = 0;
  ASSERT_TRUE(parseDataHeader(resp, count, bytes));
  EXPECT_GT(count, 0u);
  EXPECT_EQ(bytes, resp.size() - resp.find('\n') - 1);
  EXPECT_EQ(static_cast<size_t>(std::count(resp.begin(), resp.end(), '\n')), count + 1);
}

TEST_F(CommandHandlerTest, MeasSincTimeoutReturnsTimeoutErrorAndResets)
{
  initHardware(true);

  ON_CALL(*m_rawHw, setDecimation(_)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, loadGenerationSignal(_)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, startMeasurement(_, _)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, isMeasurementComplete()).WillByDefault(Return(false));
  EXPECT_CALL(*m_rawHw, resetMeasurement()).Times(AtLeast(1)).WillRepeatedly(Return(true));

  std::string resp = send("MEASURE:SINC 200,100");
  EXPECT_EQ(resp.rfind("ERR_TIMEOUT", 0), 0u) << resp;
  EXPECT_NE(resp.find("timeout"), std::string::npos);
  EXPECT_FALSE(m_handler->getStatus().measurementInProgress);
}

TEST_F(CommandHandlerTest, MeasSincLoadFailureReturnsHardwareError)
{
  initHardware(true);

  ON_CALL(*m_rawHw, setDecimation(_)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, loadGenerationSignal(_)).WillByDefault(Return(false));

  EXPECT_NE(send("MEASURE:SINC 200,100").find("Failed to load signal"), std::string::npos);
}

TEST_F(CommandHandlerTest, MeasSincStartFailureReturnsHardwareError)
{
  initHardware(true);

  ON_CALL(*m_rawHw, setDecimation(_)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, loadGenerationSignal(_)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, startMeasurement(_, _)).WillByDefault(Return(false));

  EXPECT_NE(send("MEASURE:SINC 200,100").find("Failed to start measurement"),
            std::string::npos);
}

TEST_F(CommandHandlerTest, MeasSincAcquireFailureReturnsHardwareError)
{
  initHardware(true);

  ON_CALL(*m_rawHw, setDecimation(_)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, loadGenerationSignal(_)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, startMeasurement(_, _)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, isMeasurementComplete()).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, resetMeasurement()).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, getAcquiredSignal(_)).WillByDefault(Return(false));

  EXPECT_NE(send("MEASURE:SINC 200,100").find("Failed to read acquired data"),
            std::string::npos);
}

TEST_F(CommandHandlerTest, SweepHappyPathReturnsExpectedPointCount)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);

  std::string resp = send("MEASURE:SWEEP 200,10"); // default step 1 kHz -> 11 points
  ASSERT_EQ(resp.compare(0, 3, "OK "), 0) << resp;

  size_t count = 0;
  size_t bytes = 0;
  ASSERT_TRUE(parseDataHeader(resp, count, bytes));
  EXPECT_EQ(count, 11u);
  EXPECT_EQ(bytes, resp.size() - resp.find('\n') - 1);
}

TEST_F(CommandHandlerTest, MeasCommandsRejectNonFiniteArguments)
{
  initHardware(true);

  EXPECT_NE(send("MEASURE:SINC nan,1").find("ERR_SYNTAX"), std::string::npos);
  EXPECT_NE(send("MEASURE:SINC 200,nan").find("ERR_SYNTAX"), std::string::npos);
  EXPECT_NE(send("MEASURE:SWEEP inf,10").find("ERR_SYNTAX"), std::string::npos);
}

TEST_F(CommandHandlerTest, MeasSincRejectsBandAboveNyquist)
{
  initHardware(true);

  EXPECT_CALL(*m_rawHw, setDecimation(_)).Times(0);

  // Default decimation 64 -> Nyquist 976.56 kHz
  EXPECT_NE(send("MEASURE:SINC 3900,200").find("ERR_PARAM"), std::string::npos);
  // Hardcoded values used to trigger undefined float-to-int conversion
  EXPECT_NE(send("MEASURE:SINC 1e9,1").find("ERR_PARAM"), std::string::npos);
  // decimation 16 -> Nyquist 3906.25 kHz; band edge 4000 kHz exceeds it
  EXPECT_NE(send("MEASURE:SINC 3800,400,8192,16").find("ERR_PARAM"), std::string::npos);
}

TEST_F(CommandHandlerTest, MeasCommandsRejectInvalidOptionalArguments)
{
  initHardware(true);

  EXPECT_NE(send("MEASURE:SINC 200,100,abc").find("ERR_SYNTAX"), std::string::npos);
  EXPECT_NE(send("MEASURE:SINC 200,100,8192,64,nan").find("ERR_SYNTAX"), std::string::npos);
  EXPECT_NE(send("MEASURE:SWEEP 200,10,1,64,inf").find("ERR_SYNTAX"), std::string::npos);
  EXPECT_NE(send("MEASURE:SWEEP 200,10,1,notadec").find("ERR_SYNTAX"), std::string::npos);
}

TEST_F(CommandHandlerTest, SweepRejectsStopFrequencyAboveNyquist)
{
  initHardware(true);

  EXPECT_CALL(*m_rawHw, setDecimation(_)).Times(0);

  EXPECT_NE(send("MEASURE:SWEEP 3900,200").find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(send("MEASURE:SWEEP 200,2000").find("ERR_PARAM"), std::string::npos);
}

TEST_F(CommandHandlerTest, SweepRejectsTooManyPoints)
{
  initHardware(true);

  EXPECT_CALL(*m_rawHw, setDecimation(_)).Times(0);

  std::string resp = send("MEASURE:SWEEP 200,10,0.0001");
  EXPECT_NE(resp.find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(resp.find("4096"), std::string::npos);
  EXPECT_NE(resp.find("points (max"), std::string::npos);
  EXPECT_EQ(resp.find("e+"), std::string::npos);
}

TEST_F(CommandHandlerTest, SweepTimeoutReturnsTimeoutErrorAndResets)
{
  initHardware(true);

  ON_CALL(*m_rawHw, setDecimation(_)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, loadGenerationSignal(_)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, startMeasurement(_, _)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, isMeasurementComplete()).WillByDefault(Return(false));
  EXPECT_CALL(*m_rawHw, resetMeasurement()).Times(AtLeast(1)).WillRepeatedly(Return(true));

  std::string resp = send("MEASURE:SWEEP 200,10");
  EXPECT_EQ(resp.rfind("ERR_TIMEOUT", 0), 0u) << resp;
  EXPECT_NE(resp.find("timeout"), std::string::npos);
  EXPECT_FALSE(m_handler->getStatus().measurementInProgress);
}

TEST_F(CommandHandlerTest, SweepStopsBetweenPointsWhenCancelled)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);

  int checks = 0;
  m_handler->setCancelCheck([&checks]() { return ++checks > 3; });
  EXPECT_CALL(*m_rawHw, startMeasurement(_, _)).Times(3).WillRepeatedly(Return(true));
  EXPECT_CALL(*m_rawHw, resetMeasurement()).Times(AtLeast(1)).WillRepeatedly(Return(true));

  std::string resp = send("MEASURE:SWEEP 200,10"); // 11 points
  EXPECT_EQ(resp, "ERR_ABORTED: Sweep cancelled after 3 points\n");
  EXPECT_FALSE(m_handler->getStatus().measurementInProgress);
}

TEST_F(CommandHandlerTest, SweepCancelledBeforeFirstPointNeverStarts)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);

  m_handler->setCancelCheck([]() { return true; });
  EXPECT_CALL(*m_rawHw, startMeasurement(_, _)).Times(0);

  EXPECT_EQ(send("MEASURE:SWEEP 200,10").rfind("ERR_ABORTED", 0), 0u);
}

TEST_F(CommandHandlerTest, SweepRunsToTheEndWhenNotCancelled)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);

  int checks = 0;
  m_handler->setCancelCheck([&checks]() { ++checks; return false; });

  std::string resp = send("MEASURE:SWEEP 200,10");
  ASSERT_EQ(resp.compare(0, 3, "OK "), 0) << resp;
  EXPECT_EQ(checks, 11);
}

TEST_F(CommandHandlerTest, CancelCheckDoesNotAffectOtherCommands)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);

  m_handler->setCancelCheck([]() { return true; });

  EXPECT_EQ(send("MEASURE:SINC 200,100").compare(0, 3, "OK "), 0);
  EXPECT_EQ(send("SYSTEM:PING").compare(0, 2, "OK"), 0);
}

TEST_F(CommandHandlerTest, SweepStepDoesNotOvershootStop)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);

  std::string resp = send("MEASURE:SWEEP 500,10,7"); // start 495, stop 505 -> 495, 502
  ASSERT_EQ(resp.compare(0, 3, "OK "), 0) << resp;

  size_t count = 0;
  size_t bytes = 0;
  ASSERT_TRUE(parseDataHeader(resp, count, bytes));
  EXPECT_EQ(count, 2u);
  EXPECT_NE(resp.find("495.000"), std::string::npos);
  EXPECT_NE(resp.find("502.000"), std::string::npos);
  EXPECT_EQ(resp.find("509.000"), std::string::npos);
  EXPECT_FALSE(m_handler->getStatus().measurementInProgress);
}

TEST_F(CommandHandlerTest, SweepLastPointStaysBelowNyquist)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);

  // dec 64 -> Nyquist 976.5625 kHz; a point at 977 kHz would exceed stop and Nyquist
  std::string resp = send("MEASURE:SWEEP 970,10,4");
  ASSERT_EQ(resp.compare(0, 3, "OK "), 0) << resp;

  size_t count = 0;
  size_t bytes = 0;
  ASSERT_TRUE(parseDataHeader(resp, count, bytes));
  EXPECT_EQ(count, 3u);
  EXPECT_EQ(resp.find("977.000"), std::string::npos);
  EXPECT_FALSE(m_handler->getStatus().measurementInProgress);
}

TEST_F(CommandHandlerTest, SweepIncludesStopFrequencyWithFractionalStep)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);

  // 0.1, 0.2 and 0.05 are not exact in float: the stop point must survive
  auto rows = parseSpectrum(send("MEASURE:SWEEP 200,10,0.1"));
  ASSERT_EQ(rows.size(), 101u);
  EXPECT_NEAR(rows.front().freqKHz, 195.0, 1e-3);
  EXPECT_NEAR(rows.back().freqKHz, 205.0, 1e-3);

  EXPECT_EQ(parseSpectrum(send("MEASURE:SWEEP 200,10,0.2")).size(), 51u);
  EXPECT_EQ(parseSpectrum(send("MEASURE:SWEEP 200,10,0.05")).size(), 201u);

  rows = parseSpectrum(send("MEASURE:SWEEP 200,1,0.1"));
  ASSERT_EQ(rows.size(), 11u);
  EXPECT_NEAR(rows.back().freqKHz, 200.5, 1e-3);
}

TEST_F(CommandHandlerTest, SweepFrequenciesAreEvenlySpacedAndNeverExceedStop)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);

  auto rows = parseSpectrum(send("MEASURE:SWEEP 200,10,0.1"));
  ASSERT_EQ(rows.size(), 101u);
  for (size_t i = 0; i < rows.size(); ++i)
  {
    EXPECT_NEAR(rows[i].freqKHz, 195.0 + 0.1 * static_cast<double>(i), 1.5e-3) << "point " << i;
    EXPECT_LE(rows[i].freqKHz, 205.0);
  }
}

TEST_F(CommandHandlerTest, SweepFullSizeKeepsLastPoint)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);

  // 4096 points is the maximum: 409.5 / 0.1 must not round down to 4095
  auto rows = parseSpectrum(send("MEASURE:SWEEP 500,409.5,0.1"));
  ASSERT_EQ(rows.size(), 4096u);
  EXPECT_NEAR(rows.back().freqKHz, 704.75, 1e-3);
}

TEST_F(CommandHandlerTest, SweepReportsMagnitudeAndPhaseOfResponse)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);

  // One point at 199.75 kHz; the response lags the excitation by 30 degrees
  respondWithSine(199750.0, 0.5, -M_PI / 6.0);
  auto rows = parseSpectrum(send("MEASURE:SWEEP 200,0.5,1"));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_NEAR(rows[0].freqKHz, 199.75, 1e-3);
  EXPECT_NEAR(rows[0].magnitude, 0.5, 5e-3);
  EXPECT_NEAR(rows[0].phaseRad, -M_PI / 6.0, 1e-2);

  // A lead is reported positive
  respondWithSine(199750.0, 0.25, M_PI / 3.0);
  rows = parseSpectrum(send("MEASURE:SWEEP 200,0.5,1"));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_NEAR(rows[0].magnitude, 0.25, 5e-3);
  EXPECT_NEAR(rows[0].phaseRad, M_PI / 3.0, 1e-2);
}

TEST_F(CommandHandlerTest, SweepLoopbackGivesZeroPhaseAndExcitationAmplitude)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);

  // Acquisition = the generated signal: unit gain, no phase shift
  auto loaded = std::make_shared<std::vector<float>>();
  ON_CALL(*m_rawHw, loadGenerationSignal(_))
      .WillByDefault(Invoke([loaded](const std::vector<float>& signal) {
        *loaded = signal;
        return true;
      }));
  ON_CALL(*m_rawHw, getAcquiredSignal(_))
      .WillByDefault(WithArg<0>(Invoke([loaded](std::vector<float>& s) {
        std::copy_n(loaded->begin(), std::min(s.size(), loaded->size()), s.begin());
        return true;
      })));

  auto rows = parseSpectrum(send("MEASURE:SWEEP 200,10,1,64,0.8"));
  ASSERT_EQ(rows.size(), 11u);
  for (const auto& row : rows)
  {
    EXPECT_NEAR(row.magnitude, 0.8, 1e-2) << row.freqKHz;
    EXPECT_NEAR(row.phaseRad, 0.0, 1e-2) << row.freqKHz;
  }
}

TEST_F(CommandHandlerTest, SweepSinglePointSucceeds)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);

  std::string resp = send("MEASURE:SWEEP 200,0.1,1"); // start 199.95, stop 200.05
  ASSERT_EQ(resp.compare(0, 3, "OK "), 0) << resp;

  size_t count = 0;
  size_t bytes = 0;
  ASSERT_TRUE(parseDataHeader(resp, count, bytes));
  EXPECT_EQ(count, 1u);
  EXPECT_NE(resp.find("199.950"), std::string::npos);
}

TEST_F(CommandHandlerTest, SweepClampsStartToZero)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);

  std::string resp = send("MEASURE:SWEEP 0.2,1,1"); // start clamped to 0, stop 0.7
  ASSERT_EQ(resp.compare(0, 3, "OK "), 0) << resp;

  size_t count = 0;
  size_t bytes = 0;
  ASSERT_TRUE(parseDataHeader(resp, count, bytes));
  EXPECT_EQ(count, 1u);
  EXPECT_NE(resp.find("0.000"), std::string::npos);
}

TEST_F(CommandHandlerTest, SweepMidLoopFailureResetsAndClearsBusy)
{
  initHardware(true);

  EXPECT_CALL(*m_rawHw, setDecimation(_)).WillOnce(Return(true));
  EXPECT_CALL(*m_rawHw, loadGenerationSignal(_)).Times(2).WillRepeatedly(Return(true));
  EXPECT_CALL(*m_rawHw, startMeasurement(_, _)).WillOnce(Return(true)).WillOnce(Return(false));
  EXPECT_CALL(*m_rawHw, isMeasurementComplete()).WillOnce(Return(true));
  EXPECT_CALL(*m_rawHw, getAcquiredSignal(_))
      .WillOnce(WithArg<0>(Invoke([](std::vector<float>& s) {
        s.assign(8192, 0.0f);
        return true;
      })));
  EXPECT_CALL(*m_rawHw, resetMeasurement()).Times(AtLeast(1)).WillRepeatedly(Return(true));

  std::string resp = send("MEASURE:SWEEP 200,1,1"); // 2 points, second start fails
  EXPECT_NE(resp.find("ERR_HARDWARE"), std::string::npos);
  EXPECT_NE(resp.find("Failed to start measurement"), std::string::npos);
  EXPECT_FALSE(m_handler->getStatus().measurementInProgress);
}

TEST_F(CommandHandlerTest, ExceptionFromHardwareClearsBusyFlag)
{
  initHardware(true);

  ON_CALL(*m_rawHw, setDecimation(_)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, loadGenerationSignal(_)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, startMeasurement(_, _)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, isMeasurementComplete()).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, getAcquiredSignal(_)).WillByDefault(Throw(std::runtime_error("boom")));
  EXPECT_CALL(*m_rawHw, resetMeasurement()).Times(AtLeast(1)).WillRepeatedly(Return(true));

  EXPECT_THROW(send("MEASURE:SINC 200,100"), std::runtime_error);
  EXPECT_FALSE(m_handler->getStatus().measurementInProgress);
}

TEST_F(CommandHandlerTest, MeasSincReportsPeakAtSignalFrequencyAndLevel)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);

  // Bin 839 of 8192 at decimation 64: 200.0336 kHz, no scalloping loss
  const double binHz = 125000000.0 / 64.0 / 8192.0;
  const double signalHz = 839.0 * binHz;

  auto peakOf = [&](double amplitude) {
    respondWithSine(signalHz, amplitude, 0.0);
    auto rows = parseSpectrum(send("MEASURE:SINC 200,100,8192,64,1"));
    EXPECT_FALSE(rows.empty());
    SpectrumRow peak{};
    for (const auto& row : rows)
    {
      // Only the requested band is returned
      EXPECT_GE(row.freqKHz, 150.0 - 1e-3);
      EXPECT_LE(row.freqKHz, 250.0 + 1e-3);
      if (row.magnitude > peak.magnitude)
      {
        peak = row;
      }
    }
    return peak;
  };

  // Hann window, amplitude spectrum scaled by 1/N: a sine of amplitude A peaks at A/4
  SpectrumRow peak = peakOf(0.5);
  EXPECT_NEAR(peak.freqKHz, signalHz / 1000.0, 1e-3);
  EXPECT_NEAR(peak.magnitude, 0.125, 1.5e-3);

  // Linear in the signal level
  EXPECT_NEAR(peakOf(0.25).magnitude, 0.0625, 1e-3);
}

TEST_F(CommandHandlerTest, RejectedMeasurementKeepsSaturationEvidence)
{
  initHardware(true);
  primeSuccessfulMeasurement(1024);

  ON_CALL(*m_rawHw, getAcquiredSignal(_))
      .WillByDefault(WithArg<0>(Invoke([](std::vector<float>& s) {
        s.assign(1024, 1.0f);
        return true;
      })));
  EXPECT_EQ(send("MEASURE:SINC 200,100,1024,64,1").compare(0, 3, "OK "), 0);
  EXPECT_NE(send("SYSTEM:STATUS?").find("SAT=1"), std::string::npos);

  // A plain parameter error must not clear what the last acquisition showed
  EXPECT_NE(send("MEASURE:SINC 3900,200").find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(send("MEASURE:SWEEP 970,20").find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(send("SYSTEM:STATUS?").find("SAT=1"), std::string::npos);
}

TEST_F(CommandHandlerTest, MeasSincSucceeds)
{
  initHardware(true);
  primeSuccessfulMeasurement(1024);

  std::string resp = send("MEASURE:SINC 200,100,1024,64,1");
  EXPECT_EQ(resp.compare(0, 3, "OK "), 0) << resp;
}

TEST_F(CommandHandlerTest, BoardMuxRouteConvertsToOneBasedChannels)
{
  initHardware(true);

  EXPECT_CALL(*m_rawBoard, setMuxRoute(1, 2)).WillOnce(Return(true));
  EXPECT_EQ(send("BOARD:MUX:ROUTE 2,3"), "OK\n");
}

TEST_F(CommandHandlerTest, BoardMuxRouteRejectsOutOfRangeChannels)
{
  initHardware(true);
  EXPECT_NE(send("BOARD:MUX:ROUTE 0,1").find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(send("BOARD:MUX:ROUTE 5,1").find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(send("BOARD:MUX:ROUTE 1").find("ERR_SYNTAX"), std::string::npos);
}

TEST_F(CommandHandlerTest, BoardMuxDisconnectConvertsToOneBasedChannel)
{
  initHardware(true);

  EXPECT_CALL(*m_rawBoard, disconnectMux(2)).WillOnce(Return(true));
  EXPECT_EQ(send("BOARD:MUX:DISCONNECT 3"), "OK\n");
}

TEST_F(CommandHandlerTest, BoardGainEchoesGainValue)
{
  initHardware(true);

  EXPECT_CALL(*m_rawBoard, setGain(0, GainSetting::GAIN_2)).WillOnce(Return(true));
  EXPECT_EQ(send("BOARD:GAIN 1,4"), "OK 2\n");
}

TEST_F(CommandHandlerTest, BoardGainRejectsInvalidParameters)
{
  initHardware(true);
  EXPECT_NE(send("BOARD:GAIN 5,3").find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(send("BOARD:GAIN 1,8").find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(send("BOARD:GAIN 1").find("ERR_SYNTAX"), std::string::npos);
}

TEST_F(CommandHandlerTest, BoardFailurePropagatesLastError)
{
  initHardware(true);

  static const std::string c_BoardError = "UART timeout";
  EXPECT_CALL(*m_rawBoard, setGain(_, _)).WillOnce(Return(false));
  ON_CALL(*m_rawBoard, getLastError()).WillByDefault(ReturnRef(c_BoardError));

  std::string resp = send("BOARD:GAIN 1,3");
  EXPECT_NE(resp.find("ERR_HARDWARE"), std::string::npos);
  EXPECT_NE(resp.find("UART timeout"), std::string::npos);
}

TEST_F(CommandHandlerTest, BoardCommandsFailWhenBoardNotConnected)
{
  initHardware(false);

  EXPECT_NE(send("BOARD:MUX:ROUTE 1,2").find("Board not connected"), std::string::npos);
  EXPECT_NE(send("BOARD:GAIN 1,3").find("Board not connected"), std::string::npos);
  EXPECT_NE(send("BOARD:RESET").find("Board not connected"), std::string::npos);
  EXPECT_NE(send("BOARD:STATUS?").find("Board not connected"), std::string::npos);
}

TEST_F(CommandHandlerTest, BoardResetAndStatusForwardToBoard)
{
  initHardware(true);

  EXPECT_CALL(*m_rawBoard, reset()).WillOnce(Return(true));
  EXPECT_EQ(send("BOARD:RESET"), "OK Board reset complete\n");

  EXPECT_CALL(*m_rawBoard, getStatus(_))
      .WillOnce(Invoke([](std::string& out) {
        out = "=== MUX Status ===\nRouting:\n  OUT1 <- IN1";
        return true;
      }));
  EXPECT_EQ(send("BOARD:STATUS?"),
            "OK LINES 3\n=== MUX Status ===\nRouting:\n  OUT1 <- IN1\n");
}

TEST_F(CommandHandlerTest, BoardMuxDisconnectRejectsOutOfRangeChannel)
{
  initHardware(true);
  EXPECT_NE(send("BOARD:MUX:DISCONNECT 0").find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(send("BOARD:MUX:DISCONNECT 5").find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(send("BOARD:MUX:DISCONNECT").find("ERR_SYNTAX"), std::string::npos);
}

TEST_F(CommandHandlerTest, BoardMuxRouteFailurePropagatesError)
{
  initHardware(true);

  static const std::string c_Error = "mux relay stuck";
  EXPECT_CALL(*m_rawBoard, setMuxRoute(_, _)).WillOnce(Return(false));
  ON_CALL(*m_rawBoard, getLastError()).WillByDefault(ReturnRef(c_Error));

  std::string resp = send("BOARD:MUX:ROUTE 1,2");
  EXPECT_NE(resp.find("ERR_HARDWARE"), std::string::npos);
  EXPECT_NE(resp.find("mux relay stuck"), std::string::npos);
}

TEST_F(CommandHandlerTest, BoardResetFailurePropagatesError)
{
  initHardware(true);

  static const std::string c_Error = "reset line low";
  EXPECT_CALL(*m_rawBoard, reset()).WillOnce(Return(false));
  ON_CALL(*m_rawBoard, getLastError()).WillByDefault(ReturnRef(c_Error));

  std::string resp = send("BOARD:RESET");
  EXPECT_NE(resp.find("ERR_HARDWARE"), std::string::npos);
  EXPECT_NE(resp.find("reset line low"), std::string::npos);
}

TEST_F(CommandHandlerTest, BoardStatusFailurePropagatesError)
{
  initHardware(true);

  static const std::string c_Error = "status timeout";
  EXPECT_CALL(*m_rawBoard, getStatus(_)).WillOnce(Return(false));
  ON_CALL(*m_rawBoard, getLastError()).WillByDefault(ReturnRef(c_Error));

  std::string resp = send("BOARD:STATUS?");
  EXPECT_NE(resp.find("ERR_HARDWARE"), std::string::npos);
  EXPECT_NE(resp.find("status timeout"), std::string::npos);
}

TEST_F(CommandHandlerTest, BoardRejectsInvalidArgumentTypes)
{
  initHardware(true);
  EXPECT_NE(send("BOARD:GAIN abc,3").find("ERR_SYNTAX"), std::string::npos);
  EXPECT_NE(send("BOARD:GAIN 1,x").find("ERR_SYNTAX"), std::string::npos);
  EXPECT_NE(send("BOARD:MUX:ROUTE 1.5,2").find("ERR_SYNTAX"), std::string::npos);
}

TEST_F(CommandHandlerTest, AdcLoopCommandsBeforeInitReturnNotInitialized)
{
  EXPECT_NE(send("BOARD:ADC:LOOP 1,3").find("ERR_NOT_INIT"), std::string::npos);
  EXPECT_NE(send("BOARD:ADC:LOOP OFF").find("ERR_NOT_INIT"), std::string::npos);
}

TEST_F(CommandHandlerTest, AdcLoopRequiresConnectedBoard)
{
  initHardware(false);

  EXPECT_NE(send("BOARD:ADC:LOOP 1,3").find("Board not connected"), std::string::npos);
  EXPECT_NE(send("BOARD:ADC:LOOP OFF").find("Board not connected"), std::string::npos);
}

TEST_F(CommandHandlerTest, AdcLoopRejectedInRpOnlyMode)
{
  createHandler(AFM::OperatingMode::RP_ONLY);
  ON_CALL(*m_rawHw, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, getDecimation()).WillByDefault(Return(64));
  ASSERT_EQ(send("SYSTEM:INIT").compare(0, 2, "OK"), 0);

  EXPECT_NE(send("BOARD:ADC:LOOP 1,3").find("RP-only mode"), std::string::npos);
}

TEST_F(CommandHandlerTest, AdcLoopRejectsInvalidArguments)
{
  initHardware(true);

  EXPECT_NE(send("BOARD:ADC:LOOP 0,3").find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(send("BOARD:ADC:LOOP 1,5").find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(send("BOARD:ADC:LOOP 1").find("ERR_SYNTAX"), std::string::npos);
  EXPECT_NE(send("BOARD:ADC:LOOP x,3").find("ERR_SYNTAX"), std::string::npos);
}

TEST_F(CommandHandlerTest, AdcLoopConfigureQueryAndStatus)
{
  initHardware(true);

  EXPECT_EQ(send("BOARD:ADC:LOOP?"), "OK AUTO\n");
  EXPECT_EQ(send("BOARD:ADC:LOOP 1,3"), "OK 1,3\n");
  EXPECT_EQ(send("BOARD:ADC:LOOP?"), "OK 1,3\n");

  std::string status = send("SYSTEM:STATUS?");
  EXPECT_NE(status.find("LOOP=1,3"), std::string::npos);
  EXPECT_NE(status.find("OVERDRIVE=0"), std::string::npos);
  EXPECT_NE(status.find("SAT=0"), std::string::npos);
}

TEST_F(CommandHandlerTest, AdcLoopOffClearsMonitorAndWarning)
{
  initHardware(true);

  EXPECT_EQ(send("BOARD:ADC:LOOP 1,3"), "OK 1,3\n");
  EXPECT_CALL(*m_rawBoard, setGain(0, GainSetting::GAIN_16)).WillOnce(Return(true));
  EXPECT_NE(send("BOARD:GAIN 1,7").find("WARN"), std::string::npos);
  EXPECT_NE(send("SYSTEM:STATUS?").find("OVERDRIVE=1"), std::string::npos);

  EXPECT_EQ(send("BOARD:ADC:LOOP OFF"), "OK ADC loop monitor off\n");
  EXPECT_EQ(send("BOARD:ADC:LOOP?"), "OK OFF\n");
  EXPECT_EQ(send("SYSTEM:STATUS?").find("OVERDRIVE=1"), std::string::npos);
  EXPECT_NE(send("SYSTEM:STATUS?").find("LOOP=OFF"), std::string::npos);
}

TEST_F(CommandHandlerTest, BoardGainWarnsWhenExciteBoostSaturatesAdc)
{
  initHardware(true);
  EXPECT_EQ(send("BOARD:ADC:LOOP 1,3"), "OK 1,3\n");

  // x16 boost at default amplitude 1.0 clamps at the board rail; the
  // suggestion falls back to the excite channel (x1 keeps ret x1 safe)
  EXPECT_CALL(*m_rawBoard, setGain(0, GainSetting::GAIN_16)).WillOnce(Return(true));
  std::string resp = send("BOARD:GAIN 1,7");
  EXPECT_EQ(resp.compare(0, 6, "OK 16 "), 0) << resp;
  EXPECT_NE(resp.find("WARN"), std::string::npos);
  EXPECT_NE(resp.find("10.50"), std::string::npos);
  EXPECT_NE(resp.find("suggested: BOARD:GAIN 1,3"), std::string::npos);
  EXPECT_NE(resp.find("excitation amplitude <= 0.06"), std::string::npos);
  EXPECT_NE(send("SYSTEM:STATUS?").find("OVERDRIVE=1"), std::string::npos);
}

TEST_F(CommandHandlerTest, BoardGainWarnSuggestsSafeReturnGain)
{
  initHardware(true);
  EXPECT_EQ(send("BOARD:ADC:LOOP 1,3"), "OK 1,3\n");

  // Excite channel x2 drives 2 V to the AFM; a x1 return channel passes it on
  EXPECT_CALL(*m_rawBoard, setGain(0, GainSetting::GAIN_2)).WillOnce(Return(true));
  EXPECT_EQ(send("BOARD:GAIN 1,4").compare(0, 5, "OK 2 "), 0);

  EXPECT_CALL(*m_rawBoard, setGain(2, GainSetting::GAIN_1)).WillOnce(Return(true));
  std::string resp = send("BOARD:GAIN 3,3");
  EXPECT_NE(resp.find("WARN"), std::string::npos);
  EXPECT_NE(resp.find("2.00"), std::string::npos);
  EXPECT_NE(resp.find("suggested: BOARD:GAIN 3,2"), std::string::npos);

  // Applying the suggested x1/2 return gain clears the warning
  EXPECT_CALL(*m_rawBoard, setGain(2, GainSetting::GAIN_1_2)).WillOnce(Return(true));
  EXPECT_EQ(send("BOARD:GAIN 3,2"), "OK 1/2\n");
  EXPECT_NE(send("SYSTEM:STATUS?").find("OVERDRIVE=0"), std::string::npos);
}

TEST_F(CommandHandlerTest, MeasSincUpdatesOverdriveFromAmplitude)
{
  initHardware(true);
  primeSuccessfulMeasurement(1024);
  EXPECT_EQ(send("BOARD:ADC:LOOP 1,3"), "OK 1,3\n");

  EXPECT_CALL(*m_rawBoard, setGain(0, GainSetting::GAIN_2)).WillOnce(Return(true));
  EXPECT_EQ(send("BOARD:GAIN 1,4").compare(0, 5, "OK 2 "), 0);
  EXPECT_NE(send("SYSTEM:STATUS?").find("OVERDRIVE=1"), std::string::npos);

  // Reduced amplitude brings the predicted peak back inside full scale
  std::string resp = send("MEASURE:SINC 200,100,1024,64,0.4");
  EXPECT_EQ(resp.compare(0, 3, "OK "), 0) << resp;
  EXPECT_NE(send("SYSTEM:STATUS?").find("OVERDRIVE=0"), std::string::npos);

  // With the override the full amplitude runs
  EXPECT_EQ(send("BOARD:ADC:PROTECT OFF"), "OK OFF\n");
  resp = send("MEASURE:SINC 200,100,1024,64,1");
  EXPECT_EQ(resp.compare(0, 3, "OK "), 0) << resp;
  EXPECT_NE(send("SYSTEM:STATUS?").find("OVERDRIVE=1"), std::string::npos);
}

TEST_F(CommandHandlerTest, MeasSincDetectsSaturation)
{
  initHardware(true);
  primeSuccessfulMeasurement(1024);

  EXPECT_EQ(send("MEASURE:SINC 200,100,1024,64,1").compare(0, 3, "OK "), 0);
  EXPECT_NE(send("SYSTEM:STATUS?").find("SAT=0"), std::string::npos);
  EXPECT_EQ(send("SYSTEM:STATUS?").find("SAT=1"), std::string::npos);

  // Acquisition pinned at full scale must be flagged
  ON_CALL(*m_rawHw, getAcquiredSignal(_))
      .WillByDefault(WithArg<0>(Invoke([](std::vector<float>& s) {
        s.assign(1024, 1.0f);
        return true;
      })));

  EXPECT_EQ(send("MEASURE:SINC 200,100,1024,64,1").compare(0, 3, "OK "), 0);
  EXPECT_NE(send("SYSTEM:STATUS?").find("SAT=1"), std::string::npos);
}

TEST_F(CommandHandlerTest, SweepDetectsSaturation)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);

  ON_CALL(*m_rawHw, getAcquiredSignal(_))
      .WillByDefault(WithArg<0>(Invoke([](std::vector<float>& s) {
        s.assign(8192, -1.0f);
        return true;
      })));

  std::string resp = send("MEASURE:SWEEP 200,1,1");
  EXPECT_EQ(resp.compare(0, 3, "OK "), 0) << resp;
  EXPECT_NE(send("SYSTEM:STATUS?").find("SAT=1"), std::string::npos);
}

TEST_F(CommandHandlerTest, BoardResetClearsOverdriveWarning)
{
  initHardware(true);
  EXPECT_EQ(send("BOARD:ADC:LOOP 1,3"), "OK 1,3\n");

  EXPECT_CALL(*m_rawBoard, setGain(0, GainSetting::GAIN_16)).WillOnce(Return(true));
  EXPECT_NE(send("BOARD:GAIN 1,7").find("WARN"), std::string::npos);
  EXPECT_NE(send("SYSTEM:STATUS?").find("OVERDRIVE=1"), std::string::npos);

  // The board resets all gains to x1, so the predicted peak drops to 1.0 V
  EXPECT_CALL(*m_rawBoard, reset()).WillOnce(Return(true));
  EXPECT_EQ(send("BOARD:RESET"), "OK Board reset complete\n");
  EXPECT_NE(send("SYSTEM:STATUS?").find("OVERDRIVE=0"), std::string::npos);
}

TEST_F(CommandHandlerTest, InitParsesBoardGainStatus)
{
  ON_CALL(*m_rawHw, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, getDecimation()).WillByDefault(Return(64));
  ON_CALL(*m_rawBoard, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawBoard, queryGains(_, _))
      .WillByDefault(Invoke([](std::array<GainSetting, 4>& gains,
                               std::array<bool, 4>& valid) {
        gains.fill(GainSetting::GAIN_1);
        valid.fill(true);
        gains[0] = GainSetting::GAIN_2; // IN1 boosted x2
        return true;
      }));

  ASSERT_EQ(send("SYSTEM:INIT").compare(0, 2, "OK"), 0);

  // Cached IN1 x2 gain must feed the estimate even before BOARD:GAIN is used
  std::string resp = send("BOARD:ADC:LOOP 1,2");
  EXPECT_NE(resp.find("WARN"), std::string::npos);
  EXPECT_NE(resp.find("2.00"), std::string::npos);
  EXPECT_NE(resp.find("suggested: BOARD:GAIN 2,2"), std::string::npos);
}

TEST_F(CommandHandlerTest, InitParsesFractionalGainLabels)
{
  ON_CALL(*m_rawHw, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, getDecimation()).WillByDefault(Return(64));
  ON_CALL(*m_rawBoard, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawBoard, queryGains(_, _))
      .WillByDefault(Invoke([](std::array<GainSetting, 4>& gains,
                               std::array<bool, 4>& valid) {
        gains.fill(GainSetting::GAIN_1);
        valid.fill(true);
        gains[0] = GainSetting::GAIN_16;   // IN1 x16
        gains[1] = GainSetting::GAIN_1_8;  // IN2 x1/8
        return true;
      }));

  ASSERT_EQ(send("SYSTEM:INIT").compare(0, 2, "OK"), 0);

  // IN2 cached as x1/8 attenuates the clamped x16 board drive to ~1.31 V
  std::string resp = send("BOARD:ADC:LOOP 1,2");
  EXPECT_NE(resp.find("WARN"), std::string::npos);
  EXPECT_NE(resp.find("1.31"), std::string::npos);
}

TEST_F(CommandHandlerTest, InitGainQueryFailureFailsSafe)
{
  ON_CALL(*m_rawHw, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, getDecimation()).WillByDefault(Return(64));
  ON_CALL(*m_rawBoard, initialize()).WillByDefault(Return(true));
  // Transport failure: unknown gains must degrade to x16 and warn, not x1
  ON_CALL(*m_rawBoard, queryGains(_, _)).WillByDefault(Return(false));

  ASSERT_EQ(send("SYSTEM:INIT").compare(0, 2, "OK"), 0);

  std::string resp = send("BOARD:ADC:LOOP 1,3");
  EXPECT_NE(resp.find("WARN"), std::string::npos);
  EXPECT_NE(resp.find("10.50"), std::string::npos); // both stages clip at the board rail
  EXPECT_NE(send("SYSTEM:STATUS?").find("OVERDRIVE=1"), std::string::npos);
}

TEST_F(CommandHandlerTest, InitGainQueryInvalidChannelFailsSafe)
{
  ON_CALL(*m_rawHw, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, getDecimation()).WillByDefault(Return(64));
  ON_CALL(*m_rawBoard, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawBoard, queryGains(_, _))
      .WillByDefault(Invoke([](std::array<GainSetting, 4>& gains,
                               std::array<bool, 4>& valid) {
        gains.fill(GainSetting::GAIN_1);
        valid.fill(true);
        valid[0] = false; // IN1 gain unknown
        return true;
      }));

  ASSERT_EQ(send("SYSTEM:INIT").compare(0, 2, "OK"), 0);

  // Unknown excite gain must be estimated as x16 (rail clamped) -> warn
  std::string resp = send("BOARD:ADC:LOOP 1,3");
  EXPECT_NE(resp.find("WARN"), std::string::npos);
  EXPECT_NE(resp.find("10.50"), std::string::npos);
}

TEST_F(CommandHandlerTest, StatusRefreshesOverdriveEstimateAfterInit)
{
  ON_CALL(*m_rawHw, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, getDecimation()).WillByDefault(Return(64));
  ON_CALL(*m_rawBoard, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawBoard, queryGains(_, _))
      .WillByDefault(Invoke([](std::array<GainSetting, 4>& gains,
                               std::array<bool, 4>& valid) {
        gains.fill(GainSetting::GAIN_1);
        valid.fill(true);
        gains[0] = GainSetting::GAIN_2; // IN1 x2: overdriving at amplitude 1.0
        return true;
      }));

  ASSERT_EQ(send("SYSTEM:INIT").compare(0, 2, "OK"), 0);

  // Configure the monitor against the first board (cached IN1 x2)
  EXPECT_EQ(send("BOARD:ADC:LOOP 1,3").compare(0, 3, "OK "), 0);

  MockRedPitayaHardware* hw2 = stageHardware();
  MockElectronicBoard* board2 = stageBoard();
  ON_CALL(*hw2, initialize()).WillByDefault(Return(true));
  ON_CALL(*hw2, getDecimation()).WillByDefault(Return(64));
  ON_CALL(*board2, initialize()).WillByDefault(Return(true));
  ON_CALL(*board2, queryGains(_, _))
      .WillByDefault(Invoke([](std::array<GainSetting, 4>& gains,
                               std::array<bool, 4>& valid) {
        gains.fill(GainSetting::GAIN_1); // board now at default gains
        valid.fill(true);
        return true;
      }));

  EXPECT_EQ(send("SYSTEM:DEINIT").compare(0, 2, "OK"), 0);
  EXPECT_EQ(send("SYSTEM:INIT").compare(0, 3, "OK "), 0);

  // Re-arming the monitor must evaluate against the freshly read gains
  EXPECT_EQ(send("BOARD:ADC:LOOP 1,3"), "OK 1,3\n");
  EXPECT_NE(send("SYSTEM:STATUS?").find("OVERDRIVE=0"), std::string::npos);
}

TEST_F(CommandHandlerTest, AdcLoopBareFormIsSyntaxError)
{
  initHardware(true);
  EXPECT_NE(send("BOARD:ADC:LOOP").find("ERR_SYNTAX"), std::string::npos);
  EXPECT_EQ(send("BOARD:ADC:LOOP?"), "OK AUTO\n");
}

TEST_F(CommandHandlerTest, RstClearsLoopConfiguration)
{
  initHardware(true);
  EXPECT_EQ(send("BOARD:ADC:LOOP 1,3"), "OK 1,3\n");
  EXPECT_EQ(send("*RST"), "OK\n");

  std::string status = send("SYSTEM:STATUS?");
  EXPECT_NE(status.find("LOOP=OFF"), std::string::npos);
  EXPECT_NE(status.find("OVERDRIVE=0"), std::string::npos);
  EXPECT_NE(send("BOARD:ADC:LOOP 1,3").find("ERR_NOT_INIT"), std::string::npos);
}

TEST_F(CommandHandlerTest, GainWithoutLoopMonitorStaysUnchanged)
{
  initHardware(true);

  EXPECT_CALL(*m_rawBoard, setGain(0, GainSetting::GAIN_16)).WillOnce(Return(true));
  EXPECT_EQ(send("BOARD:GAIN 1,7"), "OK 16\n");
  EXPECT_EQ(send("SYSTEM:STATUS?").find("OVERDRIVE=1"), std::string::npos);
}

TEST_F(CommandHandlerTest, AdcMonitorIsOnByDefaultAndFollowsRouting)
{
  initHardware(true);
  allowBoardCommands();

  EXPECT_EQ(send("BOARD:ADC:LOOP?"), "OK AUTO\n");
  EXPECT_EQ(send("BOARD:ADC:PROTECT?"), "OK ON\n");

  // Nothing routed to OUT1: no signal reaches the ADC
  std::string status = send("SYSTEM:STATUS?");
  EXPECT_NE(status.find("LOOP=1,X"), std::string::npos) << status;
  EXPECT_NE(status.find("OVERDRIVE=0"), std::string::npos);
  EXPECT_NE(status.find("PROTECT=1"), std::string::npos);

  // OUT1 <- IN3 makes IN3 the return channel
  EXPECT_EQ(send("BOARD:MUX:ROUTE 1,3"), "OK\n");
  EXPECT_NE(send("SYSTEM:STATUS?").find("LOOP=1,3"), std::string::npos);

  // Other outputs do not change the loop
  EXPECT_EQ(send("BOARD:MUX:ROUTE 3,1"), "OK\n");
  EXPECT_NE(send("SYSTEM:STATUS?").find("LOOP=1,3"), std::string::npos);

  EXPECT_EQ(send("BOARD:MUX:DISCONNECT 1"), "OK\n");
  EXPECT_NE(send("SYSTEM:STATUS?").find("LOOP=1,X"), std::string::npos);
}

TEST_F(CommandHandlerTest, AutoLoopWarnsOnGainWithoutLoopCommand)
{
  initHardware(true);
  allowBoardCommands();
  EXPECT_EQ(send("BOARD:MUX:ROUTE 1,3"), "OK\n");

  // Return x2 at the default amplitude 1.0
  std::string resp = send("BOARD:GAIN 3,4");
  EXPECT_EQ(resp.compare(0, 5, "OK 2 "), 0) << resp;
  EXPECT_NE(resp.find("WARN: expected ADC peak ~2.00 V"), std::string::npos);
  EXPECT_NE(resp.find("suggested: BOARD:GAIN 3,3"), std::string::npos);
  EXPECT_NE(send("SYSTEM:STATUS?").find("OVERDRIVE=1"), std::string::npos);
}

TEST_F(CommandHandlerTest, MuxRouteReplyCarriesOverdriveWarning)
{
  initHardware(true);
  allowBoardCommands();

  // Harmless while nothing is routed to the ADC
  EXPECT_EQ(send("BOARD:GAIN 3,5"), "OK 4\n");

  std::string resp = send("BOARD:MUX:ROUTE 1,3");
  EXPECT_EQ(resp.compare(0, 9, "OK WARN: "), 0) << resp;
  EXPECT_NE(resp.find("~4.00 V"), std::string::npos);

  // Opening the return path clears it
  EXPECT_EQ(send("BOARD:MUX:DISCONNECT 1"), "OK\n");
  EXPECT_NE(send("SYSTEM:STATUS?").find("OVERDRIVE=0"), std::string::npos);
}

TEST_F(CommandHandlerTest, MeasSincRefusedWhenPredictedPeakExceedsFullScale)
{
  initHardware(true);
  primeSuccessfulMeasurement(1024);
  allowBoardCommands();
  EXPECT_EQ(send("BOARD:MUX:ROUTE 1,3"), "OK\n");
  EXPECT_EQ(send("BOARD:GAIN 1,4").compare(0, 5, "OK 2 "), 0);

  // A refused measurement never reaches the hardware
  EXPECT_CALL(*m_rawHw, setDecimation(_)).Times(0);
  EXPECT_CALL(*m_rawHw, loadGenerationSignal(_)).Times(0);
  EXPECT_CALL(*m_rawHw, startMeasurement(_, _)).Times(0);

  std::string resp = send("MEASURE:SINC 200,100,1024,64,1");
  EXPECT_EQ(resp.compare(0, 24, "ERR_PARAM: ADC overdrive"), 0) << resp;
  EXPECT_NE(resp.find("~2.00 V"), std::string::npos);
  EXPECT_NE(resp.find("suggested: BOARD:GAIN 3,2"), std::string::npos);
  EXPECT_NE(resp.find("BOARD:ADC:PROTECT OFF overrides"), std::string::npos);
}

TEST_F(CommandHandlerTest, MeasSincRunsWhenAmplitudeKeepsPeakInRange)
{
  initHardware(true);
  primeSuccessfulMeasurement(1024);
  allowBoardCommands();
  EXPECT_EQ(send("BOARD:MUX:ROUTE 1,3"), "OK\n");
  EXPECT_EQ(send("BOARD:GAIN 1,4").compare(0, 5, "OK 2 "), 0);

  // 0.5 x 2 = full scale exactly: allowed
  std::string resp = send("MEASURE:SINC 200,100,1024,64,0.5");
  EXPECT_EQ(resp.compare(0, 3, "OK "), 0) << resp;
  EXPECT_NE(send("SYSTEM:STATUS?").find("OVERDRIVE=0"), std::string::npos);
}

TEST_F(CommandHandlerTest, SweepRefusedWhenPredictedPeakExceedsFullScale)
{
  initHardware(true);
  primeSuccessfulMeasurement(8192);
  allowBoardCommands();
  EXPECT_EQ(send("BOARD:MUX:ROUTE 1,3"), "OK\n");
  EXPECT_EQ(send("BOARD:GAIN 3,6").compare(0, 5, "OK 8 "), 0);

  EXPECT_CALL(*m_rawHw, setDecimation(_)).Times(0);
  EXPECT_CALL(*m_rawHw, loadGenerationSignal(_)).Times(0);

  std::string resp = send("MEASURE:SWEEP 200,2,1,64,0.5");
  EXPECT_EQ(resp.compare(0, 24, "ERR_PARAM: ADC overdrive"), 0) << resp;
  EXPECT_NE(resp.find("~4.00 V"), std::string::npos);
  EXPECT_FALSE(m_handler->getStatus().measurementInProgress);
}

TEST_F(CommandHandlerTest, ProtectOffAllowsOverdrivingMeasurementAndReportsIt)
{
  initHardware(true);
  primeSuccessfulMeasurement(1024);
  allowBoardCommands();
  EXPECT_EQ(send("BOARD:MUX:ROUTE 1,3"), "OK\n");
  EXPECT_EQ(send("BOARD:GAIN 1,4").compare(0, 5, "OK 2 "), 0);

  // The override reply carries the warning
  std::string resp = send("BOARD:ADC:PROTECT OFF");
  EXPECT_EQ(resp.compare(0, 7, "OK OFF "), 0) << resp;
  EXPECT_NE(resp.find("WARN"), std::string::npos);
  EXPECT_EQ(send("BOARD:ADC:PROTECT?"), "OK OFF\n");

  resp = send("MEASURE:SINC 200,100,1024,64,1");
  EXPECT_EQ(resp.compare(0, 3, "OK "), 0) << resp;
  std::string status = send("SYSTEM:STATUS?");
  EXPECT_NE(status.find("OVERDRIVE=1"), std::string::npos);
  EXPECT_NE(status.find("PROTECT=0"), std::string::npos);

  // Re-armed: refused again
  EXPECT_EQ(send("BOARD:ADC:PROTECT ON").compare(0, 5, "OK ON"), 0);
  EXPECT_NE(send("MEASURE:SINC 200,100,1024,64,1").find("ADC overdrive"), std::string::npos);
}

TEST_F(CommandHandlerTest, AdcProtectRejectsInvalidArguments)
{
  initHardware(true);

  EXPECT_NE(send("BOARD:ADC:PROTECT").find("ERR_SYNTAX"), std::string::npos);
  EXPECT_NE(send("BOARD:ADC:PROTECT MAYBE").find("ERR_SYNTAX"), std::string::npos);
  EXPECT_NE(send("BOARD:ADC:PROTECT ON,OFF").find("ERR_SYNTAX"), std::string::npos);
  EXPECT_EQ(send("BOARD:ADC:PROTECT off"), "OK OFF\n");
  EXPECT_EQ(send("BOARD:ADC:PROTECT on"), "OK ON\n");
}

TEST_F(CommandHandlerTest, AdcProtectRequiresBoardAndIsRearmedByRst)
{
  EXPECT_NE(send("BOARD:ADC:PROTECT OFF").find("ERR_NOT_INIT"), std::string::npos);

  initHardware(true);
  EXPECT_EQ(send("BOARD:ADC:PROTECT OFF"), "OK OFF\n");
  EXPECT_NE(send("SYSTEM:STATUS?").find("PROTECT=0"), std::string::npos);

  EXPECT_EQ(send("*RST"), "OK\n");
  EXPECT_NE(send("SYSTEM:STATUS?").find("PROTECT=1"), std::string::npos);
}

TEST_F(CommandHandlerTest, RpOnlyModeHasNoAdcMonitor)
{
  createHandler(AFM::OperatingMode::RP_ONLY);
  ON_CALL(*m_rawHw, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, getDecimation()).WillByDefault(Return(64));
  ASSERT_EQ(send("SYSTEM:INIT").compare(0, 2, "OK"), 0);
  primeSuccessfulMeasurement(1024);

  EXPECT_NE(send("BOARD:ADC:PROTECT?").find("RP-only mode"), std::string::npos);
  EXPECT_EQ(send("MEASURE:SINC 200,100,1024,64,1").compare(0, 3, "OK "), 0);
  EXPECT_NE(send("SYSTEM:STATUS?").find("LOOP=OFF"), std::string::npos);
}

TEST_F(CommandHandlerTest, AutoLoopDirectRouteCountsOneGainStage)
{
  initHardware(true);
  allowBoardCommands();

  // IN1 straight to the ADC: its gain applies once
  EXPECT_EQ(send("BOARD:MUX:ROUTE 1,1"), "OK\n");
  std::string resp = send("BOARD:GAIN 1,4");
  EXPECT_NE(resp.find("~2.00 V"), std::string::npos) << resp;
  EXPECT_NE(resp.find("suggested: BOARD:GAIN 1,3 or excitation amplitude <= 0.5000"),
            std::string::npos);
  EXPECT_NE(send("SYSTEM:STATUS?").find("LOOP=1,1"), std::string::npos);
}

TEST_F(CommandHandlerTest, ManualLoopOnOneChannelCountsOneGainStage)
{
  initHardware(true);
  allowBoardCommands();
  EXPECT_EQ(send("BOARD:ADC:LOOP 1,1"), "OK 1,1\n");

  std::string resp = send("BOARD:GAIN 1,7");
  EXPECT_NE(resp.find("~10.50 V"), std::string::npos) << resp;
  EXPECT_NE(resp.find("excitation amplitude <= 0.0625"), std::string::npos);
}

TEST_F(CommandHandlerTest, OverdriveAdviceNeverSuggestsZeroAmplitude)
{
  initHardware(true);
  allowBoardCommands();
  EXPECT_EQ(send("BOARD:ADC:LOOP 1,2"), "OK 1,2\n");
  EXPECT_NE(send("BOARD:GAIN 1,7").find("WARN"), std::string::npos);

  // x16 into x16: peak capped at the rail, advised amplitude non-zero
  std::string resp = send("BOARD:GAIN 2,7");
  EXPECT_NE(resp.find("~10.50 V"), std::string::npos) << resp;
  EXPECT_NE(resp.find("reduce excitation amplitude to <= 0.0039"), std::string::npos);
}

TEST_F(CommandHandlerTest, AdcLoopAutoRestoresRoutingDerivedLoop)
{
  initHardware(true);
  allowBoardCommands();

  EXPECT_EQ(send("BOARD:ADC:LOOP 2,4"), "OK 2,4\n");
  EXPECT_NE(send("SYSTEM:STATUS?").find("LOOP=2,4"), std::string::npos);

  EXPECT_EQ(send("BOARD:ADC:LOOP auto"), "OK AUTO\n");
  EXPECT_EQ(send("BOARD:ADC:LOOP?"), "OK AUTO\n");
  EXPECT_NE(send("SYSTEM:STATUS?").find("LOOP=1,X"), std::string::npos);
}

TEST_F(CommandHandlerTest, InitReadsRoutingFromBoard)
{
  ON_CALL(*m_rawHw, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, getDecimation()).WillByDefault(Return(64));
  ON_CALL(*m_rawBoard, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawBoard, queryGains(_, _))
      .WillByDefault(Invoke([](std::array<GainSetting, 4>& gains,
                               std::array<bool, 4>& valid) {
        gains.fill(GainSetting::GAIN_1);
        valid.fill(true);
        gains[1] = GainSetting::GAIN_4; // IN2 x4
        return true;
      }));
  ON_CALL(*m_rawBoard, queryRouting(_, _))
      .WillByDefault(Invoke([](std::array<uint8_t, 4>& routing,
                               std::array<bool, 4>& valid) {
        routing.fill(IElectronicBoard::DISCONNECTED);
        valid.fill(true);
        routing[0] = 1; // OUT1 <- IN2
        return true;
      }));

  ASSERT_EQ(send("SYSTEM:INIT").compare(0, 2, "OK"), 0);

  std::string status = send("SYSTEM:STATUS?");
  EXPECT_NE(status.find("LOOP=1,2"), std::string::npos) << status;
  EXPECT_NE(status.find("OVERDRIVE=1"), std::string::npos);
}

TEST_F(CommandHandlerTest, UnknownRoutingAssumesWorstCaseReturnChannel)
{
  ON_CALL(*m_rawHw, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, getDecimation()).WillByDefault(Return(64));
  ON_CALL(*m_rawBoard, initialize()).WillByDefault(Return(true));
  ON_CALL(*m_rawBoard, queryGains(_, _))
      .WillByDefault(Invoke([](std::array<GainSetting, 4>& gains,
                               std::array<bool, 4>& valid) {
        gains.fill(GainSetting::GAIN_1);
        valid.fill(true);
        gains[3] = GainSetting::GAIN_4; // IN4 x4
        return true;
      }));
  // Unreadable routing must not count as open
  ON_CALL(*m_rawBoard, queryRouting(_, _)).WillByDefault(Return(false));

  ASSERT_EQ(send("SYSTEM:INIT").compare(0, 2, "OK"), 0);
  primeSuccessfulMeasurement(1024);

  std::string status = send("SYSTEM:STATUS?");
  EXPECT_NE(status.find("LOOP=1,?"), std::string::npos) << status;
  EXPECT_NE(status.find("OVERDRIVE=1"), std::string::npos);

  std::string resp = send("MEASURE:SINC 200,100,1024,64,1");
  EXPECT_NE(resp.find("ADC overdrive"), std::string::npos) << resp;
  EXPECT_NE(resp.find("~4.00 V"), std::string::npos);
  EXPECT_NE(resp.find("board routing unknown, worst case assumed"), std::string::npos);

  // A status reply resolves it
  EXPECT_CALL(*m_rawBoard, getStatus(_))
      .WillOnce(Invoke([](std::string& out) {
        out = "Routing:\n  OUT1 <- X (disconnected)\n  OUT2 <- X (disconnected)\n"
              "Gains:\n  IN1: x1\n  IN2: x1\n  IN3: x1\n  IN4: x4\n==================";
        return true;
      }));
  EXPECT_EQ(send("BOARD:STATUS?").compare(0, 3, "OK "), 0);
  status = send("SYSTEM:STATUS?");
  EXPECT_NE(status.find("LOOP=1,X"), std::string::npos) << status;
  EXPECT_NE(status.find("OVERDRIVE=0"), std::string::npos);
}

TEST_F(CommandHandlerTest, BoardStatusReplyResynchronizesGainAndRoutingCache)
{
  initHardware(true);
  EXPECT_NE(send("SYSTEM:STATUS?").find("LOOP=1,X"), std::string::npos);

  // The board changed without the server knowing
  EXPECT_CALL(*m_rawBoard, getStatus(_))
      .WillOnce(Invoke([](std::string& out) {
        out = "> STATUS\n=== MUX Status ===\nRouting:\n  OUT1 <- IN3\n"
              "  OUT2 <- X (disconnected)\n  OUT3 <- IN1\n  OUT4 <- X (disconnected)\n"
              "Gains:\n  IN1: x1\n  IN2: x1\n  IN3: x8\n  IN4: x1\n==================";
        return true;
      }))
      .WillOnce(Invoke([](std::string& out) {
        out = "something the parser does not understand";
        return true;
      }));

  EXPECT_EQ(send("BOARD:STATUS?").compare(0, 3, "OK "), 0);
  std::string status = send("SYSTEM:STATUS?");
  EXPECT_NE(status.find("LOOP=1,3"), std::string::npos) << status;
  EXPECT_NE(status.find("OVERDRIVE=1"), std::string::npos);

  // An unreadable status keeps the cache
  EXPECT_EQ(send("BOARD:STATUS?").compare(0, 3, "OK "), 0);
  status = send("SYSTEM:STATUS?");
  EXPECT_NE(status.find("LOOP=1,3"), std::string::npos) << status;
  EXPECT_NE(status.find("OVERDRIVE=1"), std::string::npos);
}

TEST_F(CommandHandlerTest, BoardResetOpensReturnPathInCache)
{
  initHardware(true);
  allowBoardCommands();
  EXPECT_EQ(send("BOARD:MUX:ROUTE 1,3"), "OK\n");
  EXPECT_NE(send("BOARD:GAIN 3,7").find("WARN"), std::string::npos);

  EXPECT_EQ(send("BOARD:RESET"), "OK Board reset complete\n");
  std::string status = send("SYSTEM:STATUS?");
  EXPECT_NE(status.find("LOOP=1,X"), std::string::npos) << status;
  EXPECT_NE(status.find("OVERDRIVE=0"), std::string::npos);
}

TEST_F(CommandHandlerTest, FailedRouteLeavesCachedRoutingUnchanged)
{
  initHardware(true);

  static const std::string c_BoardError = "UART timeout";
  EXPECT_CALL(*m_rawBoard, setMuxRoute(0, 2)).WillOnce(Return(false));
  ON_CALL(*m_rawBoard, getLastError()).WillByDefault(ReturnRef(c_BoardError));

  EXPECT_NE(send("BOARD:MUX:ROUTE 1,3").find("ERR_HARDWARE"), std::string::npos);
  EXPECT_NE(send("SYSTEM:STATUS?").find("LOOP=1,X"), std::string::npos);
}

TEST_F(CommandHandlerTest, RefusedMeasurementKeepsSaturationEvidence)
{
  initHardware(true);
  primeSuccessfulMeasurement(1024);
  allowBoardCommands();

  ON_CALL(*m_rawHw, getAcquiredSignal(_))
      .WillByDefault(WithArg<0>(Invoke([](std::vector<float>& s) {
        s.assign(1024, 1.0f);
        return true;
      })));
  EXPECT_EQ(send("MEASURE:SINC 200,100,1024,64,1").compare(0, 3, "OK "), 0);
  EXPECT_NE(send("SYSTEM:STATUS?").find("SAT=1"), std::string::npos);

  EXPECT_EQ(send("BOARD:MUX:ROUTE 1,3"), "OK\n");
  EXPECT_NE(send("BOARD:GAIN 3,4").find("WARN"), std::string::npos);

  // Refused: not run, SAT kept
  EXPECT_NE(send("MEASURE:SINC 200,100,1024,64,1").find("ADC overdrive"), std::string::npos);
  EXPECT_NE(send("SYSTEM:STATUS?").find("SAT=1"), std::string::npos);
}

TEST_F(CommandHandlerTest, SampleGainDefaultsToOneAndIsReported)
{
  initHardware(true);

  EXPECT_EQ(send("BOARD:ADC:SAMPLEGAIN?"), "OK 1\n");
  EXPECT_NE(send("SYSTEM:STATUS?").find("SAMPLEGAIN=1"), std::string::npos);

  EXPECT_EQ(send("BOARD:ADC:SAMPLEGAIN 0.01"), "OK 0.01\n");
  EXPECT_EQ(send("BOARD:ADC:SAMPLEGAIN?"), "OK 0.01\n");
  EXPECT_NE(send("SYSTEM:STATUS?").find("SAMPLEGAIN=0.01"), std::string::npos);
}

TEST_F(CommandHandlerTest, SampleGainAllowsHighReturnGainForWeakSignals)
{
  initHardware(true);
  primeSuccessfulMeasurement(1024);
  allowBoardCommands();
  EXPECT_EQ(send("BOARD:MUX:ROUTE 1,3"), "OK\n");

  // Unity sample assumed: x16 on the return channel is refused
  EXPECT_NE(send("BOARD:GAIN 3,7").find("WARN"), std::string::npos);
  EXPECT_NE(send("MEASURE:SINC 200,100,1024,64,1").find("ADC overdrive"), std::string::npos);

  // Declared 1% return: 1.0 x 0.01 x 16 = 0.16 V
  EXPECT_EQ(send("BOARD:ADC:SAMPLEGAIN 0.01"), "OK 0.01\n");
  EXPECT_NE(send("SYSTEM:STATUS?").find("OVERDRIVE=0"), std::string::npos);
  EXPECT_EQ(send("MEASURE:SINC 200,100,1024,64,1").compare(0, 3, "OK "), 0);
}

TEST_F(CommandHandlerTest, SampleGainAboveOneTightensTheLimit)
{
  initHardware(true);
  primeSuccessfulMeasurement(1024);
  allowBoardCommands();
  EXPECT_EQ(send("BOARD:MUX:ROUTE 1,3"), "OK\n");
  EXPECT_EQ(send("MEASURE:SINC 200,100,1024,64,1").compare(0, 3, "OK "), 0);

  std::string resp = send("BOARD:ADC:SAMPLEGAIN 4");
  EXPECT_EQ(resp.compare(0, 5, "OK 4 "), 0) << resp;
  EXPECT_NE(resp.find("~4.00 V"), std::string::npos);
  EXPECT_NE(resp.find("at sample gain 4"), std::string::npos);
  EXPECT_NE(resp.find("suggested: BOARD:GAIN 3,1"), std::string::npos);

  EXPECT_NE(send("MEASURE:SINC 200,100,1024,64,1").find("ADC overdrive"), std::string::npos);
}

TEST_F(CommandHandlerTest, SampleGainDoesNotApplyToDirectRoute)
{
  initHardware(true);
  allowBoardCommands();
  EXPECT_EQ(send("BOARD:ADC:SAMPLEGAIN 0.001"), "OK 0.001\n");

  // IN1 straight to the ADC bypasses the sample
  EXPECT_EQ(send("BOARD:MUX:ROUTE 1,1"), "OK\n");
  std::string resp = send("BOARD:GAIN 1,4");
  EXPECT_NE(resp.find("~2.00 V"), std::string::npos) << resp;
  EXPECT_EQ(resp.find("sample gain"), std::string::npos);
}

TEST_F(CommandHandlerTest, SampleGainRejectsInvalidArguments)
{
  EXPECT_NE(send("BOARD:ADC:SAMPLEGAIN 0.5").find("ERR_NOT_INIT"), std::string::npos);

  initHardware(true);
  EXPECT_NE(send("BOARD:ADC:SAMPLEGAIN").find("ERR_SYNTAX"), std::string::npos);
  EXPECT_NE(send("BOARD:ADC:SAMPLEGAIN abc").find("ERR_SYNTAX"), std::string::npos);
  EXPECT_NE(send("BOARD:ADC:SAMPLEGAIN nan").find("ERR_SYNTAX"), std::string::npos);
  EXPECT_NE(send("BOARD:ADC:SAMPLEGAIN 1,2").find("ERR_SYNTAX"), std::string::npos);
  EXPECT_NE(send("BOARD:ADC:SAMPLEGAIN 0").find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(send("BOARD:ADC:SAMPLEGAIN -1").find("ERR_PARAM"), std::string::npos);
  EXPECT_NE(send("BOARD:ADC:SAMPLEGAIN 1e9").find("ERR_PARAM"), std::string::npos);

  // Rejected values leave the setting untouched
  EXPECT_EQ(send("BOARD:ADC:SAMPLEGAIN?"), "OK 1\n");
}

TEST_F(CommandHandlerTest, SampleGainIsResetByRst)
{
  initHardware(true);
  EXPECT_EQ(send("BOARD:ADC:SAMPLEGAIN 0.01"), "OK 0.01\n");

  EXPECT_EQ(send("*RST"), "OK\n");
  EXPECT_NE(send("SYSTEM:STATUS?").find("SAMPLEGAIN=1"), std::string::npos);
  EXPECT_EQ(send("SYSTEM:STATUS?").find("SAMPLEGAIN=0.01"), std::string::npos);
}

} // namespace
