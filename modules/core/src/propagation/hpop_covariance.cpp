#include "hpop_sampling.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <orbitforge/core/propagation/hpop_covariance.hpp>
#include <orbitforge/core/propagation/hpop_force.hpp>
#include <utility>

namespace orbitforge::core {
namespace {
using Row = std::array<double, 8>;
using Matrix = std::array<Row, 8>;
struct Joint {
    Matrix covariance{}, factor{};
    std::size_t size = 6;
    std::optional<std::size_t> drag, srp;
};
void check(const std::function<void()> &checkpoint) {
    if (checkpoint)
        checkpoint();
}
double finite(double value) {
    if (!std::isfinite(value))
        throw PropagationError("HPOP covariance is outside the finite double range");
    return value;
}
double normalized(double value, double left, double right) {
    // Dividing by the larger sigma first avoids forming its possibly overflowing
    // or underflowing product. Valid covariance bounds make both divisions safe.
    return value / std::max(left, right) / std::min(left, right);
}
Matrix cholesky(Matrix &covariance, std::size_t size) {
    Row sigma{};
    for (std::size_t i = 0; i < size; ++i) {
        if (!std::isfinite(covariance[i][i]) || !(covariance[i][i] > 0))
            throw std::invalid_argument("Active covariance variances must be finite and positive");
        sigma[i] = std::sqrt(covariance[i][i]);
    }
    Matrix correlation{}, factor{};
    for (std::size_t i = 0; i < size; ++i) {
        correlation[i][i] = 1;
        for (std::size_t j = 0; j < i; ++j) {
            if (!std::isfinite(covariance[i][j]) || !std::isfinite(covariance[j][i]))
                throw std::invalid_argument("Covariance entries must be finite");
            const double a = normalized(covariance[i][j], sigma[i], sigma[j]);
            const double b = normalized(covariance[j][i], sigma[i], sigma[j]);
            if (!std::isfinite(a) || !std::isfinite(b) ||
                std::abs(a - b) > 32 * std::numeric_limits<double>::epsilon())
                throw std::invalid_argument("Covariance must be symmetric in normalized units");
            covariance[i][j] = covariance[j][i] = std::midpoint(covariance[i][j], covariance[j][i]);
            correlation[i][j] = correlation[j][i] =
                normalized(covariance[i][j], sigma[i], sigma[j]);
        }
    }
    for (std::size_t i = 0; i < size; ++i)
        for (std::size_t j = 0; j <= i; ++j) {
            double value = correlation[i][j];
            for (std::size_t k = 0; k < j; ++k)
                value = std::fma(-factor[i][k], factor[j][k], value);
            if (i == j) {
                if (!(value > 0) || !std::isfinite(value))
                    throw std::invalid_argument(
                        "Joint covariance is not positive definite at available precision");
                factor[i][j] = std::sqrt(value);
            } else {
                factor[i][j] = value / factor[j][j];
                if (!std::isfinite(factor[i][j]))
                    throw std::invalid_argument("Covariance Cholesky factor is nonfinite");
            }
        }
    for (std::size_t i = 0; i < size; ++i)
        for (std::size_t j = 0; j <= i; ++j)
            factor[i][j] *= sigma[i];
    return factor;
}
Joint prepare(const HpopCovarianceInput &input) {
    if (input.coordinates.frame != HpopCovarianceFrame::gcrf &&
        input.coordinates.frame != HpopCovarianceFrame::lvlh &&
        input.coordinates.frame != HpopCovarianceFrame::frenet &&
        input.coordinates.frame != HpopCovarianceFrame::fk5_j2000 &&
        input.coordinates.frame != HpopCovarianceFrame::true_of_date)
        throw std::invalid_argument("Unsupported HPOP covariance frame");
    if (input.coordinates.velocity != HpopCovarianceVelocity::projected &&
        input.coordinates.velocity != HpopCovarianceVelocity::rotating)
        throw std::invalid_argument("Unsupported HPOP covariance velocity convention");
    Joint joint;
    for (std::size_t i = 0; i < 6; ++i)
        std::copy(input.state[i].begin(), input.state[i].end(), joint.covariance[i].begin());
    const auto add = [&](const HpopConsiderParameter &parameter) {
        const auto index = joint.size++;
        joint.covariance[index][index] = parameter.variance;
        for (std::size_t i = 0; i < 6; ++i)
            joint.covariance[index][i] = joint.covariance[i][index] =
                parameter.state_cross_covariance[i];
        return index;
    };
    if (input.drag)
        joint.drag = add(*input.drag);
    if (input.srp)
        joint.srp = add(*input.srp);
    if (!std::isfinite(input.drag_srp_covariance) ||
        (input.drag_srp_covariance != 0 && (!joint.drag || !joint.srp)))
        throw std::invalid_argument("D/K covariance requires both active consider parameters");
    if (joint.drag && joint.srp)
        joint.covariance[*joint.drag][*joint.srp] = joint.covariance[*joint.srp][*joint.drag] =
            input.drag_srp_covariance;
    joint.factor = cholesky(joint.covariance, joint.size);
    return joint;
}
// Scaled, compensated dot products avoid intermediate squares/products outside
// double range. frexp/scalbn also handle subnormal covariances without a unit floor.
double product(double a, double b, double c) {
    if (a == 0 || b == 0 || c == 0)
        return 0;
    int ea, eb, ec;
    const double ma = std::frexp(a, &ea), mb = std::frexp(b, &eb), mc = std::frexp(c, &ec);
    return finite(std::scalbn(ma * mb * mc, ea + eb + ec));
}
double dot(const Row &a, const Row &b, std::size_t size) {
    double sa = 0, sb = 0;
    for (std::size_t i = 0; i < size; ++i) {
        sa = std::max(sa, std::abs(a[i]));
        sb = std::max(sb, std::abs(b[i]));
    }
    if (sa == 0 || sb == 0)
        return 0;
    double sum = 0, residual = 0;
    for (std::size_t i = 0; i < size; ++i) {
        const double x = a[i] / sa, y = b[i] / sb, p = x * y, next = sum + p;
        residual += (std::abs(sum) >= std::abs(p) ? (sum - next) + p : (p - next) + sum) +
                    std::fma(x, y, -p);
        sum = next;
    }
    return product(sa, sb, sum + residual);
}
Matrix multiply(const Matrix &a, const Matrix &b, std::size_t rows, std::size_t inner,
                std::size_t columns) {
    Matrix result{};
    for (std::size_t j = 0; j < columns; ++j) {
        Row column{};
        for (std::size_t k = 0; k < inner; ++k)
            column[k] = b[k][j];
        for (std::size_t i = 0; i < rows; ++i)
            result[i][j] = dot(a[i], column, inner);
    }
    return result;
}
Matrix gram(const Matrix &factor, std::size_t rows, std::size_t columns) {
    Matrix result{};
    for (std::size_t i = 0; i < rows; ++i)
        for (std::size_t j = 0; j <= i; ++j)
            result[i][j] = result[j][i] = dot(factor[i], factor[j], columns);
    return result;
}
HpopStateCovariance state_block(const Matrix &matrix) {
    HpopStateCovariance result{};
    for (std::size_t i = 0; i < 6; ++i)
        std::copy_n(matrix[i].begin(), 6, result[i].begin());
    return result;
}
Matrix transition(const HpopVariationalState &value, const Joint &joint) {
    if (value.state.origin != OriginId::earth || value.state.frame != FrameId::gcrf)
        throw std::invalid_argument("HPOP covariance requires Earth/GCRF variational states");
    Matrix result{};
    for (std::size_t i = 0; i < 6; ++i) {
        finite(i < 3 ? value.state.position_m[i] : value.state.velocity_mps[i - 3]);
        for (std::size_t j = 0; j < 6; ++j)
            result[i][j] = finite(value.state_transition[i][j]);
        finite(value.drag_sensitivity[i]);
        finite(value.srp_sensitivity[i]);
        if (joint.drag)
            result[i][*joint.drag] = value.drag_sensitivity[i];
        if (joint.srp)
            result[i][*joint.srp] = value.srp_sensitivity[i];
    }
    for (std::size_t i = 6; i < joint.size; ++i)
        result[i][i] = 1;
    return result;
}
HpopCovarianceInput apply_joint(const HpopCovarianceInput &input, const Joint &joint,
                                const Matrix &f) {
    auto total =
        gram(multiply(f, joint.factor, joint.size, joint.size, joint.size), joint.size, joint.size);
    auto result = input;
    result.state = state_block(total);
    const auto cross = [&](HpopConsiderParameter &parameter, std::size_t index) {
        std::copy_n(total[index].begin(), 6, parameter.state_cross_covariance.begin());
    };
    if (joint.drag)
        cross(*result.drag, *joint.drag);
    if (joint.srp)
        cross(*result.srp, *joint.srp);
    // Constant parameter variances are preserved exactly, including Cov(D,K).
    // Reject an output whose rounded entries no longer resolve a positive joint
    // matrix, rather than silently adding noise or reporting an invalid matrix.
    try {
        (void)prepare(result);
    } catch (const std::invalid_argument &) {
        throw PropagationError("Propagated joint covariance is not numerically positive definite");
    }
    return result;
}
} // namespace
void validate_hpop_covariance(const HpopCovarianceInput &input) { (void)prepare(input); }
namespace {
HpopCovarianceState transport_prepared(const HpopCovarianceInput &input, const Joint &joint,
                                       const HpopVariationalState &variational,
                                       const std::function<void()> &checkpoint) {
    check(checkpoint);
    if (input.coordinates.frame != HpopCovarianceFrame::gcrf)
        throw std::invalid_argument(
            "Convert initial covariance to GCRF before applying a GCRF STM");
    const auto f = transition(variational, joint);
    HpopCovarianceState result{variational, apply_joint(input, joint, f), {}};
    result.covariance.coordinates = {};
    check(checkpoint);
    result.contributions.initial_state =
        state_block(gram(multiply(f, joint.factor, 6, 6, 6), 6, 6));
    const auto parameters = joint.size - 6;
    if (parameters) {
        Matrix pw{}, sensitivity{}, pzw{};
        for (std::size_t i = 0; i < parameters; ++i)
            for (std::size_t j = 0; j < parameters; ++j)
                pw[i][j] = joint.covariance[i + 6][j + 6];
        for (std::size_t i = 0; i < 6; ++i)
            for (std::size_t j = 0; j < parameters; ++j) {
                sensitivity[i][j] = f[i][j + 6];
                pzw[i][j] = joint.covariance[i][j + 6];
            }
        const auto root = cholesky(pw, parameters);
        result.contributions.consider_parameters = state_block(
            gram(multiply(sensitivity, root, 6, parameters, parameters), 6, parameters));
        const auto propagated_cross = multiply(f, pzw, 6, 6, parameters);
        for (std::size_t i = 0; i < 6; ++i)
            for (std::size_t j = 0; j <= i; ++j) {
                const double v = finite(dot(propagated_cross[i], sensitivity[j], parameters) +
                                        dot(propagated_cross[j], sensitivity[i], parameters));
                result.contributions.state_parameter_cross[i][j] =
                    result.contributions.state_parameter_cross[j][i] = v;
            }
    }
    check(checkpoint);
    return result;
}
} // namespace
HpopCovarianceState transport_hpop_covariance(HpopCovarianceInput input,
                                              HpopVariationalState variational,
                                              const std::function<void()> &checkpoint) {
    check(checkpoint);
    return transport_prepared(input, prepare(input), variational, checkpoint);
}
HpopCovarianceInput convert_hpop_covariance(CartesianState reference, HpopCovarianceInput input,
                                            HpopCovarianceCoordinates target,
                                            std::optional<std::array<double, 3>> acceleration_mps2,
                                            const std::function<void()> &checkpoint,
                                            HpopCovarianceFrameEnvironment environment) {
    check(checkpoint);
    const auto joint = prepare(input);
    const auto jacobian = hpop_covariance_frame_jacobian(reference, input.coordinates, target,
                                                         acceleration_mps2, environment);
    Matrix f{};
    for (std::size_t i = 0; i < 6; ++i)
        std::copy(jacobian[i].begin(), jacobian[i].end(), f[i].begin());
    for (std::size_t i = 6; i < joint.size; ++i)
        f[i][i] = 1;
    auto result = apply_joint(input, joint, f);
    result.coordinates = target;
    check(checkpoint);
    return result;
}
namespace {
HpopCovarianceInput initial_gcrf(CartesianState initial, HpopCovarianceInput covariance,
                                 const HpopOptions &options, const HpopEnvironment &environment,
                                 const std::function<void()> &checkpoint) {
    check(checkpoint);
    validate_hpop_covariance(covariance);
    if (covariance.coordinates.frame != HpopCovarianceFrame::gcrf) {
        std::optional<std::array<double, 3>> acceleration;
        if ((covariance.coordinates.frame == HpopCovarianceFrame::lvlh ||
             covariance.coordinates.frame == HpopCovarianceFrame::frenet) &&
            covariance.coordinates.velocity == HpopCovarianceVelocity::rotating)
            acceleration = evaluate_hpop_acceleration(initial, options, environment);
        covariance = convert_hpop_covariance(initial, covariance, {}, acceleration, checkpoint,
                                             {environment.frames, environment.eop});
    }
    return covariance;
}
} // namespace
HpopCovarianceState propagate_hpop_covariance(CartesianState initial, Epoch end,
                                              HpopCovarianceInput covariance, HpopOptions options,
                                              HpopEnvironment environment,
                                              HpopVariationalOptions variational,
                                              const std::function<void()> &checkpoint) {
    covariance = initial_gcrf(initial, std::move(covariance), options, environment, checkpoint);
    const auto state =
        propagate_hpop_variational(initial, end, std::move(options), std::move(environment),
                                   std::move(variational), checkpoint);
    return transport_hpop_covariance(covariance, state, checkpoint);
}
void sample_hpop_covariance(
    CartesianState initial, std::vector<Epoch> times, HpopCovarianceInput covariance,
    const std::function<void(std::size_t, const HpopCovarianceState &)> &sample,
    HpopOptions options, HpopEnvironment environment, HpopVariationalOptions variational,
    const std::function<void()> &checkpoint) {
    check(checkpoint);
    if (times.size() > 200000)
        throw std::invalid_argument("Covariance sampling exceeds 200000 epochs");
    if (!sample)
        throw std::invalid_argument("Covariance sampling requires an output consumer");
    covariance = initial_gcrf(initial, std::move(covariance), options, environment, checkpoint);
    const auto joint = prepare(covariance);
    detail::sample_hpop_variational(
        initial, times, std::move(options), std::move(environment), std::move(variational),
        [&](std::size_t index, const HpopVariationalState &state) {
            const auto result = transport_prepared(covariance, joint, state, checkpoint);
            sample(index, result);
            check(checkpoint);
        },
        checkpoint);
}
} // namespace orbitforge::core
