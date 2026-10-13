#include <limits>
#include <orbitforge/results/access_windows.hpp>
#include <stdexcept>
#include <utility>

namespace orbitforge::results {
AccessWindow::AccessWindow(core::Epoch start, core::Epoch end, bool start_clipped, bool end_clipped)
    : start_(start), end_(end), start_clipped_(start_clipped), end_clipped_(end_clipped) {
    const auto a = start.tai_nanoseconds(), b = end.tai_nanoseconds();
    const auto span = static_cast<std::uint64_t>(b) - static_cast<std::uint64_t>(a);
    if (b <= a || span > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("Access window requires a positive int64 nanosecond duration");
    duration_ = std::chrono::nanoseconds{static_cast<std::int64_t>(span)};
}
AccessWindows::AccessWindows(std::vector<AccessWindow> windows, AccessQuery query,
                             AccessProvenance provenance)
    : windows_(std::move(windows)), query_(query), provenance_(std::move(provenance)) {
    if (query.first.tai_nanoseconds() > query.last.tai_nanoseconds() ||
        query.search_step.count() <= 0 || query.boundary_tolerance.count() <= 0 ||
        query.boundary_tolerance > query.search_step)
        throw std::invalid_argument("Invalid Access query interval, step or tolerance");
    auto previous_end = query.first;
    for (const auto &window : windows_) {
        if (window.start().tai_nanoseconds() < previous_end.tai_nanoseconds() ||
            window.end().tai_nanoseconds() > query.last.tai_nanoseconds())
            throw std::invalid_argument("Access windows must be ordered within the query interval");
        previous_end = window.end();
    }
}
} // namespace orbitforge::results
