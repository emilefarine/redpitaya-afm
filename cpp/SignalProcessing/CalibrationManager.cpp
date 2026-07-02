#include "CalibrationManager.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>

CalibrationManager::CalibrationManager()
    : m_calibrated(false)
{
}

void CalibrationManager::setCalibration(const std::vector<float>& freqKHz,
                                        const std::vector<float>& magnitude,
                                        const std::vector<float>& phaseRad,
                                        const CalibrationParams& params)
{
  if (freqKHz.empty() || magnitude.empty() || phaseRad.empty())
  {
    std::cerr << "[CalibrationManager] Error: empty calibration data" << std::endl;
    return;
  }

  if (freqKHz.size() != magnitude.size() || freqKHz.size() != phaseRad.size())
  {
    std::cerr << "[CalibrationManager] Error: vector size mismatch" << std::endl;
    return;
  }

  m_refFreqKHz = freqKHz;
  m_refMagnitude = magnitude;
  m_refPhaseRad = phaseRad;
  m_params = params;
  m_calibrated = true;

  std::cout << "[CalibrationManager] Calibration stored: " << freqKHz.size() << " points, "
            << "center=" << params.centerKHz << " kHz, "
            << "BW=" << params.bandwidthKHz << " kHz, "
            << "dec=" << params.decimation << ", "
            << "N=" << params.numSamples << std::endl;
}

bool CalibrationManager::applyCalibration(std::vector<float>& magnitude,
                                          std::vector<float>& phaseRad,
                                          const std::vector<float>& freqKHz) const
{
  if (!m_calibrated)
  {
    return false;
  }

  if (magnitude.empty() || phaseRad.empty() || freqKHz.empty())
  {
    return false;
  }

  if (magnitude.size() != phaseRad.size() || magnitude.size() != freqKHz.size())
  {
    std::cerr << "[CalibrationManager] Warning: vector size mismatch in applyCalibration"
              << std::endl;
    return false;
  }

  size_t appliedCount = 0;

  for (size_t i = 0; i < freqKHz.size(); ++i)
  {
    float refMag = _interpolateMagnitude(freqKHz[i]);
    float refPhase = _interpolatePhase(freqKHz[i]);

    // Skip bins where the reference is too weak (avoid amplifying noise)
    if (refMag < MIN_REF_MAGNITUDE)
    {
      continue;
    }

    magnitude[i] /= refMag;
    phaseRad[i] -= refPhase;
    ++appliedCount;
  }

  std::cout << "[CalibrationManager] Calibration applied to " << appliedCount << "/"
            << freqKHz.size() << " bins" << std::endl;

  return appliedCount > 0;
}

bool CalibrationManager::isCalibrated() const
{
  return m_calibrated;
}

bool CalibrationManager::isValidFor(const CalibrationParams& params) const
{
  if (!m_calibrated)
  {
    return false;
  }
  return m_params == params;
}

void CalibrationManager::clearCalibration()
{
  m_calibrated = false;
  m_refFreqKHz.clear();
  m_refMagnitude.clear();
  m_refPhaseRad.clear();
  m_params = CalibrationParams();

  std::cout << "[CalibrationManager] Calibration cleared" << std::endl;
}

const CalibrationParams& CalibrationManager::getParams() const
{
  return m_params;
}

std::string CalibrationManager::statusString() const
{
  std::ostringstream oss;

  if (!m_calibrated)
  {
    oss << "NOT_CALIBRATED";
  }
  else
  {
    oss << "CALIBRATED"
        << " DEC=" << m_params.decimation << " N=" << m_params.numSamples
        << " CENTER=" << m_params.centerKHz << " BW=" << m_params.bandwidthKHz
        << " AMP=" << m_params.amplitude << " POINTS=" << m_refFreqKHz.size();
  }

  return oss.str();
}

float CalibrationManager::_interpolateMagnitude(float freqKHz) const
{
  if (m_refFreqKHz.empty())
  {
    return 0.0f;
  }

  if (freqKHz <= m_refFreqKHz.front())
  {
    return m_refMagnitude.front();
  }
  if (freqKHz >= m_refFreqKHz.back())
  {
    return m_refMagnitude.back();
  }

  // Binary search for the interval containing freqKHz
  auto it = std::lower_bound(m_refFreqKHz.begin(), m_refFreqKHz.end(), freqKHz);
  size_t idx = static_cast<size_t>(it - m_refFreqKHz.begin());

  if (idx == 0)
  {
    return m_refMagnitude[0];
  }

  // Linear interpolation between idx-1 and idx
  float f0 = m_refFreqKHz[idx - 1];
  float f1 = m_refFreqKHz[idx];
  float m0 = m_refMagnitude[idx - 1];
  float m1 = m_refMagnitude[idx];

  float t = (freqKHz - f0) / (f1 - f0);
  return m0 + t * (m1 - m0);
}

float CalibrationManager::_interpolatePhase(float freqKHz) const
{
  if (m_refFreqKHz.empty())
  {
    return 0.0f;
  }

  if (freqKHz <= m_refFreqKHz.front())
  {
    return m_refPhaseRad.front();
  }
  if (freqKHz >= m_refFreqKHz.back())
  {
    return m_refPhaseRad.back();
  }

  // Binary search for the interval containing freqKHz
  auto it = std::lower_bound(m_refFreqKHz.begin(), m_refFreqKHz.end(), freqKHz);
  size_t idx = static_cast<size_t>(it - m_refFreqKHz.begin());

  if (idx == 0)
  {
    return m_refPhaseRad[0];
  }

  // Linear interpolation between idx-1 and idx
  float f0 = m_refFreqKHz[idx - 1];
  float f1 = m_refFreqKHz[idx];
  float p0 = m_refPhaseRad[idx - 1];
  float p1 = m_refPhaseRad[idx];

  float t = (freqKHz - f0) / (f1 - f0);
  return p0 + t * (p1 - p0);
}
