#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <numbers>
#include <orbitforge/core/propagation/tle.hpp>
#include <string_view>

namespace orbitforge::core {
namespace {
void check(bool ok, const char *message) {
    if (!ok)
        throw std::invalid_argument(message);
}
bool digit(char c) { return c >= '0' && c <= '9'; }
bool blank(std::string_view value) {
    return std::ranges::all_of(value, [](char c) { return c == ' '; });
}
int integer(std::string_view value) {
    check(!value.empty() && std::ranges::all_of(value, digit), "Invalid TLE integer field");
    int result = 0;
    for (char c : value)
        result = result * 10 + c - '0';
    return result;
}
int padded_integer(std::string_view value) {
    while (!value.empty() && value.front() == ' ')
        value.remove_prefix(1);
    return integer(value);
}
double decimal(std::string_view value) {
    while (!value.empty() && value.front() == ' ')
        value.remove_prefix(1);
    while (!value.empty() && value.back() == ' ')
        value.remove_suffix(1);
    if (!value.empty() && value.front() == '+')
        value.remove_prefix(1);
    double result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result,
                                        std::chars_format::fixed);
    check(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
              std::isfinite(result),
          "Invalid TLE decimal field");
    return result;
}
double implied(std::string_view field) {
    if (blank(field))
        return 0;
    check(field.size() == 8 && (field[0] == ' ' || field[0] == '+' || field[0] == '-') &&
              (field[6] == '+' || field[6] == '-'),
          "Invalid TLE exponent field");
    return (field[0] == '-' ? -1.0 : 1.0) * padded_integer(field.substr(1, 5)) * 1e-5 *
           std::pow(10.0, (field[6] == '-' ? -1 : 1) * integer(field.substr(7, 1)));
}
void line_check(const std::string &line, char number) {
    check(line.size() == 69, "TLE lines must contain exactly 69 ASCII characters");
    check(line[0] == number && line[1] == ' ' && digit(line[68]), "Invalid TLE line number");
    int sum = 0;
    for (std::size_t i = 0; i < 68; ++i) {
        const char c = line[i];
        check(digit(c) || (c >= 'A' && c <= 'Z') || c == ' ' || c == '.' || c == '+' || c == '-',
              "TLE contains an invalid character");
        sum += digit(c) ? c - '0' : c == '-' ? 1 : 0;
    }
    check(sum % 10 == line[68] - '0', "TLE checksum mismatch");
}
} // namespace
TleElements parse_tle(const Tle &tle) {
    line_check(tle.line1, '1');
    line_check(tle.line2, '2');
    const std::string_view a{tle.line1}, b{tle.line2};
    for (const auto i : {8, 17, 32, 43, 52, 61, 63})
        check(a[i] == ' ', "Invalid TLE line 1 column layout");
    for (const auto i : {7, 16, 25, 33, 42, 51})
        check(b[i] == ' ', "Invalid TLE line 2 column layout");
    const auto catalog = a.substr(2, 5);
    check(catalog == b.substr(2, 5), "TLE catalog numbers do not match");
    std::string catalog_id{catalog};
    if (catalog[0] == ' ' || digit(catalog[0])) {
        (void)padded_integer(catalog);
        std::replace(catalog_id.begin(), catalog_id.end(), ' ', '0');
    } else {
        check(catalog[0] >= 'A' && catalog[0] <= 'Z' && catalog[0] != 'I' && catalog[0] != 'O',
              "Invalid TLE catalog ID");
        (void)integer(catalog.substr(1));
    }
    check(a[7] == 'U' || a[7] == 'C' || a[7] == 'S', "Invalid TLE classification");
    check(a[62] == '0', "Only standard SGP4 TLE ephemeris type 0 is supported");
    (void)padded_integer(a.substr(64, 4));
    (void)padded_integer(b.substr(63, 5));
    const int yy = padded_integer(a.substr(18, 2)), year = yy >= 57 ? 1900 + yy : 2000 + yy;
    const int day = padded_integer(a.substr(20, 3));
    check(a[23] == '.', "Invalid TLE epoch fraction");
    const int fraction = integer(a.substr(24, 8));
    using namespace std::chrono;
    const auto jan1 = sys_days{std::chrono::year{year} / January / 1};
    // NORAD day 000 denotes December 31 of the preceding year.
    check(day <= (std::chrono::year{year}.is_leap() ? 366 : 365),
          "TLE epoch day is outside its supported range");
    const auto date = year_month_day{jan1 + days{day - 1}};
    // 1e-8 day is exactly 864000 ns. Avoid subtracting large floating Julian dates.
    const auto ns = static_cast<std::int64_t>(fraction) * 864000;
    const auto sec = ns / 1'000'000'000;
    TleElements result{};
    result.catalog_id = catalog_id;
    result.epoch_utc = {
        static_cast<int>(date.year()),          static_cast<int>(unsigned(date.month())),
        static_cast<int>(unsigned(date.day())), static_cast<int>(sec / 3600),
        static_cast<int>(sec / 60 % 60),        static_cast<int>(sec % 60),
        static_cast<int>(ns % 1'000'000'000)};
    result.epoch_days_since_1950 =
        static_cast<double>((jan1 - sys_days{std::chrono::year{1949} / December / 31}).count()) +
        day - 1 + fraction * 1e-8;
    const double rad = std::numbers::pi / 180;
    result.inclination_rad = decimal(b.substr(8, 8)) * rad;
    result.raan_rad = decimal(b.substr(17, 8)) * rad;
    result.eccentricity = integer(b.substr(26, 7)) * 1e-7;
    result.argument_of_periapsis_rad = decimal(b.substr(34, 8)) * rad;
    result.mean_anomaly_rad = decimal(b.substr(43, 8)) * rad;
    result.mean_motion_revolutions_per_day = decimal(b.substr(52, 11));
    result.mean_motion_dot_over_two = decimal(a.substr(33, 10));
    result.mean_motion_ddot_over_six = implied(a.substr(44, 8));
    result.bstar = implied(a.substr(53, 8));
    check(result.inclination_rad >= 0 && result.inclination_rad <= std::numbers::pi &&
              result.mean_motion_revolutions_per_day > 0,
          "Invalid TLE inclination or mean motion");
    for (double *angle :
         {&result.raan_rad, &result.argument_of_periapsis_rad, &result.mean_anomaly_rad}) {
        check(*angle >= 0 && *angle <= 2 * std::numbers::pi,
              "TLE angles must lie in [0, 360] degrees");
        if (*angle == 2 * std::numbers::pi)
            *angle = 0;
    }
    return result;
}
} // namespace orbitforge::core
