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
          if (!m_nextBoard)
          {
            m_nextBoard = std::make_unique<MockElectronicBoard>();
          }
          m_rawBoard = m_nextBoard.get();
          return std::move(m_nextBoard);
        },
        50);
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

    std::string resp = send("SYSTEM:INIT");
    EXPECT_EQ(resp.compare(0, 2, "OK"), 0) << resp;
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

TEST_F(CommandHandlerTest, MeasSincTimeoutReturnsHardwareErrorAndResets)
{
  initHardware(true);

  ON_CALL(*m_rawHw, setDecimation(_)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, loadGenerationSignal(_)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, startMeasurement(_, _)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, isMeasurementComplete()).WillByDefault(Return(false));
  EXPECT_CALL(*m_rawHw, resetMeasurement()).Times(AtLeast(1)).WillRepeatedly(Return(true));

  std::string resp = send("MEASURE:SINC 200,100");
  EXPECT_NE(resp.find("ERR_HARDWARE"), std::string::npos);
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

TEST_F(CommandHandlerTest, SweepTimeoutReturnsHardwareErrorAndResets)
{
  initHardware(true);

  ON_CALL(*m_rawHw, setDecimation(_)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, loadGenerationSignal(_)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, startMeasurement(_, _)).WillByDefault(Return(true));
  ON_CALL(*m_rawHw, isMeasurementComplete()).WillByDefault(Return(false));
  EXPECT_CALL(*m_rawHw, resetMeasurement()).Times(AtLeast(1)).WillRepeatedly(Return(true));

  std::string resp = send("MEASURE:SWEEP 200,10");
  EXPECT_NE(resp.find("ERR_HARDWARE"), std::string::npos);
  EXPECT_NE(resp.find("timeout"), std::string::npos);
  EXPECT_FALSE(m_handler->getStatus().measurementInProgress);
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
        out = "OUT1->IN1 GAIN x2";
        return true;
      }));
  std::string resp = send("BOARD:STATUS?");
  EXPECT_NE(resp.find("OUT1->IN1 GAIN x2"), std::string::npos);
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

} // namespace
