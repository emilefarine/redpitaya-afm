#pragma once

#include "MathConstants.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

/**
 * @brief Least-squares fit of a sine of known frequency
 *
 * Fits x[i] = a * sin(w * i) + b * cos(w * i) + c. Unlike a plain sin / cos
 * correlation, the result is not biased by a DC offset on the input or by a
 * window that holds a non-whole number of periods.
 */
namespace SineFit
{

struct Result
{
  double amplitude; ///< sqrt(a^2 + b^2)
  double phaseRad;  ///< atan2(b, a): phase relative to sin(w * i)
  double offset;    ///< c
};

// Below one period in the window an offset and a sine cannot be told apart
static constexpr double c_MinPeriodsForOffset = 1.0;

namespace detail
{

inline double det3(double a, double b, double c, double d, double e, double f, double g, double h,
                   double k)
{
  return a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
}

} // namespace detail

/**
 * @param samples Acquired samples
 * @param count   Number of samples to use, from the start
 * @param omega   Angular frequency in radians per sample
 */
inline Result fit(const std::vector<float>& samples, size_t count, double omega)
{
  const size_t n = std::min(count, samples.size());
  double ss = 0.0;
  double cc = 0.0;
  double sc = 0.0;
  double s1 = 0.0;
  double c1 = 0.0;
  double xs = 0.0;
  double xc = 0.0;
  double x1 = 0.0;
  for (size_t i = 0; i < n; ++i)
  {
    const double s = std::sin(omega * static_cast<double>(i));
    const double c = std::cos(omega * static_cast<double>(i));
    const double x = samples[i];
    ss += s * s;
    cc += c * c;
    sc += s * c;
    s1 += s;
    c1 += c;
    xs += x * s;
    xc += x * c;
    x1 += x;
  }

  double a = 0.0;
  double b = 0.0;
  double offset = 0.0;
  const double periods = omega * static_cast<double>(n) / (2.0 * c_Pi);
  const double det = detail::det3(ss, sc, s1, sc, cc, c1, s1, c1, static_cast<double>(n));
  if (periods >= c_MinPeriodsForOffset && det != 0.0)
  {
    a = detail::det3(xs, sc, s1, xc, cc, c1, x1, c1, static_cast<double>(n)) / det;
    b = detail::det3(ss, xs, s1, sc, xc, c1, s1, x1, static_cast<double>(n)) / det;
    offset = detail::det3(ss, sc, xs, sc, cc, xc, s1, c1, x1) / det;
  }
  else
  {
    const double det2 = ss * cc - sc * sc;
    if (det2 != 0.0)
    {
      a = (xs * cc - xc * sc) / det2;
      b = (ss * xc - sc * xs) / det2;
    }
  }

  return {std::sqrt(a * a + b * b), std::atan2(b, a), offset};
}

} // namespace SineFit
