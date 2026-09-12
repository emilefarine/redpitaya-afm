#pragma once

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

/**
 * @brief Interface for the LPC1114 electronic board (MUX routing + gain control)
 *
 * Wire protocol (implemented by ElectronicBoardUART):
 *   Commands are ASCII text terminated by '\n' (LF)
 *   Responses: "OK\r\n" on success, "ERR:<message>\r\n" on error
 *
 * The C++ API uses 0-indexed channel parameters (0-3); the translation to
 * 1-based wire format matching board connector labels is done internally.
 */
class IElectronicBoard
{
public:
  virtual ~IElectronicBoard() = default;

  virtual bool initialize() = 0;
  virtual void close() = 0;
  virtual bool isConnected() const = 0;

  /**
   * @brief Route an input channel to an output channel
   * @param output Output channel (0-3, maps to board connector OUT1-OUT4)
   * @param input Input channel (0-3, maps to board connector IN1-IN4)
   */
  virtual bool setMuxRoute(uint8_t output, uint8_t input) = 0;

  /**
   * @brief Disconnect a multiplexer output
   * @param output Output channel (0-3, maps to board connector OUT1-OUT4)
   */
  virtual bool disconnectMux(uint8_t output) = 0;

  /**
   * @brief Set amplifier gain for a channel
   * @param channel Input channel (0-3, maps to board connector IN1-IN4)
   */
  virtual bool setGain(uint8_t channel, GainSetting gain) = 0;

  /**
   * @brief Request status from the board
   * @param statusOut String to receive status output
   */
  virtual bool getStatus(std::string& statusOut) = 0;

  /** @brief Reset board to defaults (all outputs disconnected, all gains set to 1) */
  virtual bool reset() = 0;

  /** @brief Get last error message */
  virtual const std::string& getLastError() const = 0;

  // Number of MUX channels
  static constexpr uint8_t NUM_CHANNELS = 4;
  static constexpr uint8_t DISCONNECTED = 0xFF;

  /**
   * @brief Get gain value as a human-readable string
   * @param gain Gain setting
   * @return String representation (e.g., "1/4", "1", "8")
   */
  static const char* gainToString(GainSetting gain)
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

  /**
   * @brief Get the numeric gain factor of a setting (x1/8 .. x16)
   * @param gain Gain setting
   * @return Voltage gain factor (0.125 .. 16)
   */
  static float gainFactor(GainSetting gain)
  {
    switch (gain)
    {
    case GainSetting::GAIN_1_8:
      return 0.125f;
    case GainSetting::GAIN_1_4:
      return 0.25f;
    case GainSetting::GAIN_1_2:
      return 0.5f;
    case GainSetting::GAIN_1:
      return 1.0f;
    case GainSetting::GAIN_2:
      return 2.0f;
    case GainSetting::GAIN_4:
      return 4.0f;
    case GainSetting::GAIN_8:
      return 8.0f;
    case GainSetting::GAIN_16:
      return 16.0f;

    default:
      return 1.0f;
    }
  }
};
