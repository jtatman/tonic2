// Gradient-free minimisation over the unit box [0,1]^n with CMA-ES (Hansen's "purecma" formulation,
// full covariance). Patch parameters interact (cutoff with envelope amount, level with drive), so the
// full covariance matters; for the 2..30 parameters of a patch the O(n^3) eigen-decomposition per
// generation is negligible next to rendering audio.
//
// Samples outside the box are repaired by clamping. Random numbers come from mt19937_64 (fully specified
// by the standard) and a hand-written Box-Muller transform, so a seed gives the same search on every run
// and thread count; across compilers results agree up to libm rounding.
// Population members are evaluated in parallel when threads > 1.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

namespace tonic::analysis {

struct CmaesOptions {
  double sigma0 = 0.25;   // initial step size in unit-box coordinates
  int maxEvals = 400;
  int lambda = 0;         // population size; 0 = 4 + 3 ln n
  uint64_t seed = 1;
  int threads = 1;
  double targetValue = -1.0;  // stop early once the best value is at or below this (if >= 0)
};

struct CmaesResult {
  std::vector<double> best;
  double bestValue = 0.0;
  int evals = 0;
  int generations = 0;
};

class NormalSource {
 public:
  explicit NormalSource(uint64_t seed) : rng_(seed) {}
  double operator()() {
    if (hasSpare_) { hasSpare_ = false; return spare_; }
    double u1 = 0.0;
    while (u1 <= 0.0) u1 = uniform();
    const double u2 = uniform(), r = std::sqrt(-2.0 * std::log(u1)), a = 6.283185307179586 * u2;
    spare_ = r * std::sin(a);
    hasSpare_ = true;
    return r * std::cos(a);
  }

 private:
  double uniform() { return static_cast<double>(rng_() >> 11) * (1.0 / 9007199254740992.0); }
  std::mt19937_64 rng_;
  double spare_ = 0.0;
  bool hasSpare_ = false;
};

// Symmetric eigen-decomposition by cyclic Jacobi rotations. a is n x n row-major and is destroyed;
// on return eigenvalues are in `values` and eigenvectors are the columns of `vectors`.
inline void jacobiEigen(std::vector<double> a, size_t n, std::vector<double>& values, std::vector<double>& vectors) {
  vectors.assign(n * n, 0.0);
  for (size_t i = 0; i < n; ++i) vectors[i * n + i] = 1.0;
  for (int sweep = 0; sweep < 64; ++sweep) {
    double off = 0.0;
    for (size_t p = 0; p < n; ++p)
      for (size_t q = p + 1; q < n; ++q) off += a[p * n + q] * a[p * n + q];
    if (off < 1e-30) break;
    for (size_t p = 0; p < n; ++p)
      for (size_t q = p + 1; q < n; ++q) {
        const double apq = a[p * n + q];
        if (std::fabs(apq) < 1e-300) continue;
        const double theta = (a[q * n + q] - a[p * n + p]) / (2.0 * apq);
        const double t = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
        for (size_t k = 0; k < n; ++k) {  // A <- A J (columns p, q)
          const double akp = a[k * n + p], akq = a[k * n + q];
          a[k * n + p] = c * akp - s * akq;
          a[k * n + q] = s * akp + c * akq;
        }
        for (size_t k = 0; k < n; ++k) {  // A <- J^T A (rows p, q)
          const double apk = a[p * n + k], aqk = a[q * n + k];
          a[p * n + k] = c * apk - s * aqk;
          a[q * n + k] = s * apk + c * aqk;
        }
        for (size_t k = 0; k < n; ++k) {  // V <- V J
          const double vkp = vectors[k * n + p], vkq = vectors[k * n + q];
          vectors[k * n + p] = c * vkp - s * vkq;
          vectors[k * n + q] = s * vkp + c * vkq;
        }
      }
  }
  values.resize(n);
  for (size_t i = 0; i < n; ++i) values[i] = a[i * n + i];
}

// f must be safe to call from several threads at once when options.threads > 1.
// `progress` (optional) is called after each generation with (evals, best value so far).
inline CmaesResult minimizeCmaes(std::vector<double> x0, const std::function<double(const std::vector<double>&)>& f,
                                 const CmaesOptions& opt = {},
                                 const std::function<void(int, double)>& progress = {}) {
  const size_t n = x0.size();
  CmaesResult res;
  for (double& v : x0) v = std::clamp(v, 0.0, 1.0);
  res.best = x0;
  res.bestValue = f(x0);
  res.evals = 1;
  if (n == 0) return res;

  const double dn = static_cast<double>(n);
  const int lambda = opt.lambda > 0 ? opt.lambda : 4 + static_cast<int>(3.0 * std::log(dn));
  const int mu = lambda / 2;
  std::vector<double> w(static_cast<size_t>(mu));
  for (int i = 0; i < mu; ++i) w[static_cast<size_t>(i)] = std::log(mu + 0.5) - std::log(i + 1.0);
  const double wsum = std::accumulate(w.begin(), w.end(), 0.0);
  for (double& v : w) v /= wsum;
  double w2 = 0.0;
  for (double v : w) w2 += v * v;
  const double mueff = 1.0 / w2;

  const double cs = (mueff + 2.0) / (dn + mueff + 5.0);
  const double ds = 1.0 + 2.0 * std::max(0.0, std::sqrt((mueff - 1.0) / (dn + 1.0)) - 1.0) + cs;
  const double cc = (4.0 + mueff / dn) / (dn + 4.0 + 2.0 * mueff / dn);
  const double c1 = 2.0 / ((dn + 1.3) * (dn + 1.3) + mueff);
  const double cmu = std::min(1.0 - c1, 2.0 * (mueff - 2.0 + 1.0 / mueff) / ((dn + 2.0) * (dn + 2.0) + mueff));
  const double chiN = std::sqrt(dn) * (1.0 - 1.0 / (4.0 * dn) + 1.0 / (21.0 * dn * dn));

  std::vector<double> m = x0, ps(n, 0.0), pc(n, 0.0);
  std::vector<double> C(n * n, 0.0), B(n * n, 0.0), D(n, 1.0);
  for (size_t i = 0; i < n; ++i) C[i * n + i] = B[i * n + i] = 1.0;
  double sigma = opt.sigma0;
  NormalSource normal(opt.seed);

  std::vector<std::vector<double>> xs(static_cast<size_t>(lambda), std::vector<double>(n)), ys = xs;
  std::vector<double> values(static_cast<size_t>(lambda)), z(n), yw(n), tmp(n);
  std::vector<int> order(static_cast<size_t>(lambda));

  while (res.evals + lambda <= opt.maxEvals) {
    for (size_t k = 0; k < xs.size(); ++k) {
      for (size_t j = 0; j < n; ++j) z[j] = D[j] * normal();
      for (size_t i = 0; i < n; ++i) {
        double y = 0.0;
        for (size_t j = 0; j < n; ++j) y += B[i * n + j] * z[j];
        const double x = std::clamp(m[i] + sigma * y, 0.0, 1.0);  // repair into the box
        xs[k][i] = x;
        ys[k][i] = (x - m[i]) / sigma;
      }
    }
    const int threads = std::max(1, std::min(opt.threads, lambda));
    if (threads == 1) {
      for (size_t k = 0; k < xs.size(); ++k) values[k] = f(xs[k]);
    } else {
      std::vector<std::thread> pool;
      for (int t = 0; t < threads; ++t)
        pool.emplace_back([&, t] {
          for (size_t k = static_cast<size_t>(t); k < xs.size(); k += static_cast<size_t>(threads)) values[k] = f(xs[k]);
        });
      for (auto& th : pool) th.join();
    }
    res.evals += lambda;
    ++res.generations;

    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return values[static_cast<size_t>(a)] < values[static_cast<size_t>(b)]; });
    const auto& top = order;
    if (values[static_cast<size_t>(top[0])] < res.bestValue) {
      res.bestValue = values[static_cast<size_t>(top[0])];
      res.best = xs[static_cast<size_t>(top[0])];
    }

    std::fill(yw.begin(), yw.end(), 0.0);
    for (int i = 0; i < mu; ++i)
      for (size_t j = 0; j < n; ++j) yw[j] += w[static_cast<size_t>(i)] * ys[static_cast<size_t>(top[static_cast<size_t>(i)])][j];
    for (size_t j = 0; j < n; ++j) m[j] = std::clamp(m[j] + sigma * yw[j], 0.0, 1.0);

    // ps <- (1-cs) ps + sqrt(cs(2-cs)mueff) C^-1/2 yw,  C^-1/2 = B D^-1 B^T
    for (size_t j = 0; j < n; ++j) {
      double s = 0.0;
      for (size_t i = 0; i < n; ++i) s += B[i * n + j] * yw[i];
      tmp[j] = s / D[j];
    }
    double psNorm2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
      double s = 0.0;
      for (size_t j = 0; j < n; ++j) s += B[i * n + j] * tmp[j];
      ps[i] = (1.0 - cs) * ps[i] + std::sqrt(cs * (2.0 - cs) * mueff) * s;
      psNorm2 += ps[i] * ps[i];
    }
    const double psNorm = std::sqrt(psNorm2);
    const bool hsig = psNorm / std::sqrt(1.0 - std::pow(1.0 - cs, 2.0 * res.generations)) < (1.4 + 2.0 / (dn + 1.0)) * chiN;
    for (size_t j = 0; j < n; ++j) pc[j] = (1.0 - cc) * pc[j] + (hsig ? std::sqrt(cc * (2.0 - cc) * mueff) * yw[j] : 0.0);

    const double keep = 1.0 - c1 - cmu + (hsig ? 0.0 : c1 * cc * (2.0 - cc));
    for (size_t i = 0; i < n; ++i)
      for (size_t j = 0; j <= i; ++j) {
        double rankMu = 0.0;
        for (int k = 0; k < mu; ++k) {
          const auto& y = ys[static_cast<size_t>(top[static_cast<size_t>(k)])];
          rankMu += w[static_cast<size_t>(k)] * y[i] * y[j];
        }
        const double v = keep * C[i * n + j] + c1 * pc[i] * pc[j] + cmu * rankMu;
        C[i * n + j] = C[j * n + i] = v;
      }
    std::vector<double> eig;
    jacobiEigen(C, n, eig, B);
    for (size_t j = 0; j < n; ++j) D[j] = std::sqrt(std::max(eig[j], 1e-20));

    sigma *= std::exp((cs / ds) * (psNorm / chiN - 1.0));
    sigma = std::min(sigma, 1.0);

    if (progress) progress(res.evals, res.bestValue);
    if (opt.targetValue >= 0.0 && res.bestValue <= opt.targetValue) break;
    if (sigma * *std::max_element(D.begin(), D.end()) < 1e-9) break;  // converged
  }
  return res;
}

}  // namespace tonic::analysis
