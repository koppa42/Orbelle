#include <algorithm>
#include <cmath>
#include <cspice/SpiceUsr.h>
#include <fstream>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <orbitforge/providers/spice/provider.hpp>
#include <picosha2.h>
#include <random>
#include <set>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace orbitforge::providers::spice {
namespace {
using Json = nlohmann::json;
struct Engine {
    std::mutex mutex;
    const void *active = nullptr;
    bool initialized = false;
};
Engine &engine() {
    static Engine value;
    return value;
}
// Every caller holds Engine::mutex, including all failure and cleanup paths.
void check_spice() {
    if (!failed_c()) {
        return;
    }
    char short_message[256]{}, long_message[2048]{};
    getmsg_c("SHORT", sizeof(short_message), short_message);
    getmsg_c("LONG", sizeof(long_message), long_message);
    reset_c();
    const std::string message = std::string(short_message) + ": " + long_message;
    if (std::string(short_message) == "SPICE(SPKINSUFFDATA)") {
        throw core::EphemerisCoverageError(message);
    }
    throw core::EphemerisQueryError(message);
}
void initialize_engine() {
    if (!engine().initialized) {
        char action[] = "RETURN", report[] = "NONE";
        erract_c("SET", 0, action);
        errprt_c("SET", 0, report);
        check_spice();
        if (std::string(tkvrsn_c("TOOLKIT")) != "CSPICE_N0067") {
            throw core::EphemerisDataError("This provider requires CSPICE N0067");
        }
        engine().initialized = true;
    }
}

void validate_lsk_pool() {
    // A KPL/LSK header does not stop furnsh from interpreting meta-kernel variables.
    // LSKs are loaded with ldpool; reject indirection before any further kernel loading.
    for (const auto *name : {"KERNELS_TO_LOAD", "PATH_SYMBOLS", "PATH_VALUES"}) {
        SpiceBoolean found = SPICEFALSE;
        SpiceInt count = 0;
        SpiceChar type = 0;
        dtpool_c(name, &found, &count, &type);
        check_spice();
        if (found) {
            throw core::EphemerisDataError(
                "LSK must not contain meta-kernel indirection variables");
        }
    }
    const std::pair<const char *, SpiceInt> required[] = {
        {"DELTET/DELTA_T_A", 1}, {"DELTET/K", 1}, {"DELTET/EB", 1}, {"DELTET/M", 2}};
    for (const auto &[name, expected] : required) {
        SpiceBoolean found = SPICEFALSE;
        SpiceInt count = 0;
        SpiceChar type = 0;
        dtpool_c(name, &found, &count, &type);
        check_spice();
        if (!found || type != 'N' || count != expected) {
            throw core::EphemerisDataError("LSK time constant has the wrong type or cardinality: " +
                                           std::string(name));
        }
        double values[2]{};
        gdpool_c(name, 0, expected, &count, values, &found);
        check_spice();
        if (!found || count != expected || !std::isfinite(values[0]) ||
            (expected == 2 && !std::isfinite(values[1])) ||
            (std::string(name) == "DELTET/DELTA_T_A" && values[0] != 32.184)) {
            throw core::EphemerisDataError(
                "LSK time constants are invalid or TT-TAI is not 32.184 s");
        }
    }
}

std::string read_file(const std::filesystem::path &path, std::size_t limit) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw core::EphemerisDataError("Cannot read SPICE input: " + path.string());
    }
    const auto size = input.tellg();
    if (size <= 0 || static_cast<std::uintmax_t>(size) > limit) {
        throw core::EphemerisDataError("SPICE input is empty or exceeds its size limit");
    }
    std::string data(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(data.data(), static_cast<std::streamsize>(data.size()))) {
        throw core::EphemerisDataError("Incomplete SPICE input read");
    }
    return data;
}
bool hash_string(const std::string &value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
void keys(const Json &value, std::initializer_list<const char *> expected) {
    if (!value.is_object() || value.size() != expected.size()) {
        throw core::EphemerisDataError("SPICE manifest has missing or extra fields");
    }
    for (const auto *key : expected) {
        if (!value.contains(key)) {
            throw core::EphemerisDataError("Missing SPICE manifest field: " + std::string(key));
        }
    }
}
std::string nonempty(const Json &value, const char *key) {
    auto result = value.at(key).get<std::string>();
    if (result.empty()) {
        throw core::EphemerisDataError("Empty SPICE manifest field: " + std::string(key));
    }
    return result;
}
core::Epoch epoch_field(const Json &value, const char *key) {
    const auto &number = value.at(key);
    if (!number.is_number_integer() ||
        (number.is_number_unsigned() &&
         number.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX))) {
        throw core::EphemerisDataError("SPICE coverage requires int64 TAI nanoseconds");
    }
    return core::Epoch(number.get<std::int64_t>());
}

// Own only files created in one new directory. Cleanup never recursively removes a caller path.
struct KernelCopies {
    std::filesystem::path directory;
    std::vector<std::filesystem::path> paths;
#ifdef _WIN32
    std::vector<HANDLE> leases;
#endif
    explicit KernelCopies(const std::filesystem::path &workspace) {
        std::filesystem::create_directories(workspace);
        const auto parent = std::filesystem::canonical(workspace);
        std::random_device random;
        for (int attempt = 0; attempt < 16; ++attempt) {
            auto candidate = parent / ("orbitforge-spice-" + std::to_string(random()) + "-" +
                                       std::to_string(random()));
            if (std::filesystem::create_directory(candidate)) {
                directory = std::move(candidate);
                return;
            }
        }
        throw core::EphemerisDataError("Cannot allocate a private kernel directory");
    }
    KernelCopies(const KernelCopies &) = delete;
    KernelCopies &operator=(const KernelCopies &) = delete;
    ~KernelCopies() {
#ifdef _WIN32
        for (const auto handle : leases) {
            CloseHandle(handle);
        }
#endif
        std::error_code error;
        for (const auto &path : paths) {
            std::filesystem::remove(path, error);
        }
        std::filesystem::remove(directory,
                                error); // Only succeeds when the owned directory is empty.
    }
    void add(const std::string &bytes, const std::string &kind) {
        const auto path = directory / ("kernel-" + std::to_string(paths.size()) +
                                       (kind == "LSK" ? ".tls" : ".bsp"));
        paths.push_back(path);
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) {
            throw core::EphemerisDataError("Cannot write the private kernel copy");
        }
#ifdef _WIN32
        // SPICE reads SPKs lazily. Keep the runtime copy locked against writes/deletion.
        const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            throw core::EphemerisDataError("Cannot hold the private kernel read lease");
        }
        leases.push_back(handle);
#endif
    }
};

class Snapshot final : public core::IEphemerisProvider {
  public:
    Snapshot(core::EphemerisDataInfo info, std::unique_ptr<KernelCopies> copies)
        : info_(std::move(info)), copies_(std::move(copies)),
          source_("manifest-sha256=" + info_.manifest_sha256 +
                  ";toolkit=CSPICE_N0067;frame=J2000;abcorr=NONE;velocity-scale=TDB") {}
    ~Snapshot() override {
        std::scoped_lock guard(engine().mutex);
        if (engine().active == this) {
            reset_c();
            kclear_c();
            reset_c();
            engine().active = nullptr;
        }
    }
    void validate() const {
        std::scoped_lock guard(engine().mutex);
        activate();
        // Verify required time constants at load time, without calling SPICE UTC conversion.
        (void)tdb_unlocked(info_.first_epoch);
        (void)tdb_unlocked(info_.last_epoch);
    }
    core::TdbTime to_tdb(core::Epoch epoch) const override {
        check_epoch(epoch);
        std::scoped_lock guard(engine().mutex);
        activate();
        return tdb_unlocked(epoch);
    }
    core::Epoch from_tdb(core::TdbTime time) const override {
        if (!std::isfinite(time.seconds_past_j2000)) {
            throw std::invalid_argument("TDB seconds must be finite");
        }
        std::scoped_lock guard(engine().mutex);
        activate();
        const auto first = tdb_unlocked(info_.first_epoch).seconds_past_j2000;
        const auto last = tdb_unlocked(info_.last_epoch).seconds_past_j2000;
        if (time.seconds_past_j2000 < first || time.seconds_past_j2000 > last) {
            throw core::EphemerisCoverageError("TDB is outside this snapshot's policy window");
        }
        if (time.seconds_past_j2000 == first) {
            return info_.first_epoch;
        }
        if (time.seconds_past_j2000 == last) {
            return info_.last_epoch;
        }
        const double tai = unitim_c(time.seconds_past_j2000, "TDB", "TAI");
        check_spice();
        double whole = 0;
        const auto fraction = std::modf(tai, &whole);
        constexpr std::int64_t billion = 1000000000;
        if (!std::isfinite(tai) || whole < static_cast<double>(INT64_MIN / billion) ||
            whole > static_cast<double>(INT64_MAX / billion)) {
            throw core::EphemerisCoverageError("Converted TAI does not fit Epoch");
        }
        const auto base = static_cast<std::int64_t>(whole) * billion;
        const auto remainder = static_cast<std::int64_t>(std::llround(fraction * 1e9));
        if ((base >= 0 && remainder > INT64_MAX - base) ||
            (base < 0 && remainder < INT64_MIN - base)) {
            throw core::EphemerisCoverageError("Converted TAI does not fit Epoch");
        }
        // The floating input passed the range check; clamp only its final integer rounding.
        return core::Epoch(std::clamp(base + remainder, info_.first_epoch.tai_nanoseconds(),
                                      info_.last_epoch.tai_nanoseconds()));
    }
    core::EphemerisState geometric_state(std::int32_t target, std::int32_t observer,
                                         core::Epoch epoch) const override {
        check_epoch(epoch);
        std::scoped_lock guard(engine().mutex);
        activate();
        return state_unlocked(target, observer, epoch);
    }
    std::vector<core::EphemerisState>
    geometric_states(std::int32_t target, std::int32_t observer,
                     std::span<const core::Epoch> epochs) const override {
        for (const auto epoch : epochs) {
            check_epoch(epoch);
        }
        std::vector<core::EphemerisState> result;
        result.reserve(epochs.size());
        if (epochs.empty()) {
            return result;
        }
        std::scoped_lock guard(engine().mutex);
        activate();
        for (const auto epoch : epochs) {
            result.push_back(state_unlocked(target, observer, epoch));
        }
        return result;
    }
    const core::EphemerisDataInfo &data_info() const noexcept override { return info_; }

  private:
    void check_epoch(core::Epoch epoch) const {
        if (epoch.tai_nanoseconds() < info_.first_epoch.tai_nanoseconds() ||
            epoch.tai_nanoseconds() > info_.last_epoch.tai_nanoseconds()) {
            throw core::EphemerisCoverageError(
                "Epoch is outside this snapshot's policy window; extrapolation is disabled");
        }
    }
    void activate() const {
        initialize_engine();
        if (engine().active == this) {
            return;
        }
        engine().active = nullptr;
        kclear_c();
        check_spice();
        try {
            for (std::size_t i = 0; i < copies_->paths.size(); ++i) {
                const auto path = copies_->paths[i].string();
                if (path.size() > 255) {
                    throw core::EphemerisDataError(
                        "CSPICE kernel paths must fit 255 bytes; choose a shorter workspace path");
                }
                char architecture[8]{}, kind[8]{};
                getfat_c(path.c_str(), sizeof(architecture), sizeof(kind), architecture, kind);
                check_spice();
                if (std::string(kind) != info_.kernels[i].kind ||
                    std::string(architecture) != (info_.kernels[i].kind == "LSK" ? "KPL" : "DAF")) {
                    throw core::EphemerisDataError(
                        "Kernel content does not match the declared LSK/SPK type");
                }
                if (info_.kernels[i].kind == "LSK") {
                    ldpool_c(path.c_str());
                    check_spice();
                    validate_lsk_pool();
                } else {
                    furnsh_c(path.c_str());
                }
                check_spice();
            }
            engine().active = this;
        } catch (...) {
            reset_c();
            kclear_c();
            reset_c();
            throw;
        }
    }
    core::TdbTime tdb_unlocked(core::Epoch epoch) const {
        const double et = unitim_c(epoch.seconds_since(core::Epoch(0)), "TAI", "TDB");
        check_spice();
        if (!std::isfinite(et)) {
            throw core::EphemerisQueryError("CSPICE returned a nonfinite TDB time");
        }
        return {et};
    }
    core::EphemerisState state_unlocked(std::int32_t target, std::int32_t observer,
                                        core::Epoch epoch) const {
        static_assert(sizeof(SpiceInt) == sizeof(std::int32_t));
        const auto tdb = tdb_unlocked(epoch);
        double state[6]{}, light_time = 0;
        spkez_c(target, tdb.seconds_past_j2000, "J2000", "NONE", observer, state, &light_time);
        check_spice();
        for (auto &value : state) {
            value *= 1000; // CSPICE km and km/s -> public SI units.
            if (!std::isfinite(value)) {
                throw core::EphemerisQueryError("CSPICE returned a nonfinite geometric state");
            }
        }
        return {epoch,
                target,
                observer,
                core::FrameId::spice_j2000,
                {state[0], state[1], state[2]},
                {state[3], state[4], state[5]},
                core::TimeScale::tdb,
                tdb,
                source_};
    }
    core::EphemerisDataInfo info_;
    std::unique_ptr<KernelCopies> copies_;
    std::string source_;
};

class TwoBody final : public core::IStateProvider {
  public:
    TwoBody(core::CartesianState initial, core::GravitationalParameter mu)
        : initial_(std::move(initial)), mu_(mu.metres_cubed_per_second_squared) {
        if (initial_.origin != core::OriginId::earth || initial_.frame != core::FrameId::gcrf) {
            throw std::invalid_argument("Two-body initial state must be Earth-centred GCRF");
        }
        if (!std::isfinite(mu_) || mu_ <= 0) {
            throw std::invalid_argument(
                "Two-body gravitational parameter must be finite and positive");
        }
        for (std::size_t k = 0; k < 3; ++k) {
            if (!std::isfinite(initial_.position_m[k]) ||
                !std::isfinite(initial_.velocity_mps[k])) {
                throw std::invalid_argument("Two-body initial state must be finite");
            }
            input_[k] = initial_.position_m[k];
            input_[k + 3] = initial_.velocity_mps[k];
        }
        const auto r = std::hypot(input_[0], input_[1], input_[2]);
        const auto v = std::hypot(input_[3], input_[4], input_[5]);
        if (!(r > 0) || !(v > 0) || !std::isfinite(r) || !std::isfinite(v)) {
            throw std::invalid_argument(
                "Two-body position and velocity norms must be finite and nonzero");
        }
        // Normalize before the cross product so this validation does not square
        // dimensional numbers or introduce a near-radial eccentricity cutoff.
        const std::array<double, 3> a{input_[0] / r, input_[1] / r, input_[2] / r};
        const std::array<double, 3> b{input_[3] / v, input_[4] / v, input_[5] / v};
        if (std::hypot(a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                       a[0] * b[1] - a[1] * b[0]) == 0) {
            throw std::invalid_argument("Radial two-body motion is unsupported");
        }
        // Validate the CSPICE numerical domain before a Session can be published.
        (void)propagate(initial_.epoch);
    }

    core::CartesianState state_at(core::Epoch epoch) const override {
        return epoch == initial_.epoch ? initial_ : propagate(epoch);
    }

  private:
    core::CartesianState propagate(core::Epoch epoch) const {
        std::array<double, 6> values{};
        try {
            std::scoped_lock guard(engine().mutex);
            initialize_engine();
            // prop2b accepts consistent units (including SI) and no kernels.
            // It must not clear or replace engine().active.
            prop2b_c(mu_, input_.data(), epoch.seconds_since(initial_.epoch), values.data());
            check_spice();
        } catch (const core::EphemerisQueryError &error) {
            throw core::PropagationError(error.what());
        } catch (const core::EphemerisDataError &error) {
            throw core::PropagationError(error.what());
        }
        if (!std::all_of(values.begin(), values.end(), [](double x) { return std::isfinite(x); })) {
            throw core::PropagationError("CSPICE two-body propagation returned a non-finite state");
        }
        return {epoch,
                initial_.origin,
                initial_.frame,
                {values[0], values[1], values[2]},
                {values[3], values[4], values[5]}};
    }
    const core::CartesianState initial_;
    const double mu_;
    std::array<double, 6> input_{};
};
} // namespace

std::shared_ptr<const core::IStateProvider> make_two_body(core::CartesianState initial,
                                                          core::GravitationalParameter mu) {
    return std::make_shared<TwoBody>(std::move(initial), mu);
}

std::unique_ptr<core::IEphemerisProvider> load_snapshot(const std::filesystem::path &manifest_path,
                                                        const std::filesystem::path &workspace) {
    try {
        const auto manifest_bytes = read_file(manifest_path, 64 * 1024);
        const auto manifest = Json::parse(manifest_bytes);
        keys(manifest,
             {"schema_version", "snapshot_id", "model", "first_tai_ns", "last_tai_ns", "kernels"});
        if (!manifest.at("schema_version").is_number_integer() ||
            manifest.at("schema_version") != 1) {
            throw core::EphemerisDataError("Unsupported SPICE manifest schema version");
        }
        core::EphemerisDataInfo info{nonempty(manifest, "snapshot_id"),
                                     picosha2::hash256_hex_string(manifest_bytes),
                                     nonempty(manifest, "model"),
                                     "CSPICE_N0067",
                                     epoch_field(manifest, "first_tai_ns"),
                                     epoch_field(manifest, "last_tai_ns"),
                                     {}};
        if (info.first_epoch.tai_nanoseconds() >= info.last_epoch.tai_nanoseconds()) {
            throw core::EphemerisDataError("SPICE snapshot needs a nonempty policy window");
        }
        const auto &kernels = manifest.at("kernels");
        if (!kernels.is_array() || kernels.empty() || kernels.size() > 16) {
            throw core::EphemerisDataError("SPICE snapshot requires 1 to 16 explicit kernels");
        }
        const auto parent =
            std::filesystem::canonical(std::filesystem::absolute(manifest_path).parent_path());
        auto copies = std::make_unique<KernelCopies>(workspace);
        std::set<std::filesystem::path> paths;
        std::size_t total = 0;
        int lsk_count = 0, spk_count = 0;
        for (const auto &kernel : kernels) {
            keys(kernel, {"file", "kind", "sha256", "source_url", "source_sha256", "producer"});
            core::KernelDataInfo item{
                nonempty(kernel, "file"),          nonempty(kernel, "kind"),
                nonempty(kernel, "sha256"),        nonempty(kernel, "source_url"),
                nonempty(kernel, "source_sha256"), nonempty(kernel, "producer")};
            const std::filesystem::path name(item.file);
            if (name.has_parent_path() || name.has_root_path() || name == "." || name == ".." ||
                !hash_string(item.sha256) || !hash_string(item.source_sha256) ||
                (item.kind != "LSK" && item.kind != "SPK")) {
                throw core::EphemerisDataError("Invalid SPICE filename, hash or kernel kind");
            }
            const auto path = std::filesystem::canonical(parent / name);
            if (path.parent_path() != parent || !paths.insert(path).second) {
                throw core::EphemerisDataError(
                    "SPICE kernel escapes the snapshot or is duplicated");
            }
            const auto bytes = read_file(path, 64 * 1024 * 1024 - total);
            total += bytes.size();
            if (picosha2::hash256_hex_string(bytes) != item.sha256) {
                throw core::EphemerisDataError("SPICE kernel SHA256 mismatch: " + item.file);
            }
            copies->add(bytes, item.kind);
            lsk_count += item.kind == "LSK" ? 1 : 0;
            spk_count += item.kind == "SPK" ? 1 : 0;
            info.kernels.push_back(std::move(item));
        }
        if (lsk_count != 1 || spk_count < 1) {
            throw core::EphemerisDataError(
                "SPICE snapshot requires exactly one LSK and at least one SPK");
        }
        auto provider = std::make_unique<Snapshot>(std::move(info), std::move(copies));
        provider->validate();
        return provider;
    } catch (const core::EphemerisDataError &) {
        throw;
    } catch (const std::exception &error) {
        throw core::EphemerisDataError(std::string("Cannot load SPICE snapshot: ") + error.what());
    }
}
} // namespace orbitforge::providers::spice
