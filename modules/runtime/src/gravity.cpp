#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <orbitforge/runtime/gravity.hpp>
#include <picosha2.h>
#include <sstream>

namespace orbitforge::runtime {
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
std::string lower(std::string_view s) {
    std::string result(s);
    for (auto &c : result)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return result;
}
template <class T> T number(std::string_view s) {
    if (s.starts_with('+'))
        s.remove_prefix(1);
    T value{};
    auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    require(result.ec == std::errc{} && result.ptr == s.data() + s.size() &&
                std::isfinite(static_cast<double>(value)),
            "Invalid gravity numeric field");
    return value;
}
std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> result;
    while (!line.empty()) {
        const auto start = line.find_first_not_of(" \t\r");
        if (start == line.npos || line[start] == '#')
            break;
        line.remove_prefix(start);
        const auto end = line.find_first_of(" \t\r");
        result.push_back(line.substr(0, end));
        if (end == line.npos)
            break;
        line.remove_prefix(end);
    }
    return result;
}
} // namespace
void validate_gravity_reference(std::span<const application::DataReference> recorded,
                                const std::string &selection, const core::GravityInfo &info) {
    const auto selected = gravity_reference(selection, info);
    const auto found =
        std::ranges::find(recorded, selected.role, &application::DataReference::role);
    require(found != recorded.end() && *found == selected,
            "Gravity provider differs from the project's confirmed data; use a preparation plan");
}
std::string gravity_download_url(const std::string &model) {
    if (model == "egm96")
        return "https://icgem.gfz.de/getmodel/gfc/"
               "971b0a3b49a497910aad23cd85e066d4cd9af0aeafe7ce6301a696bed8570be3/EGM96.gfc";
    if (model == "egm2008")
        return "https://icgem.gfz.de/getmodel/gfc/"
               "c50128797a9cb62e936337c890e4425f03f0461d7329b09a8cc8561504465340/EGM2008.gfc";
    throw std::invalid_argument("No online source for this gravity model");
}
std::shared_ptr<const core::GravityModel> load_gravity(const std::filesystem::path &file,
                                                       const std::string &source_url,
                                                       const std::function<void()> &checkpoint) {
    std::ifstream input(file, std::ios::binary);
    require(input.is_open(), "Cannot open gravity file");
    picosha2::hash256_one_by_one hash;
    core::GravityInfo info;
    info.source_url = source_url;
    std::map<std::string, std::string> header;
    std::vector<core::GravityCoefficient> coefficients;
    std::vector<bool> seen;
    bool grv = false, started = false, ended = false, normalized = true;
    std::size_t line_number = 0, bytes = 0, count = 0;
    const auto begin = [&] {
        const auto get = [&](const char *gfc, const char *stk) -> const std::string & {
            const auto it = header.find(grv ? stk : gfc);
            require(it != header.end(), "Gravity header is incomplete");
            return it->second;
        };
        info.model = get("modelname", "model");
        info.mu = number<double>(get("earth_gravity_constant", "gm"));
        info.radius_m = number<double>(get("radius", "refdistance"));
        info.degree = number<int>(get("max_degree", "degree"));
        info.order = grv ? number<int>(get("max_degree", "order")) : info.degree;
        require(info.degree >= 2 && info.degree <= 2190 && info.order >= 0 &&
                    info.order <= info.degree,
                "Gravity file degree/order must be within 2..2190");
        // This loader is for the terrestrial HPOP environment, not lunar/planetary fields.
        require(info.mu > 3.9e14 && info.mu < 4.1e14 && info.radius_m > 6.3e6 &&
                    info.radius_m < 6.5e6,
                "Gravity file does not describe an Earth field in SI units");
        if (grv) {
            const auto norm = lower(get("norm", "normalized"));
            require(norm == "yes" || norm == "no", "Invalid gravity normalization");
            normalized = norm == "yes";
            const auto tide = lower(get("tide_system", "includespermtide"));
            require(tide == "yes" || tide == "no", "Invalid permanent tide designation");
            info.tide_system = tide == "yes" ? "zero_tide" : "tide_free";
        } else {
            require(get("product_type", "") == "gravity_field", "Not an ICGEM gravity field");
            info.tide_system = get("tide_system", "");
            // Fully normalized is the ICGEM format's default when norm is absent (EGM96).
            if (header.contains("norm")) {
                require(header.at("norm") == "fully_normalized" ||
                            header.at("norm") == "unnormalized",
                        "Unsupported ICGEM normalization");
                normalized = header.at("norm") == "fully_normalized";
            }
        }
        seen.resize(static_cast<std::size_t>((info.degree + 1) * (info.degree + 2) / 2));
        started = true;
    };
    for (std::string line; std::getline(input, line);) {
        if ((++line_number % 1024) == 0 && checkpoint)
            checkpoint();
        require(line.size() <= 4096, "Gravity file line exceeds 4096 bytes");
        bytes += line.size() + 1;
        require(bytes <= 512U * 1024 * 1024, "Gravity file exceeds 512 MiB");
        hash.process(line.begin(), line.end());
        if (!input.eof()) {
            const std::string lf = "\n";
            hash.process(lf.begin(), lf.end());
        }
        // Fortran D exponents occur in ICGEM data. Change only numeric text, after hashing.
        const auto raw = fields(line);
        if (raw.empty())
            continue;
        const auto key = lower(raw.front());
        if (!started) {
            if (key.starts_with("stk.v.")) {
                grv = true;
                continue;
            }
            if ((!grv && key == "end_of_head") ||
                (grv && key == "begin" && raw.size() == 2 && lower(raw[1]) == "coefficients")) {
                begin();
                continue;
            }
            if (grv && key == "begin")
                throw std::runtime_error(
                    "STK tide/process-noise blocks are not supported by the static gravity loader");
            const bool known = grv || key == "modelname" || key == "earth_gravity_constant" ||
                               key == "radius" || key == "max_degree" || key == "norm" ||
                               key == "tide_system" || key == "product_type" || key == "errors" ||
                               key == "format";
            if (known) {
                require(raw.size() == 2 && !header.contains(key),
                        "Invalid or duplicate gravity header");
                if (grv)
                    require(key == "model" || key == "gm" || key == "refdistance" ||
                                key == "degree" || key == "order" || key == "normalized" ||
                                key == "includespermtide",
                            "Unsupported STK gravity header field");
                header.emplace(key, raw[1]);
            }
            continue;
        }
        if (grv && key == "end" && raw.size() == 2 && lower(raw[1]) == "coefficients") {
            require(!ended, "Duplicate gravity coefficients block");
            ended = true;
            continue;
        }
        require(!ended, "Unsupported records after gravity coefficients");
        require(grv || key == "gfc", "Time-dependent or unknown gravity records are unsupported");
        const std::size_t offset = grv ? 0 : 1;
        require(raw.size() >= offset + 4 && raw.size() <= offset + 8,
                "Incomplete gravity coefficient record");
        const int n = number<int>(raw[offset]), m = number<int>(raw[offset + 1]);
        require(n >= 0 && n <= info.degree && m >= 0 && m <= std::min(n, info.order),
                "Gravity coefficient exceeds declared degree/order");
        const auto i = static_cast<std::size_t>(n * (n + 1) / 2 + m);
        require(!seen[i], "Duplicate gravity coefficient");
        seen[i] = true;
        const auto real = [](std::string_view s) {
            std::string value(s);
            std::replace(value.begin(), value.end(), 'D', 'E');
            std::replace(value.begin(), value.end(), 'd', 'e');
            return number<double>(value);
        };
        double c = real(raw[offset + 2]), s = real(raw[offset + 3]);
        for (std::size_t j = offset + 4; j < raw.size(); ++j)
            require(real(raw[j]) >= 0, "Invalid gravity uncertainty");
        require((n != 0 || (c == 1 && s == 0)) && (n != 1 || (c == 0 && s == 0)) &&
                    (m != 0 || s == 0),
                "Gravity field must be geocentric with unit monopole");
        if (!normalized && n <= core::gravity_max_degree) {
            // Logarithmic normalization avoids factorial overflow. Underflow cannot be
            // silently accepted for a nonzero unnormalized coefficient.
            const double log_scale = 0.5 * (std::log((m == 0 ? 1.0 : 2.0) * (2 * n + 1)) +
                                            std::lgamma(n - m + 1.0) - std::lgamma(n + m + 1.0));
            const auto convert = [log_scale](double v) {
                return v == 0 ? 0.0 : std::copysign(std::exp(std::log(std::abs(v)) - log_scale), v);
            };
            c = convert(c);
            s = convert(s);
            require(std::isfinite(c) && std::isfinite(s),
                    "Unnormalized gravity coefficient overflow");
        }
        if (n <= core::gravity_max_degree)
            coefficients.push_back({n, m, c, s});
        ++count;
    }
    require(input.eof() && !input.bad() && started && count > 0 && (!grv || ended),
            "Incomplete gravity file");
    if (!grv) {
        // ICGEM fields are dense through their square part. EGM2008 legitimately
        // stops at order 2159 in degrees 2160..2190; infer the declared rectangular tail.
        int last_order = -1;
        for (int m = 0; m <= info.degree; ++m)
            if (seen[static_cast<std::size_t>(info.degree * (info.degree + 1) / 2 + m)])
                last_order = m;
        require(last_order >= std::min(info.degree, core::gravity_max_degree),
                "Truncated ICGEM gravity file");
        info.order = last_order;
        for (int n = 2; n <= info.degree; ++n)
            for (int m = 0; m <= std::min(n, last_order); ++m)
                require(seen[static_cast<std::size_t>(n * (n + 1) / 2 + m)],
                        "Missing ICGEM gravity coefficient");
    }
    if (checkpoint)
        checkpoint();
    hash.finish();
    info.sha256 = picosha2::get_hash_hex_string(hash);
    return std::make_shared<const core::GravityModel>(std::move(info), coefficients);
}
application::DataReference gravity_reference(const std::string &selection,
                                             const core::GravityInfo &info) {
    // Raw bytes carry all physical metadata; the reader/model version covers interpretation.
    constexpr auto version = "static-normalized-gravity-360-v1";
    return {"gravity/" + selection, info.model, info.sha256, version,
            picosha2::hash256_hex_string(std::string(version))};
}
} // namespace orbitforge::runtime
