#include "FFTProcessor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

FFTProcessor::FFTProcessor(double samplingFrequency)
    : m_samplingFrequency(samplingFrequency)
    , m_forwardPlan(nullptr)
    , m_inputBuffer(nullptr)
    , m_outputBuffer(nullptr)
    , m_planSize(0)
{
  if (samplingFrequency <= 0)
  {
    throw std::invalid_argument("Sampling frequency must be positive");
  }
}

FFTProcessor::~FFTProcessor()
{
  _cleanupPlan();
}

std::vector<std::complex<float>> FFTProcessor::computeFFT(const std::vector<float>& inputSignal)
{
  if (inputSignal.empty())
  {
    throw std::invalid_argument("Input signal cannot be empty");
  }

  const size_t N = inputSignal.size();

  if (m_planSize != N)
  {
    _initializePlan(N);
  }

  std::copy(inputSignal.begin(), inputSignal.end(), m_inputBuffer);

  fftwf_execute(m_forwardPlan);

  size_t numOutputSamples = N / 2 + 1; // Real-to-complex FFT output size
  std::vector<std::complex<float>> result(numOutputSamples);

  for (size_t i = 0; i < numOutputSamples; ++i)
  {
    // FFTW3 complex format: [0] = real, [1] = imaginary
    result[i] = std::complex<float>(m_outputBuffer[i][0] / N, m_outputBuffer[i][1] / N);
  }

  return result;
}

std::vector<float> FFTProcessor::computeMagnitudeSpectrum(
    const std::vector<std::complex<float>>& fftResult) const
{
  std::vector<float> magnitude;
  magnitude.reserve(fftResult.size());

  for (const auto& complexValue : fftResult)
  {
    float mag = std::abs(complexValue);
    magnitude.push_back(mag);
  }

  return magnitude;
}

std::vector<float> FFTProcessor::computePhaseSpectrum(
    const std::vector<std::complex<float>>& fftResult) const
{
  std::vector<float> phase;
  phase.reserve(fftResult.size());

  for (const auto& complexValue : fftResult)
  {
    float ph = std::arg(complexValue);
    phase.push_back(ph);
  }

  return phase;
}

std::vector<float> FFTProcessor::getFrequencyAxis(uint32_t numSamples) const
{
  std::vector<float> frequencies;

  // For real signals, we only need frequencies from 0 to Fs/2
  uint32_t numFreqBins = numSamples / 2 + 1;
  frequencies.reserve(numFreqBins);

  for (uint32_t i = 0; i < numFreqBins; ++i)
  {
    float freq = static_cast<float>(i) * m_samplingFrequency / numSamples;
    frequencies.push_back(freq);
  }

  return frequencies;
}

void FFTProcessor::applyWindow(std::vector<float>& signal, WindowType windowType) const
{
  const size_t N = signal.size();

  switch (windowType)
  {
  case WindowType::Hann:
    // Hann window: w(n) = 0.5 * (1 - cos(2π*n/(N-1)))
    for (size_t i = 0; i < N; ++i)
    {
      double window = 0.5 * (1.0 - cos(2.0 * M_PI * i / (N - 1)));
      signal[i] *= static_cast<float>(window);
    }
    break;

  case WindowType::Hamming:
    // Hamming window: w(n) = 0.54 - 0.46 * cos(2π*n/(N-1))
    for (size_t i = 0; i < N; ++i)
    {
      double window = 0.54 - 0.46 * cos(2.0 * M_PI * i / (N - 1));
      signal[i] *= static_cast<float>(window);
    }
    break;

  case WindowType::Blackman:
    // w(n) = 0.42 - 0.5*cos(2π*n/(N-1)) + 0.08*cos(4π*n/(N-1))
    for (size_t i = 0; i < N; ++i)
    {
      double window =
          0.42 - 0.5 * cos(2.0 * M_PI * i / (N - 1)) + 0.08 * cos(4.0 * M_PI * i / (N - 1));
      signal[i] *= static_cast<float>(window);
    }
    break;

  case WindowType::Rectangle:
    break;

  default:
    throw std::invalid_argument("Unknown window type");
  }
}

void FFTProcessor::_initializePlan(size_t size) const
{
  _cleanupPlan();

  m_planSize = size;
  size_t outputSize = size / 2 + 1; // Real-to-complex FFT output size

  m_inputBuffer = fftwf_alloc_real(size);
  m_outputBuffer = fftwf_alloc_complex(outputSize);

  if (!m_inputBuffer || !m_outputBuffer)
  {
    _cleanupPlan();
    throw std::runtime_error("Failed to allocate FFTW buffers");
  }

  m_forwardPlan =
      fftwf_plan_dft_r2c_1d(static_cast<int>(size), m_inputBuffer, m_outputBuffer, FFTW_ESTIMATE);

  if (!m_forwardPlan)
  {
    _cleanupPlan();
    throw std::runtime_error("Failed to create FFTW plan");
  }
}

void FFTProcessor::_cleanupPlan() const
{
  if (m_forwardPlan)
  {
    fftwf_destroy_plan(m_forwardPlan);
    m_forwardPlan = nullptr;
  }
  if (m_inputBuffer)
  {
    fftwf_free(m_inputBuffer);
    m_inputBuffer = nullptr;
  }
  if (m_outputBuffer)
  {
    fftwf_free(m_outputBuffer);
    m_outputBuffer = nullptr;
  }
  m_planSize = 0;
}