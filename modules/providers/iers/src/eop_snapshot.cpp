#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <locale>
#include <nlohmann/json.hpp>
#include <numbers>
#include <orbitforge/providers/iers/eop_snapshot.hpp>
#include <picosha2.h>
#include <sstream>
#include <vector>

namespace orbitforge::providers::iers {
namespace {
constexpr double radians_per_arcsecond = std::numbers::pi / (180.0 * 3600.0);
constexpr double seconds_per_day = 86400.0;
struct Node {
    core::Epoch epoch;
    std::array<double, 5> value;
    std::array<double, 5> rate;
    unsigned prediction_mask = 0;
    unsigned rate_prediction_mask = 0;
};

std::string read_file(const std::filesystem::path &path, std::size_t max_bytes) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw core::EopDataError("Cannot read EOP file: " + path.string());
    }
    const auto size = input.tellg();
    if (size < 0 || static_cast<std::uintmax_t>(size) > max_bytes) {
        throw core::EopDataError("EOP input exceeds the supported file size");
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        throw core::EopDataError("Incomplete EOP file read");
    }
    return bytes;
}

bool is_sha256(const std::string &value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

int manifest_mjd(const nlohmann::json &manifest, const char *key, int minimum) {
    const auto &value = manifest.at(key);
    // This interpolation uses observed x/y rates and CPO; pre-1984 C04 contains placeholders.
    if (!value.is_number_integer() || value < minimum || value > 100000) {
        throw core::EopDataError("EOP coverage MJD must be a supported integer");
    }
    return value.get<int>();
}

std::vector<Node> parse_c04(const std::string &bytes, int first, int last,
                            const core::ITimeProvider &time) {
    if (bytes.find("20 C04") == std::string::npos ||
        bytes.find("sampled at 0h UTC") == std::string::npos ||
        bytes.find("dX(\")") == std::string::npos || bytes.find("dY(\")") == std::string::npos) {
        throw core::EopDataError("Expected IERS 20 C04 0h UTC dX/dY header");
    }
    std::vector<Node> nodes;
    std::istringstream input(bytes);
    std::string line;
    int previous_mjd = -1;
    const auto mjd_zero = std::chrono::sys_days(std::chrono::year(1858) / 11 / 17);
    while (std::getline(input, line)) {
        const auto start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos || line[start] == '#') {
            continue;
        }
        std::istringstream row(line);
        row.imbue(std::locale::classic());
        int year = 0, month = 0, day = 0, hour = 0;
        std::array<double, 17> fields{};
        if (!(row >> year >> month >> day >> hour)) {
            throw core::EopDataError("Malformed C04 date fields");
        }
        for (auto &field : fields) {
            if (!(row >> field) || !std::isfinite(field)) {
                throw core::EopDataError("Missing or nonfinite C04 field");
            }
        }
        row >> std::ws;
        if (!row.eof() || year < 1600 || year > 2400 || month < 1 || month > 12 || day < 1 ||
            day > 31 || hour != 0) {
            throw core::EopDataError("C04 requires exactly 21 fields and a valid 0h UTC date");
        }
        const auto date =
            std::chrono::year(year) / static_cast<unsigned>(month) / static_cast<unsigned>(day);
        if (!date.ok()) {
            throw core::EopDataError("Invalid C04 calendar date");
        }
        const auto calendar_mjd = (std::chrono::sys_days(date) - mjd_zero).count();
        if (fields[0] != static_cast<double>(calendar_mjd) ||
            (previous_mjd >= 0 && calendar_mjd != previous_mjd + 1)) {
            throw core::EopDataError("C04 date/MJD mismatch, duplicate, unordered or missing day");
        }
        previous_mjd = static_cast<int>(calendar_mjd);
        if (std::abs(fields[1]) >= 10 || std::abs(fields[2]) >= 10 || std::abs(fields[3]) >= 2 ||
            std::abs(fields[4]) >= 10 || std::abs(fields[5]) >= 10 || std::abs(fields[6]) >= 10 ||
            std::abs(fields[7]) >= 10 || std::abs(fields[8]) >= 1 ||
            std::any_of(fields.begin() + 9, fields.end(), [](double v) { return v < 0; })) {
            throw core::EopDataError(
                "C04 parameter or uncertainty is outside the supported domain");
        }
        if (calendar_mjd < first || calendar_mjd > last) {
            continue; // The manifest explicitly selects this window of the hash-checked file.
        }
        const auto epoch = time.from_utc({year, month, day, 0, 0, 0, 0});
        const double ut1_tai = fields[3] - time.tai_minus_utc_seconds(epoch);
        nodes.push_back(
            {epoch,
             {ut1_tai, fields[1] * radians_per_arcsecond, fields[2] * radians_per_arcsecond,
              fields[4] * radians_per_arcsecond, fields[5] * radians_per_arcsecond},
             {-fields[8] / seconds_per_day, fields[6] * radians_per_arcsecond / seconds_per_day,
              fields[7] * radians_per_arcsecond / seconds_per_day, 0, 0}});
    }
    if (nodes.size() != static_cast<std::size_t>(last - first + 1)) {
        throw core::EopDataError("C04 file does not cover every day declared in the manifest");
    }
    // C04 has no dX/dY rates. Adjacent secants give shared node rates and a C1 interpolant.
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto left = i == 0 ? i : i - 1;
        const auto right = i + 1 == nodes.size() ? i : i + 1;
        const double dt = nodes[right].epoch.seconds_since(nodes[left].epoch);
        for (std::size_t k = 3; k < 5; ++k) {
            nodes[i].rate[k] = (nodes[right].value[k] - nodes[left].value[k]) / dt;
        }
    }
    return nodes;
}

std::vector<Node> parse_finals(const std::string &bytes, int first, int last,
                               const core::ITimeProvider &time) {
    std::vector<Node> nodes;
    std::istringstream input(bytes);
    std::string line;
    const auto mjd_zero = std::chrono::sys_days(std::chrono::year(1858) / 11 / 17);
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        if (line.size() < 15)
            throw core::EopDataError("Truncated finals2000A date");
        const auto field = [&](std::size_t offset, std::size_t count) {
            std::istringstream stream(line.substr(offset, count));
            stream.imbue(std::locale::classic());
            double value;
            if (!(stream >> value) || !std::isfinite(value))
                throw core::EopDataError("Missing or nonfinite finals2000A field");
            stream >> std::ws;
            if (!stream.eof())
                throw core::EopDataError("Invalid finals2000A numeric field");
            return value;
        };
        const double mjd = field(7, 8);
        if (mjd < first || mjd > last)
            continue;
        if (line.size() < 134 || mjd != first + static_cast<int>(nodes.size()))
            throw core::EopDataError("finals2000A requires complete, consecutive daily samples");
        const auto date_field = [&](std::size_t offset) {
            const auto value = field(offset, 2);
            if (value != std::floor(value) || value < 0 || value > 99)
                throw core::EopDataError("Invalid finals2000A calendar field");
            return static_cast<int>(value);
        };
        const int year = date_field(0) + (mjd <= 51543 ? 1900 : 2000), month = date_field(2),
                  day = date_field(4);
        const auto date =
            std::chrono::year(year) / static_cast<unsigned>(month) / static_cast<unsigned>(day);
        if (!date.ok() || (std::chrono::sys_days(date) - mjd_zero).count() != mjd)
            throw core::EopDataError("finals2000A date/MJD mismatch");
        unsigned prediction = 0;
        for (const auto &[offset, mask] :
             {std::pair{16u, 1u}, std::pair{57u, 2u}, std::pair{95u, 4u}}) {
            if (line[offset] != 'I' && line[offset] != 'P')
                throw core::EopDataError("Missing finals2000A observation/prediction flag");
            if (line[offset] == 'P')
                prediction |= mask;
        }
        const double x = field(18, 9), y = field(37, 9), dut1 = field(58, 10),
                     dx = field(97, 9) / 1000, dy = field(116, 9) / 1000;
        if (std::abs(x) >= 10 || std::abs(y) >= 10 || std::abs(dut1) >= 2 || std::abs(dx) >= 10 ||
            std::abs(dy) >= 10)
            throw core::EopDataError("finals2000A parameter outside supported domain");
        for (const auto &[offset, count] :
             {std::pair{27u, 9u}, std::pair{46u, 9u}, std::pair{68u, 10u}, std::pair{106u, 9u},
              std::pair{125u, 9u}})
            if (field(offset, count) < 0)
                throw core::EopDataError("Negative finals2000A uncertainty");
        const auto epoch = time.from_utc({year, month, day, 0, 0, 0, 0});
        nodes.push_back(
            {epoch,
             {dut1 - time.tai_minus_utc_seconds(epoch), x * radians_per_arcsecond,
              y * radians_per_arcsecond, dx * radians_per_arcsecond, dy * radians_per_arcsecond},
             {},
             prediction});
    }
    if (nodes.size() != static_cast<std::size_t>(last - first + 1))
        throw core::EopDataError("finals2000A does not cover the declared interval");
    // Bulletin A does not always supply LOD and does not supply polar-motion rates.
    // Use shared adjacent secants for all five fields; keep this interpolation
    // explicitly distinct from the C04 model that uses measured rates.
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto left = i == 0 ? i : i - 1, right = i + 1 == nodes.size() ? i : i + 1;
        const auto dt = nodes[right].epoch.seconds_since(nodes[left].epoch);
        nodes[i].rate_prediction_mask = nodes[left].prediction_mask | nodes[right].prediction_mask;
        for (std::size_t k = 0; k < 5; ++k)
            nodes[i].rate[k] = (nodes[right].value[k] - nodes[left].value[k]) / dt;
    }
    return nodes;
}

class EopSnapshot final : public core::IEarthOrientationProvider {
  public:
    EopSnapshot(std::vector<Node> nodes, core::EopDataInfo info)
        : nodes_(std::move(nodes)), info_(std::move(info)),
          source_id_(info_.id + ";sha256=" + info_.sha256 +
                     ";time-sha256=" + info_.time_data.sha256 +
                     ";first-tai-ns=" + std::to_string(info_.first_epoch.tai_nanoseconds()) +
                     ";last-tai-ns=" + std::to_string(info_.last_epoch.tai_nanoseconds()) +
                     ";interpolation=" + info_.interpolation + ";subdaily-tides=none") {}

    core::EarthOrientationState at(core::Epoch epoch) const override {
        if (epoch.tai_nanoseconds() < nodes_.front().epoch.tai_nanoseconds() ||
            epoch.tai_nanoseconds() > nodes_.back().epoch.tai_nanoseconds()) {
            throw core::EopCoverageError(
                "Epoch is outside this EOP snapshot; extrapolation is disabled");
        }
        const auto upper =
            std::lower_bound(nodes_.begin(), nodes_.end(), epoch, [](const Node &n, core::Epoch t) {
                return n.epoch.tai_nanoseconds() < t.tai_nanoseconds();
            });
        std::array<double, 5> value{}, rate{};
        unsigned prediction_mask = upper->prediction_mask | upper->rate_prediction_mask;
        if (upper->epoch == epoch) {
            value = upper->value;
            rate = upper->rate;
        } else {
            const auto &b = *upper;
            const auto &a = *(upper - 1);
            prediction_mask |= a.prediction_mask | a.rate_prediction_mask;
            const double h = b.epoch.seconds_since(a.epoch);
            const double t = epoch.seconds_since(a.epoch) / h;
            // Stable Hermite form around the left value. Avoid cancellation of UT1-TAI's
            // ~37-second offset while differentiating millisecond daily variations.
            for (std::size_t k = 0; k < 5; ++k) {
                const double delta = b.value[k] - a.value[k];
                const double c2 = 3 * delta - h * (2 * a.rate[k] + b.rate[k]);
                const double c3 = -2 * delta + h * (a.rate[k] + b.rate[k]);
                value[k] = a.value[k] + t * (h * a.rate[k] + t * (c2 + t * c3));
                rate[k] = a.rate[k] + t * (2 * c2 + 3 * t * c3) / h;
            }
        }
        const auto source = info_.interpolation == "finals2000a-hermite-tai-v1"
                                ? source_id_ + ";prediction-mask=" + std::to_string(prediction_mask)
                                : source_id_;
        return {{epoch, value[0], value[1], value[2], value[3], value[4], source},
                {rate[0], rate[1], rate[2], rate[3], rate[4]}};
    }
    const core::EopDataInfo &data_info() const noexcept override { return info_; }

  private:
    std::vector<Node> nodes_;
    core::EopDataInfo info_;
    std::string source_id_;
};
} // namespace

std::unique_ptr<core::IEarthOrientationProvider>
load_eop_snapshot(const std::filesystem::path &manifest_path, const core::ITimeProvider &time) {
    try {
        const auto manifest = nlohmann::json::parse(read_file(manifest_path, 64 * 1024));
        const bool finals = manifest.value("format", std::string{}) == "iers-finals2000a";
        const std::string frame = finals ? "ITRS" : "ITRF2020";
        const std::string interpolation =
            finals ? "finals2000a-hermite-tai-v1" : "c04-hermite-tai-v1";
        if (!manifest.is_object() || !manifest.at("schema_version").is_number_integer() ||
            manifest.at("schema_version") != 1 ||
            (!finals && manifest.at("format") != "iers-20-c04-0h-dxdy") ||
            manifest.at("reference_frame") != frame ||
            manifest.at("interpolation") != interpolation) {
            throw core::EopDataError(
                "Unsupported EOP manifest version, format, frame or interpolation");
        }
        constexpr std::array keys{"schema_version", "snapshot_id",     "format",
                                  "data_file",      "sha256",          "source_url",
                                  "source_sha256",  "retrieved_on",    "first_mjd",
                                  "last_mjd",       "reference_frame", "interpolation"};
        for (const auto &[key, value] : manifest.items()) {
            (void)value;
            if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
                throw core::EopDataError("Unknown EOP manifest field: " + key);
            }
        }
        const auto id = manifest.at("snapshot_id").get<std::string>();
        const auto hash = manifest.at("sha256").get<std::string>();
        const auto source = manifest.at("source_url").get<std::string>();
        if (id.empty() || source.empty() || !is_sha256(hash) ||
            !is_sha256(manifest.at("source_sha256").get<std::string>()) ||
            manifest.at("retrieved_on").get<std::string>().empty()) {
            throw core::EopDataError("Incomplete EOP provenance or invalid SHA256");
        }
        const auto filename = std::filesystem::path(manifest.at("data_file").get<std::string>());
        if (filename.empty() || filename.has_parent_path() || filename.has_root_path() ||
            filename == "." || filename == "..") {
            throw core::EopDataError("EOP data_file must be a filename beside its manifest");
        }
        const auto directory =
            std::filesystem::canonical(std::filesystem::absolute(manifest_path).parent_path());
        const auto data_path = std::filesystem::canonical(directory / filename);
        if (data_path.parent_path() != directory) {
            throw core::EopDataError("EOP data_file resolves outside the snapshot directory");
        }
        const auto bytes = read_file(data_path, 16 * 1024 * 1024);
        if (picosha2::hash256_hex_string(bytes) != hash) {
            throw core::EopDataError("EOP data SHA256 mismatch");
        }
        const int first = manifest_mjd(manifest, "first_mjd", finals ? 41317 : 45700);
        const int last = manifest_mjd(manifest, "last_mjd", finals ? 41317 : 45700);
        if (last <= first) {
            throw core::EopDataError("EOP snapshot requires at least two daily samples");
        }
        auto nodes =
            finals ? parse_finals(bytes, first, last, time) : parse_c04(bytes, first, last, time);
        core::EopDataInfo info{id,
                               hash,
                               source,
                               frame,
                               interpolation,
                               nodes.front().epoch,
                               nodes.back().epoch,
                               time.data_info()};
        return std::make_unique<EopSnapshot>(std::move(nodes), std::move(info));
    } catch (const core::EopDataError &) {
        throw;
    } catch (const std::exception &error) {
        throw core::EopDataError(std::string("Cannot load EOP snapshot: ") + error.what());
    }
}
std::filesystem::path import_eop_snapshot(const std::filesystem::path &manifest_path,
                                          const std::filesystem::path &store,
                                          const core::ITimeProvider &time) {
    // Validate the source before creating anything. Validate the copied bytes again
    // so changing the source during import cannot publish unchecked data.
    (void)load_eop_snapshot(manifest_path, time);
    const auto manifest_bytes = read_file(manifest_path, 64 * 1024);
    const auto manifest = nlohmann::json::parse(manifest_bytes);
    const auto filename = std::filesystem::path(manifest.at("data_file").get<std::string>());
    if (filename.empty() || filename.has_parent_path() || filename.has_root_path() ||
        filename == "." || filename == ".." || filename == "manifest.json")
        throw core::EopDataError("Invalid snapshot data filename for import");
    const auto source = std::filesystem::canonical(manifest_path).parent_path();
    const auto data_path = std::filesystem::canonical(source / filename);
    if (data_path.parent_path() != source)
        throw core::EopDataError("Snapshot data resolves outside its directory");
    const auto data_bytes = read_file(data_path, 16 * 1024 * 1024);
    std::filesystem::create_directories(store);
    const auto root = std::filesystem::canonical(store);
    const auto destination = root / picosha2::hash256_hex_string(manifest_bytes);
    const auto published = destination / "manifest.json";
    const auto verify_existing = [&] {
        if (std::filesystem::canonical(destination).parent_path() != root ||
            read_file(published, 64 * 1024) != manifest_bytes)
            throw core::EopDataError("Snapshot store entry does not match its content address");
        (void)load_eop_snapshot(published, time);
    };
    if (std::filesystem::exists(destination)) {
        verify_existing();
        return published;
    }
    std::filesystem::path temporary;
    for (unsigned i = 0; i < 1000; ++i) {
        temporary = root / (".import-" + destination.filename().string() + "-" + std::to_string(i));
        if (std::filesystem::create_directory(temporary))
            break;
        temporary.clear();
    }
    if (temporary.empty())
        throw core::EopDataError("Cannot allocate a snapshot import directory");
    const auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove(temporary / filename, ignored);
        std::filesystem::remove(temporary / "manifest.json", ignored);
        std::filesystem::remove(temporary, ignored);
    };
    try {
        const auto write = [](const std::filesystem::path &path, const std::string &bytes) {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.exceptions(std::ios::failbit | std::ios::badbit);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.close();
        };
        write(temporary / "manifest.json", manifest_bytes);
        write(temporary / filename, data_bytes);
        (void)load_eop_snapshot(temporary / "manifest.json", time);
        std::error_code error;
        std::filesystem::rename(temporary, destination, error);
        if (error) {
            // A concurrent identical import may already have published the same entry.
            if (!std::filesystem::exists(destination))
                throw std::filesystem::filesystem_error("Cannot publish EOP snapshot", error);
            verify_existing();
            cleanup();
        }
        return published;
    } catch (...) {
        cleanup();
        throw;
    }
}
} // namespace orbitforge::providers::iers
