#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace poker {

// Discounted CFR (Brown & Sandholm, "Solving Imperfect-Information Games via
// Discounted Regret Minimization", 2019). DCFR(1.5, 0, 2) is the paper's
// recommended setting: positive regrets decay a little, negative regrets are
// halved every iteration (so an action that turns good recovers quickly), and
// iteration t's strategy counts in the average with weight proportional to t^2.
struct DcfrParams {
    double alpha = 1.5;
    double beta = 0.0;
    double gamma = 2.0;
};

struct DcfrDiscount {
    double positive = 0.0;  // Multiplies accumulated regrets that are > 0
    double negative = 0.0;  // Multiplies accumulated regrets that are <= 0
    double strategy = 0.0;  // Multiplies the accumulated strategy sum
};

// The factors iteration `iteration` (1, 2, ...) applies to what earlier
// iterations accumulated, before adding its own values:
//     R_t = R_{t-1} * f(R_{t-1}) + r_t,    S_t = S_{t-1} * g + s_t.
// The paper scales the sums at the end of iteration t, after adding r_t, by
// t^a/(t^a+1), t^b/(t^b+1) and (t/(t+1))^g. Scaling at the start of iteration t by
// the factors for t-1 is the same thing, so this returns them with k = t - 1
// (iteration 1 gets k = 0: zero for positive regrets and strategy sums, which are
// still empty then). The sign test reads R_{t-1}, as in the paper. With this
// schedule iteration t's strategy ends up with weight prod_{k=t}^{T-1}
// (k/(k+1))^g = (t/T)^g in the average after T iterations.
// Each information set must be discounted exactly once per iteration.
inline DcfrDiscount dcfr_discount(const DcfrParams& params, int iteration) {
    const double k = static_cast<double>(std::max(0, iteration - 1));
    const double pa = std::pow(k, params.alpha);
    const double pb = std::pow(k, params.beta);  // pow(0, 0) = 1: negative regrets halve from the start
    DcfrDiscount discount;
    discount.positive = pa / (pa + 1.0);
    discount.negative = pb / (pb + 1.0);
    discount.strategy = std::pow(k / (k + 1.0), params.gamma);
    return discount;
}

// An accumulated regret after the discount, before this iteration's regret is added.
inline double discounted_regret(double accumulated, const DcfrDiscount& discount) {
    return accumulated * (accumulated > 0.0 ? discount.positive : discount.negative);
}

// Regret matching over a block laid out action-major: entry a * hands + h is
// action a for hand h. Each hand's strategy is its positive regrets normalised to
// sum 1, or uniform when none is positive. Sums run in double in action order.
// Also normalises a strategy sum (all entries >= 0) into the average strategy.
template <typename R, typename S>
void regret_match(std::span<const R> regrets, int actions, int hands, std::span<S> strategy) {
    const auto n = static_cast<std::size_t>(hands);
    const auto count = static_cast<std::size_t>(actions);
    const double uniform = 1.0 / static_cast<double>(actions);
    for (std::size_t h = 0; h < n; ++h) {
        double sum = 0.0;
        for (std::size_t a = 0; a < count; ++a) sum += std::max(0.0, static_cast<double>(regrets[a * n + h]));
        for (std::size_t a = 0; a < count; ++a) {
            const double positive = std::max(0.0, static_cast<double>(regrets[a * n + h]));
            strategy[a * n + h] = static_cast<S>(sum > 0.0 ? positive / sum : uniform);
        }
    }
}

}  // namespace poker
