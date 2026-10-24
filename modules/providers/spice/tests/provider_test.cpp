#include <algorithm>
#include <chrono>
#include <cmath>
#include <cspice/SpiceUsr.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <orbelle/core/propagation/hpop_force.hpp>
#include <orbelle/providers/spice/provider.hpp>
#include <picosha2.h>
#ifdef ORBELLE_SPICE_TEST_ERFA
#include <erfa.h>
#include <orbelle/providers/erfa/provider.hpp>
#endif

using namespace orbelle;
namespace {
using Json = nlohmann::json;
std::string read(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
void spice_ok() {
    if (failed_c()) {
        char message[2048]{};
        getmsg_c("LONG", sizeof(message), message);
        reset_c();
        throw std::runtime_error(message);
    }
}
void write_kernel(const std::filesystem::path &path, double offset) {
    // Test-only construction, in a single thread before any Provider is created.
    std::filesystem::remove(path);
    char action[] = "RETURN", report[] = "NONE";
    erract_c("SET", 0, action);
    errprt_c("SET", 0, report);
    SpiceInt handle = 0;
    spkopn_c(path.string().c_str(), "Orbelle synthetic Chebyshev cubic", 0, &handle);
    spice_ok();
    const double coefficients[]{7000 + offset, 10, 2, 0.5, -1200, 0.2, 3, 0, 500, -0.3, 0, 0.6};
    spkw02_c(handle, 100001, 399, "J2000", -3600, 3600, "Orbelle synthetic cubic", 7200, 1, 3,
             coefficients, -3600);
    spice_ok();
    spkcls_c(handle);
    spice_ok();
}
struct Fixture {
    std::filesystem::path root;
    std::filesystem::path manifest;
    std::filesystem::path workspace;
    Json document;
    Fixture() {
        root = std::filesystem::path(ORBELLE_SPICE_TEST_DIR) /
               ::testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::create_directories(root);
        manifest = root / "manifest.json";
        workspace = root / "runtime";
        std::filesystem::copy_file(
            std::filesystem::path(ORBELLE_SPICE_SAMPLE).parent_path() / "naif0012.tls",
            root / "naif0012.tls", std::filesystem::copy_options::overwrite_existing);
        write_kernel(root / "a.bsp", 0);
        write_kernel(root / "b.bsp", 100);
        document = {
            {"schema_version", 1},
            {"snapshot_id", "synthetic-cubic"},
            {"model", "artificial-Chebyshev-cubic"},
            {"first_tai_ns", -3600000000000LL},
            {"last_tai_ns", 3600000000000LL},
            {"kernels", Json::array({kernel("naif0012.tls", "LSK"), kernel("a.bsp", "SPK")})}};
        save();
    }
    Json kernel(const std::string &name, const std::string &kind) const {
        const auto hash = picosha2::hash256_hex_string(read(root / name));
        return {{"file", name},          {"kind", kind},
                {"sha256", hash},        {"source_url", "synthetic-test-fixture"},
                {"source_sha256", hash}, {"producer", "Orbelle test suite"}};
    }
    void save() const { std::ofstream(manifest) << document.dump(2); }
    auto load() const { return providers::spice::load_snapshot(manifest, workspace); }
};
void analytic(const core::EphemerisState &state, double offset = 0) {
    const double s = state.tdb.seconds_past_j2000 / 3600;
    const double x = 7000 + offset + 10 * s + 2 * (2 * s * s - 1) + 0.5 * (4 * s * s * s - 3 * s);
    const double y = -1200 + 0.2 * s + 3 * (2 * s * s - 1);
    const double z = 500 - 0.3 * s + 0.6 * (4 * s * s * s - 3 * s);
    const std::array positions{x, y, z};
    const std::array velocities{(10 + 8 * s + 0.5 * (12 * s * s - 3)) / 3600, (0.2 + 12 * s) / 3600,
                                (-0.3 + 0.6 * (12 * s * s - 3)) / 3600};
    for (std::size_t k = 0; k < 3; ++k) {
        EXPECT_NEAR(state.position_m[k], positions[k] * 1000, 1e-6);
        EXPECT_NEAR(state.velocity_mps[k], velocities[k] * 1000, 1e-9);
    }
}
} // namespace

TEST(Spice, AnalyticSpkHasExplicitUnitsAxesAndVelocityScale) {
    Fixture input;
    const auto provider = input.load();
    for (const auto ns :
         {-3000000000000LL, -32184000000LL, 0LL, 1234567890123LL, 3000000000000LL}) {
        const auto state = provider->geometric_state(100001, 399, core::Epoch(ns));
        EXPECT_EQ(state.epoch, core::Epoch(ns));
        EXPECT_EQ(state.frame, core::FrameId::spice_j2000);
        EXPECT_EQ(state.velocity_time_scale, core::TimeScale::tdb);
        EXPECT_EQ(state.target_naif_id, 100001);
        EXPECT_EQ(state.observer_naif_id, 399);
        EXPECT_NE(state.source_id.find(provider->data_info().manifest_sha256), std::string::npos);
        analytic(state);
    }
}

TEST(Spice, ThirdBodyCoverageRejectsInteriorGapsAndChecksEveryRequiredChain) {
    Fixture input;
    const auto file = input.root / "third.bsp";
    std::filesystem::remove(file);
    SpiceInt handle = 0;
    spkopn_c(file.string().c_str(), "synthetic DE chains with interior gap", 0, &handle);
    spice_ok();
    const double coefficients[]{10000, 0, 0, 0, 1000, 0, 0, 0, 2000, 0, 0, 0};
    for (int body : {1, 3, 5, 10, 199, 399, 301, 599}) {
        for (const auto &interval : {std::pair{-3600., -100.}, std::pair{100., 3600.}}) {
            spkw02_c(handle, body,
                     body < 11     ? 0
                     : body == 199 ? 1
                     : body == 599 ? 5
                                   : 3,
                     "J2000", interval.first, interval.second, "synthetic segment",
                     interval.second - interval.first, 1, 3, coefficients, interval.first);
            spice_ok();
        }
    }
    spkcls_c(handle);
    spice_ok();
    input.document["kernels"][1] = input.kernel("third.bsp", "SPK");
    input.save();
    const auto provider = input.load();
    const core::Epoch before{-1000000000000}, after{1000000000000};
    EXPECT_NO_THROW((void)provider->geometric_state(10, 399, before));
    EXPECT_NO_THROW((void)provider->geometric_state(10, 399, after));
    EXPECT_THROW(provider->validate_earth_gravity_interval(before, after, true, true),
                 core::EphemerisCoverageError);
    EXPECT_NO_THROW(provider->validate_earth_gravity_interval(after, after, true, true));
    EXPECT_NO_THROW(
        provider->validate_earth_gravity_interval(core::Epoch{200000000000}, after, false, true));
    EXPECT_THROW(provider->validate_earth_gravity_interval(after, before, true, true),
                 core::EphemerisCoverageError);
    for (auto target : {199, 5, 599}) {
        const std::array targets{std::int32_t(target)};
        EXPECT_THROW(provider->validate_earth_gravity_interval(before, after, targets),
                     core::EphemerisCoverageError);
        EXPECT_NO_THROW(provider->validate_earth_gravity_interval(after, after, targets));
    }
    const std::array missing{std::int32_t{6}};
    EXPECT_THROW(provider->validate_earth_gravity_interval(after, after, missing),
                 core::EphemerisCoverageError);
}

TEST(Spice, TaiTdbRoundTripAndEpochZeroAreCorrect) {
    Fixture input;
    const auto provider = input.load();
    for (const auto ns :
         {-3600000000000LL, -32184000000LL, -1LL, 0LL, 1LL, 1234567890123LL, 3600000000000LL}) {
        const auto time = provider->to_tdb(core::Epoch(ns));
        EXPECT_LE(std::abs(provider->from_tdb(time).seconds_since(core::Epoch(ns))), 1e-6);
    }
    EXPECT_NEAR(provider->to_tdb(core::Epoch(0)).seconds_past_j2000, 32.183927, 2e-6);
    EXPECT_THROW((void)provider->to_tdb(core::Epoch(3600000000001LL)),
                 core::EphemerisCoverageError);
    EXPECT_THROW((void)provider->from_tdb({1e20}), core::EphemerisCoverageError);
    EXPECT_THROW((void)provider->from_tdb({std::numeric_limits<double>::quiet_NaN()}),
                 std::invalid_argument);
}

TEST(Spice, MissingActualCoverageAndQueryErrorsDoNotPoisonEngine) {
    Fixture input;
    const auto provider = input.load();
    // This is inside the declared TAI window but outside the actual SPK ET range.
    EXPECT_THROW((void)provider->geometric_state(100001, 399, core::Epoch(3600000000000LL)),
                 core::EphemerisCoverageError);
    EXPECT_THROW((void)provider->geometric_state(999999, 399, core::Epoch(0)),
                 core::EphemerisCoverageError);
    analytic(provider->geometric_state(100001, 399, core::Epoch(0)));
}

TEST(Spice, LskIndirectionAndIncompleteTimeConstantsAreRejected) {
    Fixture input;
    const auto original = read(input.root / "naif0012.tls");
    const std::vector<std::string> invalid{"DELTET/M = ( 6.239996 )",
                                           "DELTET/DELTA_T_A = ( 32 )",
                                           "DELTET/K = ( 'not-numeric' )",
                                           "KERNELS_TO_LOAD = ( '" +
                                               (input.root / "b.bsp").generic_string() + "' )",
                                           "PATH_SYMBOLS = ( 'EXTRA' )",
                                           "PATH_VALUES = ( '.' )"};
    // First attempt in this process has no previous valid UNITIM cache.
    std::ofstream(input.root / "naif0012.tls") << original << "\n\\begindata\n"
                                               << invalid.front() << '\n';
    input.document["kernels"][0] = input.kernel("naif0012.tls", "LSK");
    input.save();
    EXPECT_THROW((void)input.load(), core::EphemerisDataError);
    std::ofstream(input.root / "naif0012.tls") << original;
    input.document["kernels"][0] = input.kernel("naif0012.tls", "LSK");
    input.save();
    const auto good = input.load();
    for (const auto &line : invalid) {
        std::ofstream(input.root / "naif0012.tls") << original << "\n\\begindata\n" << line << '\n';
        input.document["kernels"][0] = input.kernel("naif0012.tls", "LSK");
        input.save();
        EXPECT_THROW((void)input.load(), core::EphemerisDataError);
        analytic(good->geometric_state(100001, 399, core::Epoch(0)));
    }
}

TEST(Spice, ExtremeEpochRoundtripAccountsForDoubleTimeQuantization) {
    Fixture input;
    input.document["first_tai_ns"] = INT64_MIN;
    input.document["last_tai_ns"] = INT64_MAX;
    input.save();
    const auto provider = input.load();
    const std::array<std::int64_t, 7> samples{
        INT64_MIN,        INT64_MIN + 4096, -8589934587521630764LL, 0, 8589934587521630764LL,
        INT64_MAX - 4096, INT64_MAX};
    for (const auto ns : samples) {
        const auto tdb = provider->to_tdb(core::Epoch(ns));
        const double ulp = std::abs(
            std::nextafter(tdb.seconds_past_j2000, std::numeric_limits<double>::infinity()) -
            tdb.seconds_past_j2000);
        EXPECT_LE(std::abs(provider->from_tdb(tdb).seconds_since(core::Epoch(ns))),
                  std::max(1e-6, 4 * ulp));
    }
}

TEST(Spice, KernelOrderAndContextSwitchingAreDeterministic) {
    Fixture input;
    input.document["kernels"].push_back(input.kernel("b.bsp", "SPK"));
    input.save();
    const auto second_wins = input.load();
    std::swap(input.document["kernels"][1], input.document["kernels"][2]);
    input.save();
    const auto first_wins = input.load();
    for (int i = 0; i < 4; ++i) {
        analytic(second_wins->geometric_state(100001, 399, core::Epoch(0)), 100);
        analytic(first_wins->geometric_state(100001, 399, core::Epoch(0)));
    }
    EXPECT_NE(first_wins->data_info().manifest_sha256, second_wins->data_info().manifest_sha256);
}

TEST(Spice, RepeatedQueriesKeepExactEpochBodyObserverAndSurviveEvictionAndErrors) {
    Fixture input;
    const auto provider = input.load();
    const core::Epoch epoch{1234567890123LL};
    const auto original = provider->geometric_state(100001, 399, epoch);
    for (int pass = 0; pass < 2; ++pass) {
        const auto repeated = provider->geometric_state(100001, 399, epoch);
        EXPECT_EQ(original.position_m, repeated.position_m);
        EXPECT_EQ(original.velocity_mps, repeated.velocity_mps);
        EXPECT_EQ(original.tdb.seconds_past_j2000, repeated.tdb.seconds_past_j2000);
        EXPECT_EQ(original.source_id, repeated.source_id);
        const auto reverse = provider->geometric_state(399, 100001, epoch);
        EXPECT_EQ(reverse.target_naif_id, 399);
        EXPECT_EQ(reverse.observer_naif_id, 100001);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            EXPECT_DOUBLE_EQ(reverse.position_m[axis], -original.position_m[axis]);
            EXPECT_DOUBLE_EQ(reverse.velocity_mps[axis], -original.velocity_mps[axis]);
        }
        const core::Epoch adjacent{epoch.tai_nanoseconds() + 1};
        const auto next = provider->geometric_state(100001, 399, adjacent);
        EXPECT_EQ(next.epoch, adjacent);
        EXPECT_NE(next.position_m, original.position_m);
        analytic(next);
        EXPECT_THROW((void)provider->geometric_state(999999, 399, epoch),
                     core::EphemerisCoverageError);
        EXPECT_THROW((void)provider->geometric_state(100001, 399, core::Epoch{3600000000001LL}),
                     core::EphemerisCoverageError);
        for (int i = 0; i < 128; ++i)
            analytic(provider->geometric_state(100001, 399, core::Epoch{i * 1000000000LL}));
    }
}

TEST(Spice, ConcurrentAlternatingSnapshotsMatchSerialQueries) {
    Fixture input;
    const auto a = input.load();
    input.document["kernels"][1] = input.kernel("b.bsp", "SPK");
    input.save();
    const auto b = input.load();
    const std::vector<core::Epoch> epochs{core::Epoch(-1000000000000LL), core::Epoch(0),
                                          core::Epoch(1000000000000LL)};
    const auto expected_a = a->geometric_states(100001, 399, epochs);
    const auto expected_b = b->geometric_states(100001, 399, epochs);
    std::vector<std::future<void>> tasks;
    for (int thread = 0; thread < 8; ++thread) {
        tasks.push_back(std::async(std::launch::async, [&, thread] {
            for (int i = 0; i < 24; ++i) {
                const bool choose_a = (i + thread) % 2 == 0;
                const auto actual = (choose_a ? a : b)->geometric_states(100001, 399, epochs);
                const auto &expected = choose_a ? expected_a : expected_b;
                for (std::size_t k = 0; k < epochs.size(); ++k) {
                    EXPECT_EQ(actual[k].position_m, expected[k].position_m);
                    EXPECT_EQ(actual[k].velocity_mps, expected[k].velocity_mps);
                    EXPECT_EQ(actual[k].source_id, expected[k].source_id);
                }
            }
        }));
    }
    for (auto &task : tasks) {
        task.get();
    }
}

TEST(Spice, OwnedCopiesIgnoreSourceChangesAndAreCleanedAfterClose) {
    Fixture input;
    {
        const auto provider = input.load();
        std::ofstream(input.root / "a.bsp", std::ios::binary | std::ios::trunc) << "corrupted";
        analytic(provider->geometric_state(100001, 399, core::Epoch(0)));
        EXPECT_THROW((void)input.load(), core::EphemerisDataError);
        analytic(provider->geometric_state(100001, 399, core::Epoch(0)));
    }
    EXPECT_TRUE(std::filesystem::is_empty(input.workspace));
}

TEST(Spice, InvalidSnapshotDoesNotReplaceLiveContext) {
    Fixture input;
    const auto provider = input.load();
    const auto original = input.document;
    for (const auto &value : {Json(1.5), Json(2), Json("1")}) {
        input.document["schema_version"] = value;
        input.save();
        EXPECT_THROW((void)input.load(), core::EphemerisDataError);
    }
    input.document = original;
    input.document["kernels"][1]["file"] = "../outside.bsp";
    input.save();
    EXPECT_THROW((void)input.load(), core::EphemerisDataError);
    input.document = original;
    // Valid LSK header but no time constants: exercise CSPICE failure/reset after activation.
    std::ofstream(input.root / "naif0012.tls") << "KPL/LSK\n\\begindata\nX = 1\n";
    input.document["kernels"][0] = input.kernel("naif0012.tls", "LSK");
    input.save();
    EXPECT_THROW((void)input.load(), core::EphemerisDataError);
    analytic(provider->geometric_state(100001, 399, core::Epoch(0)));
}

#ifdef ORBELLE_SPICE_TEST_ERFA
TEST(Spice, RealSnapshotSunMoonAndIndependentTdbModel) {
    const auto workspace = std::filesystem::path(ORBELLE_SPICE_TEST_DIR) / "real-runtime";
    const auto provider = providers::spice::load_snapshot(ORBELLE_SPICE_SAMPLE, workspace);
    const auto time = providers::erfa::make_builtin_time_provider();
    for (const auto utc : {core::UtcDateTime{2016, 12, 28, 0, 0, 0, 0},
                           core::UtcDateTime{2016, 12, 31, 23, 59, 60, 123456789},
                           core::UtcDateTime{2017, 1, 4, 0, 0, 0, 0}}) {
        const auto epoch = time->from_utc(utc);
        const auto tdb = provider->to_tdb(epoch);
        EXPECT_LE(std::abs(provider->from_tdb(tdb).seconds_since(epoch)), 1e-6);
        const auto tt = time->julian_date(epoch, core::TimeScale::tt);
        const auto independent_delta = eraDtdb(tt.day, tt.fraction, 0, 0, 0, 0);
        EXPECT_NEAR(tdb.seconds_past_j2000 - (epoch.seconds_since(core::Epoch(0)) + 32.184),
                    independent_delta, 50e-6);
        const auto sun = provider->geometric_state(10, 399, epoch);
        const auto moon = provider->geometric_state(301, 399, epoch);
        EXPECT_NEAR(std::hypot(sun.position_m[0], sun.position_m[1], sun.position_m[2]), 1.47e11,
                    1e9);
        EXPECT_GT(std::hypot(moon.position_m[0], moon.position_m[1], moon.position_m[2]), 3.5e8);
        EXPECT_LT(std::hypot(moon.position_m[0], moon.position_m[1], moon.position_m[2]), 4.1e8);
    }
}
#endif

namespace {
constexpr std::array<std::pair<int, int>, 5> de_queries{
    {{10, 399}, {301, 399}, {399, 0}, {10, 0}, {10, 301}}};
struct DirectSample {
    core::Epoch epoch;
    double et;
    std::array<std::array<double, 6>, de_queries.size()> states;
    std::array<double, 3> apparent;
};
// Capture independent high-level CSPICE results before creating any Provider.
// Never manipulate the global kernel pool behind a live Provider's back.
std::vector<DirectSample> direct_samples(const std::filesystem::path &manifest) {
    const auto document = Json::parse(read(manifest));
    kclear_c();
    std::vector<std::filesystem::path> spks;
    for (const auto &kernel : document.at("kernels")) {
        const auto path = manifest.parent_path() / kernel.at("file").get<std::string>();
        furnsh_c(path.string().c_str());
        spice_ok();
        if (kernel.at("kind") != "LSK")
            spks.push_back(path);
    }
    const auto first = document.at("first_tai_ns").get<std::int64_t>() + 600000000000LL;
    const auto last = document.at("last_tai_ns").get<std::int64_t>();
    std::vector<std::int64_t> epochs{first, last};
    for (int i = 0; i < 513; ++i)
        epochs.push_back(first + (last - first) / 512 * i);
    // Include both sides of every segment and type-2 record boundary in range.
    const auto boundary = [&](double et) {
        const auto ns = static_cast<std::int64_t>(std::llround(unitim_c(et, "TDB", "TAI") * 1e9));
        for (const auto offset : {-10000LL, -1LL, 0LL, 1LL, 10000LL})
            if (ns + offset >= first && ns + offset <= last)
                epochs.push_back(ns + offset);
    };
    for (const auto &path : spks) {
        SpiceInt handle = 0;
        dafopr_c(path.string().c_str(), &handle);
        dafbfs_c(handle);
        SpiceBoolean found = SPICEFALSE;
        daffna_c(&found);
        while (found) {
            double descriptor[5]{}, bounds[2]{};
            SpiceInt fields[6]{};
            dafgs_c(descriptor);
            dafus_c(descriptor, 2, 6, bounds, fields);
            boundary(bounds[0]);
            boundary(bounds[1]);
            if (fields[3] == 2) {
                double directory[4]{};
                dafgda_c(handle, fields[5] - 3, fields[5], directory);
                for (int i = 0; i <= static_cast<int>(directory[3]); ++i)
                    boundary(directory[0] + directory[1] * i);
            }
            daffna_c(&found);
        }
        dafcls_c(handle);
        spice_ok();
    }
    std::sort(epochs.begin(), epochs.end());
    epochs.erase(std::unique(epochs.begin(), epochs.end()), epochs.end());
    std::vector<DirectSample> result;
    for (const auto ns : epochs) {
        // Use the public Epoch conversion: converting the entire ns count to a
        // double first would introduce an extra rounding at modern dates.
        DirectSample sample{core::Epoch{ns},
                            unitim_c(core::Epoch{ns}.seconds_since(core::Epoch{0}), "TAI", "TDB"),
                            {},
                            {}};
        double light_time = 0;
        for (std::size_t i = 0; i < de_queries.size(); ++i) {
            const auto [target, observer] = de_queries[i];
            spkez_c(target, sample.et, "J2000", "NONE", observer, sample.states[i].data(),
                    &light_time);
        }
        spkezp_c(10, sample.et, "J2000", "LT+S", 399, sample.apparent.data(), &light_time);
        spice_ok();
        result.push_back(sample);
    }
    kclear_c();
    spice_ok();
    return result;
}
void compare_direct(const core::IEphemerisProvider &provider, const DirectSample &sample) {
    SCOPED_TRACE(sample.epoch.tai_nanoseconds());
    EXPECT_EQ(provider.to_tdb(sample.epoch).seconds_past_j2000, sample.et);
    for (std::size_t i = 0; i < de_queries.size(); ++i) {
        const auto [target, observer] = de_queries[i];
        SCOPED_TRACE(target);
        SCOPED_TRACE(observer);
        const auto state = provider.geometric_state(target, observer, sample.epoch);
        EXPECT_EQ(state.epoch, sample.epoch);
        for (unsigned axis = 0; axis < 3; ++axis) {
            EXPECT_EQ(state.position_m[axis], sample.states[i][axis] * 1000);
            EXPECT_EQ(state.velocity_mps[axis], sample.states[i][axis + 3] * 1000);
        }
    }
    const auto apparent = provider.apparent_sun_from_earth(sample.epoch);
    for (unsigned axis = 0; axis < 3; ++axis)
        EXPECT_EQ(apparent[axis], sample.apparent[axis] * 1000);
}
void de_kernel(const std::filesystem::path &path, double shift, bool overlays) {
    std::filesystem::remove(path);
    SpiceInt handle = 0;
    spkopn_c(path.string().c_str(), "Synthetic DE chains and overlapping segments", 0, &handle);
    const auto segment = [&](int body, int centre, const char *frame, int type, double first,
                             double interval, int records, double origin) {
        constexpr int degree = 5;
        const int axes = type == 2 ? 3 : 6;
        std::vector<double> coefficients(static_cast<std::size_t>(records * axes * (degree + 1)));
        for (int record = 0; record < records; ++record)
            for (int axis = 0; axis < axes; ++axis)
                for (int c = 0; c <= degree; ++c)
                    coefficients[(record * axes + axis) * (degree + 1) + c] =
                        c == 0 ? (axis < 3 ? origin + shift + 13 * record + 17 * axis : .3)
                               : (axis + 1.) / (c * c * c);
        if (type == 2)
            spkw02_c(handle, body, centre, frame, first, first + interval * records,
                     "Original type 2 coefficients", interval, records, degree, coefficients.data(),
                     first);
        else
            spkw03_c(handle, body, centre, frame, first, first + interval * records,
                     "Higher priority type 3", interval, records, degree, coefficients.data(),
                     first);
        spice_ok();
    };
    if (!overlays) {
        segment(10, 0, "J2000", 2, -6000, 250, 48, 1e6);
        segment(3, 0, "J2000", 2, -6000, 250, 48, 8e7);
        segment(399, 3, "J2000", 2, -6000, 250, 48, 1000);
        segment(301, 3, "J2000", 2, -6000, 250, 48, 3e5);
    } else {
        segment(10, 0, "J2000", 2, -900, 2100, 1, 2e6);
        segment(10, 0, "J2000", 2, -400, 700, 1, 3e6);
        segment(10, 0, "J2000", 3, -100, 200, 1, 4e6);
        segment(301, 0, "J2000", 2, -500, 1000, 1, 7e7);
        segment(3, 0, "ECLIPJ2000", 2, -700, 1400, 1, 9e7);
    }
    spkcls_c(handle);
    spice_ok();
}
} // namespace

TEST(Spice, DeType2MatchesToolkitAtDenseEpochsAndRecordBoundaries) {
    const auto expected = direct_samples(ORBELLE_SPICE_SAMPLE);
    const auto provider = providers::spice::load_snapshot(
        ORBELLE_SPICE_SAMPLE, std::filesystem::path(ORBELLE_SPICE_TEST_DIR) / "type2-real");
    for (const auto &sample : expected)
        compare_direct(*provider, sample);
    for (auto sample = expected.rbegin(); sample != expected.rend(); ++sample)
        compare_direct(*provider, *sample);
}

TEST(Spice, DeType2HonoursOverlappingFileSegmentFrameAndTypePriority) {
    Fixture fixture;
    de_kernel(fixture.root / "a.bsp", 0, false);
    de_kernel(fixture.root / "b.bsp", 1234, true);
    fixture.document["kernels"] =
        Json::array({fixture.kernel("naif0012.tls", "LSK"), fixture.kernel("a.bsp", "SPK"),
                     fixture.kernel("b.bsp", "SPK")});
    fixture.save();
    const auto overlay_expected = direct_samples(fixture.manifest);
    std::swap(fixture.document["kernels"][1], fixture.document["kernels"][2]);
    fixture.save();
    const auto base_expected = direct_samples(fixture.manifest);
    const auto base = fixture.load();
    std::swap(fixture.document["kernels"][1], fixture.document["kernels"][2]);
    fixture.save();
    const auto overlay = fixture.load();
    for (const auto &sample : base_expected)
        compare_direct(*base, sample);
    for (const auto &sample : overlay_expected)
        compare_direct(*overlay, sample);
    // Alternate snapshots/DAF handles and jump across more records than fit in
    // either cache. Concurrent queries must match the independently loaded pool.
    std::vector<std::future<void>> workers;
    for (unsigned worker = 0; worker < 4; ++worker)
        workers.push_back(std::async(std::launch::async, [&, worker] {
            for (unsigned i = 0; i < 24; ++i) {
                const auto index = (i * 137 + worker * 23) % base_expected.size();
                compare_direct(*base, base_expected[index]);
                compare_direct(*overlay, overlay_expected[index]);
            }
        }));
    for (auto &worker : workers)
        worker.get();
    EXPECT_THROW((void)base->geometric_state(10, 399, core::Epoch{3600000000001LL}),
                 core::EphemerisCoverageError);
    EXPECT_THROW((void)base->geometric_state(999999, 399, core::Epoch{0}),
                 core::EphemerisCoverageError);
    compare_direct(*base, base_expected.front());
}

namespace {
constexpr std::array<double, 3> sun_origin{-8e7, 1e7, 2e7}, sun_velocity{.01, -.02, .03};
constexpr std::array<double, 3> earth_origin{2e7, 4e7, -3e7}, earth_velocity{20, -15, 3};
void linear_sun_kernel(Fixture &fixture, bool sun_gap = false, double earth_first = -4000) {
    const auto path = fixture.root / "a.bsp";
    std::filesystem::remove(path);
    SpiceInt handle = 0;
    spkopn_c(path.string().c_str(), "Linear Sun and Earth", 0, &handle);
    spice_ok();
    const auto segment = [&](int body, int centre, double first, double last,
                             const std::array<double, 3> &position,
                             const std::array<double, 3> &velocity) {
        double coefficients[6]{};
        for (std::size_t i = 0; i < 3; ++i) {
            coefficients[2 * i] = position[i] + velocity[i] * (first + last) / 2;
            coefficients[2 * i + 1] = velocity[i] * (last - first) / 2;
        }
        spkw02_c(handle, body, centre, "J2000", first, last, "Linear reference", last - first, 1, 1,
                 coefficients, first);
        spice_ok();
    };
    segment(3, 0, earth_first, 4000, earth_origin, earth_velocity);
    segment(399, 3, earth_first, 4000, {}, {});
    if (sun_gap) {
        segment(10, 0, -4000, -700, sun_origin, sun_velocity);
        segment(10, 0, -100, 4000, sun_origin, sun_velocity);
    } else
        segment(10, 0, -4000, 4000, sun_origin, sun_velocity);
    spkcls_c(handle);
    spice_ok();
    fixture.document["kernels"] =
        Json::array({fixture.kernel("naif0012.tls", "LSK"), fixture.kernel("a.bsp", "SPK")});
    fixture.save();
}
} // namespace
TEST(Spice, MovingSunLightTimeAndAberration) {
    for (bool custom_clock : {false, true}) {
        SCOPED_TRACE(custom_clock);
        Fixture fixture;
        if (custom_clock) {
            // Amplify the rate difference to detect omission or reversal of the
            // TAI/TDB velocity derivative. This is an artificial clock model.
            std::ofstream(fixture.root / "naif0012.tls", std::ios::app)
                << "\n\\begindata\nDELTET/K = 5\nDELTET/EB = 0.1\n"
                   "DELTET/M = (0.4 0.002)\n\\begintext\n";
        }
        linear_sun_kernel(fixture);
        const auto provider = fixture.load();
        provider->validate_spacecraft_apparent_sun_interval(core::Epoch{0},
                                                            core::Epoch{100000000000});
        for (double sign : {-1., 0., 1.}) {
            const std::array<double, 3> position{7e6, 2e6, -1e6};
            const std::array<double, 3> velocity{sign * 1000, sign * 7500, sign * -2000};
            const auto actual =
                provider->apparent_sun_from_spacecraft(core::Epoch{0}, position, velocity);
            const long double et = provider->to_tdb(core::Epoch{0}).seconds_past_j2000;
            const long double k = custom_clock ? 5 : 1.657e-3;
            const long double eb = custom_clock ? .1 : 1.671e-2;
            const long double m0 = custom_clock ? .4 : 6.239996;
            const long double m1 = custom_clock ? .002 : 1.99096871e-7;
            const auto mean = m0 + m1 * et;
            const auto rate =
                1 - k * std::cos(mean + eb * std::sin(mean)) * m1 * (1 + eb * std::cos(mean));
            EXPECT_NEAR(provider->tdb_seconds_per_tai_second(core::Epoch{0}),
                        static_cast<double>(1 / rate), 3e-16);
            EXPECT_THROW((void)provider->tdb_seconds_per_tai_second(core::Epoch{3600000000001LL}),
                         core::EphemerisCoverageError);
            constexpr long double light_speed = 299792.458L; // km / TDB second.
            std::array<long double, 3> observer{}, beta{}, ray{};
            long double distance2 = 0;
            for (std::size_t i = 0; i < 3; ++i) {
                observer[i] = earth_origin[i] + earth_velocity[i] * et + position[i] / 1000;
                beta[i] = (earth_velocity[i] + velocity[i] * rate / 1000) / light_speed;
                ray[i] = sun_origin[i] + sun_velocity[i] * et - observer[i];
                distance2 += ray[i] * ray[i];
            }
            // One Newtonian light-time iteration, with an analytic moving Sun.
            const auto emission = et - std::sqrt(distance2) / light_speed;
            distance2 = 0;
            for (std::size_t i = 0; i < 3; ++i) {
                ray[i] = sun_origin[i] + sun_velocity[i] * emission - observer[i];
                distance2 += ray[i] * ray[i];
            }
            const auto distance = std::sqrt(distance2);
            long double parallel = 0, transverse2 = 0;
            for (std::size_t i = 0; i < 3; ++i) {
                ray[i] /= distance;
                parallel += beta[i] * ray[i];
            }
            for (std::size_t i = 0; i < 3; ++i) {
                beta[i] -= parallel * ray[i];
                transverse2 += beta[i] * beta[i];
            }
            for (std::size_t i = 0; i < 3; ++i) {
                const auto expected =
                    distance * (ray[i] * std::sqrt(1 - transverse2) + beta[i]) * 1000;
                // Millimetre bound is ~10^-14 relative: double SPK/rotation roundoff.
                EXPECT_NEAR(actual[i], static_cast<double>(expected), 1e-3);
            }
        }
        const auto centre = provider->apparent_sun_from_spacecraft(core::Epoch{0}, {}, {});
        const auto earth = provider->apparent_sun_from_earth(core::Epoch{0});
        for (std::size_t i = 0; i < 3; ++i)
            EXPECT_NEAR(centre[i], earth[i], 1e-3);
        // A geometric Sun entry must never be returned for the same-epoch
        // light-time/aberration query, or vice versa.
        const auto geometric = provider->geometric_state(10, 399, core::Epoch{0});
        EXPECT_NE(geometric.position_m, earth);
        EXPECT_EQ(provider->apparent_sun_from_earth(core::Epoch{0}), earth);
        EXPECT_EQ(provider->geometric_state(10, 399, core::Epoch{0}).position_m,
                  geometric.position_m);
        EXPECT_THROW((void)provider->apparent_sun_from_spacecraft(core::Epoch{0}, {2e15, 0, 0}, {}),
                     core::EphemerisCoverageError);
        EXPECT_THROW((void)provider->apparent_sun_from_spacecraft(
                         core::Epoch{0}, {std::numeric_limits<double>::quiet_NaN(), 0, 0}, {}),
                     std::invalid_argument);
        EXPECT_THROW((void)provider->apparent_sun_from_spacecraft(core::Epoch{0}, {}, {4e8, 0, 0}),
                     std::invalid_argument);
        EXPECT_EQ(provider->apparent_sun_from_spacecraft(core::Epoch{0}, {}, {}), centre);
    }
}
TEST(Spice, MovingSunCertifiesHistoryAndGaps) {
    Fixture fixture;
    linear_sun_kernel(fixture, true);
    {
        const auto provider = fixture.load();
        // Receiving endpoints are covered, but retarded Sun history has a hole.
        provider->validate_earth_gravity_interval(core::Epoch{0}, core::Epoch{100000000000}, true,
                                                  false);
        EXPECT_THROW(provider->validate_spacecraft_apparent_sun_interval(core::Epoch{0},
                                                                         core::Epoch{100000000000}),
                     core::EphemerisCoverageError);
        EXPECT_THROW((void)provider->apparent_sun_from_spacecraft(core::Epoch{0}, {}, {}),
                     core::EphemerisCoverageError);
        // A cached reception-time Sun must not conceal a hole at emission.
        EXPECT_NO_THROW((void)provider->geometric_state(10, 399, core::Epoch{0}));
        EXPECT_THROW((void)provider->apparent_sun_from_earth(core::Epoch{0}),
                     core::EphemerisCoverageError);
        const auto valid = provider->apparent_sun_from_earth(core::Epoch{1000000000000LL});
        EXPECT_THROW((void)provider->apparent_sun_from_earth(core::Epoch{0}),
                     core::EphemerisCoverageError);
        EXPECT_EQ(provider->apparent_sun_from_earth(core::Epoch{1000000000000LL}), valid);
    }
    linear_sun_kernel(fixture, false, -100);
    const auto provider = fixture.load();
    // Earth is needed at reception only; requiring ancient Earth data would be
    // spurious when the full past Sun chain is available.
    EXPECT_NO_THROW(provider->validate_spacecraft_apparent_sun_interval(core::Epoch{0},
                                                                        core::Epoch{100000000000}));
    EXPECT_NO_THROW((void)provider->apparent_sun_from_spacecraft(core::Epoch{0}, {}, {}));
    EXPECT_THROW(provider->validate_spacecraft_apparent_sun_interval(core::Epoch{-1000000000000},
                                                                     core::Epoch{0}),
                 core::EphemerisCoverageError);
    EXPECT_THROW(provider->validate_spacecraft_apparent_sun_interval(core::Epoch{100000000000},
                                                                     core::Epoch{0}),
                 core::EphemerisCoverageError);
    EXPECT_THROW(provider->validate_spacecraft_apparent_sun_interval(core::Epoch{-3600000000001},
                                                                     core::Epoch{0}),
                 core::EphemerisCoverageError);
}

namespace {
void planetary_test_kernel(const std::filesystem::path &path, int bad_centre = 0,
                           const char *frame = "J2000", bool include_planets = true) {
    std::filesystem::remove(path);
    SpiceInt handle = 0;
    spkopn_c(path.string().c_str(), "Synthetic planetary subset", 0, &handle);
    spice_ok();
    const double coefficients[]{7000, 10, 2, .5, -1200, .2, 3, 0, 500, -.3, 0, .6};
    for (int id : {10, 4, 499, 599, 699, 799, 899, 999}) {
        if (!include_planets && id >= 499)
            continue;
        const int centre = id < 499 ? 0 : (id == 599 && bad_centre ? bad_centre : id / 100);
        spkw02_c(handle, id, centre, frame, -3600, 3600, "Original synthetic segment", 7200, 1, 3,
                 coefficients, -3600);
        spice_ok();
    }
    spkcls_c(handle);
    spice_ok();
}
} // namespace
TEST(Spice, PlanetarySubsetsRetainSixCentresAndMatchIndependentChebyshevValues) {
    Fixture input;
    const auto source = input.root / "planets.bsp";
    planetary_test_kernel(source);
    const auto directory =
        input.root /
        ("subset-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto ids = providers::spice::extract_planetary_centres(source, directory);
    ASSERT_EQ(ids, (std::vector<std::int32_t>{499, 599, 699, 799, 899, 999}));
    input.document["kernels"] = Json::array({input.kernel("naif0012.tls", "LSK")});
    for (auto id : ids) {
        const auto name = "centre-" + std::to_string(id) + ".bsp";
        std::filesystem::copy_file(directory / name, input.root / name,
                                   std::filesystem::copy_options::overwrite_existing);
        input.document["kernels"].push_back(input.kernel(name, "SPK_BODY_CENTRES"));
    }
    input.save();
    const auto provider = input.load();
    for (auto id : ids)
        for (auto ns : {-3000000000000LL, 0LL, 1234567890123LL, 3000000000000LL})
            analytic(provider->geometric_state(id, id / 100, core::Epoch(ns)));
    EXPECT_THROW((void)provider->geometric_state(10, 0, core::Epoch{0}),
                 core::EphemerisCoverageError);
    const auto original = read(directory / "centre-499.bsp");
    EXPECT_THROW((void)providers::spice::extract_planetary_centres(source, directory),
                 core::EphemerisDataError);
    EXPECT_EQ(read(directory / "centre-499.bsp"), original);
    // A role declaration cannot authorize unrelated targets to override the base chain.
    std::filesystem::copy_file(source, input.root / "centre-599.bsp",
                               std::filesystem::copy_options::overwrite_existing);
    input.document["kernels"] = Json::array(
        {input.kernel("naif0012.tls", "LSK"), input.kernel("centre-599.bsp", "SPK_BODY_CENTRES")});
    input.save();
    EXPECT_THROW((void)input.load(), core::EphemerisDataError);
    analytic(provider->geometric_state(599, 5, core::Epoch{0}));
}
TEST(Spice, PlanetaryExtractionRejectsWrongChainsAxesAndCleansOnlyItsCreatedFiles) {
    Fixture input;
    const auto source = input.root / "planet-source.bsp";
    const auto directory =
        input.root /
        ("partial-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "keep.txt") << "caller-owned";
    for (const auto wrong : {1, 2, 3}) {
        planetary_test_kernel(source, wrong == 1 ? 4 : 0, wrong == 2 ? "ECLIPJ2000" : "J2000",
                              wrong != 3);
        EXPECT_THROW((void)providers::spice::extract_planetary_centres(source, directory),
                     core::EphemerisDataError);
        EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory),
                                std::filesystem::directory_iterator{}),
                  1);
        EXPECT_EQ(read(directory / "keep.txt"), "caller-owned");
    }
    planetary_test_kernel(source);
    unsigned checks = 0;
    EXPECT_THROW((void)providers::spice::extract_planetary_centres(
                     source, directory,
                     [&] {
                         if (++checks == 5)
                             throw std::runtime_error("cancel after writing two planets");
                     }),
                 std::runtime_error);
    EXPECT_EQ(checks, 5U);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory),
                            std::filesystem::directory_iterator{}),
              1);
}

namespace {
using SunVector = std::array<double, 3>;
using SunMatrix = std::array<SunVector, 3>;
double maximum(const SunMatrix &a) {
    double result = 0;
    for (const auto &row : a)
        for (double x : row)
            result = std::max(result, std::abs(x));
    return result;
}
// The caller has activated its immutable kernel set. Reference calls use the
// original CSPICE spkapo, not the provider's explicit LT/stelab implementation.
SunVector reference_apparent(double et, const SunVector &r, const SunVector &v, double rate,
                             const char *correction = "LT+S") {
    double observer[6]{}, output[3]{}, light_time = 0;
    spkssb_c(399, et, "J2000", observer);
    spice_ok();
    for (unsigned i = 0; i < 3; ++i) {
        observer[i] += r[i] / 1000;
        observer[i + 3] += v[i] * rate / 1000;
    }
    spkapo_c(10, et, "J2000", observer, correction, output, &light_time);
    spice_ok();
    return {output[0] * 1000, output[1] * 1000, output[2] * 1000};
}
double reference_rate(double et, bool custom) {
    // Independently specified LSK constants, including the exaggerated test clock.
    const long double k = custom ? 5 : 1.657e-3;
    const long double eb = custom ? .1 : 1.671e-2;
    const long double m0 = custom ? .4 : 6.239996;
    const long double m1 = custom ? .002 : 1.99096871e-7;
    const auto mean = m0 + m1 * et;
    return static_cast<double>(1 - k * std::cos(mean + eb * std::sin(mean)) * m1 *
                                       (1 + eb * std::cos(mean)));
}
template <class Function>
SunMatrix independent_sun_difference(Function function, SunVector r, SunVector v, double h,
                                     bool velocity) {
    SunMatrix result{};
    constexpr std::array weights{4. / 5, -1. / 5, 4. / 105, -1. / 280};
    for (unsigned j = 0; j < 3; ++j)
        for (unsigned k = 0; k < weights.size(); ++k) {
            auto rp = r, rm = r, vp = v, vm = v;
            (velocity ? vp : rp)[j] += (k + 1) * h;
            (velocity ? vm : rm)[j] -= (k + 1) * h;
            const auto plus = function(rp, vp), minus = function(rm, vm);
            for (unsigned i = 0; i < 3; ++i)
                result[i][j] += weights[k] * (plus[i] - minus[i]) / h;
        }
    return result;
}
class DifferentialSolarSystem final : public core::IThirdBodyEphemeris {
    const core::IEphemerisProvider &provider_;
    core::ThirdBodyDataInfo info_{};

  public:
    explicit DifferentialSolarSystem(const core::IEphemerisProvider &p) : provider_(p) {}
    void validate_spacecraft_apparent_sun_interval(core::Epoch a, core::Epoch b) const override {
        provider_.validate_spacecraft_apparent_sun_interval(a, b);
    }
    void validate_apparent_sun_interval(core::Epoch a, core::Epoch b) const override {
        provider_.validate_apparent_sun_interval(a, b);
    }

    const core::ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
    void validate_interval(core::Epoch, core::Epoch,
                           std::span<const core::ThirdBody>) const override {}
    SunVector position(core::ThirdBody body, core::Epoch epoch) const override {
        return provider_.geometric_state(core::third_body_naif_id(body), 399, epoch).position_m;
    }
    SunVector apparent_sun_from_earth(core::Epoch epoch) const override {
        return provider_.apparent_sun_from_earth(epoch);
    }
    SunVector apparent_sun_from_spacecraft(core::Epoch epoch, const SunVector &r,
                                           const SunVector &v) const override {
        return provider_.apparent_sun_from_spacecraft(epoch, r, v);
    }
    core::ApparentSunDifferential
    apparent_sun_from_spacecraft_differential(core::Epoch epoch, const SunVector &r,
                                              const SunVector &v) const override {
        return provider_.apparent_sun_from_spacecraft_differential(epoch, r, v);
    }
};
void check_apparent_derivatives(const core::IEphemerisProvider &provider, core::Epoch epoch,
                                bool custom_clock, double &max_r, double &max_v) {
    const auto et = provider.to_tdb(epoch).seconds_past_j2000;
    const auto rate = reference_rate(et, custom_clock);
    for (const auto &r : std::array{SunVector{7e6, 2e6, -1e6}, SunVector{-4.2e7, 1e7, -2e7},
                                    SunVector{1e10, -2e10, 3e10}})
        for (double sign : {-1., 0., 1.}) {
            SCOPED_TRACE(sign);
            const SunVector v{sign * 1000, sign * 7500, sign * -2000};
            const auto actual = provider.apparent_sun_from_spacecraft_differential(epoch, r, v);
            EXPECT_EQ(actual.position_m, provider.apparent_sun_from_spacecraft(epoch, r, v));
            const auto reference = [&](const SunVector &rr, const SunVector &vv) {
                return reference_apparent(et, rr, vv, rate);
            };
            const auto nominal = reference(r, v);
            for (unsigned i = 0; i < 3; ++i)
                EXPECT_NEAR(actual.position_m[i], nominal[i], 1e-3);
            for (bool velocity : {false, true}) {
                const double step = velocity ? 128 : 50000;
                const auto coarse = independent_sun_difference(reference, r, v, 2 * step, velocity);
                const auto fine = independent_sun_difference(reference, r, v, step, velocity);
                const auto &matrix =
                    velocity ? actual.velocity_jacobian_s : actual.position_jacobian;
                const double scale = maximum(fine);
                for (unsigned i = 0; i < 3; ++i)
                    for (unsigned j = 0; j < 3; ++j) {
                        EXPECT_NEAR(coarse[i][j], fine[i][j], 3e-9 * scale + 1e-10);
                        EXPECT_NEAR(matrix[i][j], fine[i][j], 5e-9 * scale + 1e-10);
                        auto &max = velocity ? max_v : max_r;
                        max = std::max(max, std::abs(matrix[i][j] - fine[i][j]) / scale);
                    }
            }
        }
}
} // namespace
TEST(Spice, ApparentSunDerivativesMatchIndependentSpkapoCartesianDifferences) {
    double max_r = 0, max_v = 0;
    for (bool custom : {false, true}) {
        SCOPED_TRACE(custom);
        Fixture fixture;
        if (custom)
            std::ofstream(fixture.root / "naif0012.tls", std::ios::app)
                << "\n\\begindata\nDELTET/K = 5\nDELTET/EB = 0.1\n"
                   "DELTET/M = (0.4 0.002)\n\\begintext\n";
        linear_sun_kernel(fixture);
        const auto provider = fixture.load();
        for (std::int64_t seconds : {-900, 0, 1800})
            check_apparent_derivatives(*provider, core::Epoch{seconds * 1000000000}, custom, max_r,
                                       max_v);
    }
    const auto provider = providers::spice::load_snapshot(
        ORBELLE_SPICE_SAMPLE,
        std::filesystem::path(ORBELLE_SPICE_TEST_DIR) / "apparent-derivative-real");
    for (double fraction : {.1, .5, .9}) {
        const auto &info = provider->data_info();
        const auto epoch =
            core::Epoch{info.first_epoch.tai_nanoseconds() +
                        static_cast<std::int64_t>(
                            (info.last_epoch.seconds_since(info.first_epoch) * fraction) * 1e9)};
        check_apparent_derivatives(*provider, epoch, false, max_r, max_v);
    }
    RecordProperty("cases", 81);
    RecordProperty("max_scaled_position_jacobian_error", max_r);
    RecordProperty("max_scaled_velocity_jacobian_error", max_v);
}
TEST(Spice, SrpStateChainMatchesIndependentApparentSunAndForceDifferences) {
    const auto provider = providers::spice::load_snapshot(
        ORBELLE_SPICE_SAMPLE,
        std::filesystem::path(ORBELLE_SPICE_TEST_DIR) / "srp-apparent-chain-real");
    const auto epoch =
        core::Epoch{provider->data_info().first_epoch.tai_nanoseconds() + 900000000000};
    DifferentialSolarSystem ephemeris(*provider);
    const auto geometric = ephemeris.position(core::ThirdBody::sun, epoch);
    const double length = std::hypot(geometric[0], geometric[1], geometric[2]);
    const double et = provider->to_tdb(epoch).seconds_past_j2000;
    const double rate = reference_rate(et, false);
    unsigned count = 0;
    double max_r = 0, max_v = 0, chain_fraction = 0;
    // The synthetic high speed makes the position light-time/aberration chain
    // resolvable separately from the much larger fixed-Sun position block.
    for (const auto &observer_velocity :
         std::array{SunVector{2000, 6500, -1000}, SunVector{4e7, 3e7, -2e7}})
        for (auto method : {core::SrpSunPositionMethod::geometric,
                            core::SrpSunPositionMethod::apparent_to_true_cb,
                            core::SrpSunPositionMethod::apparent})
            for (double radius : {7e6, 42164000.})
                for (double offset : {-2., -.5, .5, 2.}) {
                    SCOPED_TRACE(static_cast<int>(method));
                    SCOPED_TRACE(radius);
                    SCOPED_TRACE(offset);
                    core::SrpOptions options;
                    options.enabled = true;
                    options.moon_shadow = false;
                    options.sun_position_method = method;
                    options.area_mass_ratio_m2_per_kg = 10;
                    auto axis_sun = geometric;
                    if (method == core::SrpSunPositionMethod::apparent)
                        axis_sun = reference_apparent(et, {}, observer_velocity, rate);
                    SunVector n{}, tangent{0, 0, 1};
                    const double axis_length = std::hypot(axis_sun[0], axis_sun[1], axis_sun[2]);
                    for (unsigned i = 0; i < 3; ++i)
                        n[i] = axis_sun[i] / axis_length;
                    for (unsigned i = 0; i < 3; ++i)
                        tangent[i] -= n[2] * n[i];
                    const double tangent_length = std::hypot(tangent[0], tangent[1], tangent[2]);
                    for (double &value : tangent)
                        value /= tangent_length;
                    const double angle =
                        std::asin(options.earth_radius_m / radius) +
                        offset * std::asin(options.sun_radius_m / (length + radius));
                    core::CartesianState state{
                        epoch, core::OriginId::earth, core::FrameId::gcrf, {}, observer_velocity};
                    for (unsigned i = 0; i < 3; ++i)
                        state.position_m[i] =
                            radius * (-std::cos(angle) * n[i] + std::sin(angle) * tangent[i]);
                    const auto actual = core::srp_state_differential(state, options, ephemeris);
                    core::HpopOptions combined_options;
                    combined_options.gravity_degree = combined_options.gravity_order = 0;
                    combined_options.srp = options;
                    core::HpopEnvironment combined_environment;
                    combined_environment.ephemeris =
                        std::make_shared<DifferentialSolarSystem>(*provider);
                    const auto combined = core::hpop_force_differential(state, combined_options,
                                                                        combined_environment);
                    const auto central =
                        core::builtin_egm96()->differential(state.position_m, 0, 0);
                    EXPECT_EQ(combined.force.acceleration_mps2,
                              core::evaluate_hpop_acceleration(state, combined_options,
                                                               combined_environment));
                    EXPECT_EQ(combined.force.velocity_jacobian_per_s,
                              actual.force.velocity_jacobian_per_s);
                    EXPECT_EQ(combined.srp_parameter_jacobian_pa, actual.srp_parameter_jacobian_pa);
                    for (unsigned i = 0; i < 3; ++i)
                        for (unsigned j = 0; j < 3; ++j)
                            EXPECT_EQ(combined.force.position_jacobian_per_s2[i][j],
                                      central.position_jacobian_per_s2[i][j] +
                                          actual.force.position_jacobian_per_s2[i][j]);

                    if (method == core::SrpSunPositionMethod::apparent &&
                        observer_velocity[0] > 1e6) {
                        auto selected_sun =
                            reference_apparent(et, state.position_m, state.velocity_mps, rate);
                        for (unsigned i = 0; i < 3; ++i)
                            selected_sun[i] += state.position_m[i];
                        const auto local =
                            core::srp_differential(state.position_m, selected_sun, options);
                        const double scale = maximum(actual.force.position_jacobian_per_s2);
                        if (scale > 0)
                            for (unsigned i = 0; i < 3; ++i)
                                for (unsigned j = 0; j < 3; ++j)
                                    chain_fraction = std::max(
                                        chain_fraction,
                                        std::abs(actual.force.position_jacobian_per_s2[i][j] -
                                                 local.force.position_jacobian_per_s2[i][j]) /
                                            scale);
                    }
                    // Complete nominal force, with the Sun obtained independently via CSPICE.
                    // The disk evaluator has its own 641 independent reference cases.
                    const auto reference = [&](const SunVector &r, const SunVector &v) {
                        SunVector sun = geometric;
                        if (method == core::SrpSunPositionMethod::apparent_to_true_cb)
                            sun = reference_apparent(et, {}, {}, rate);
                        if (method == core::SrpSunPositionMethod::apparent) {
                            sun = reference_apparent(et, r, v, rate);
                            for (unsigned i = 0; i < 3; ++i)
                                sun[i] += r[i];
                        }
                        return core::evaluate_srp(r, sun, options).acceleration_mps2;
                    };
                    const auto nominal = reference(state.position_m, state.velocity_mps);
                    for (unsigned i = 0; i < 3; ++i)
                        EXPECT_NEAR(actual.force.acceleration_mps2[i], nominal[i], 2e-14);
                    for (bool velocity : {false, true}) {
                        const double step = 128;
                        const auto fine = independent_sun_difference(
                            reference, state.position_m, state.velocity_mps, step, velocity);
                        const auto coarse = independent_sun_difference(
                            reference, state.position_m, state.velocity_mps, 2 * step, velocity);
                        const auto &matrix = velocity ? actual.force.velocity_jacobian_per_s
                                                      : actual.force.position_jacobian_per_s2;
                        const double scale = maximum(fine);
                        for (unsigned i = 0; i < 3; ++i)
                            for (unsigned j = 0; j < 3; ++j) {
                                EXPECT_NEAR(coarse[i][j], fine[i][j], 5e-7 * scale + 2e-23);
                                EXPECT_NEAR(matrix[i][j], fine[i][j], 1e-6 * scale + 2e-23);
                                auto &max = velocity ? max_v : max_r;
                                max = std::max(max, std::abs(matrix[i][j] - fine[i][j]) /
                                                        (scale + 2e-17));
                            }
                    }
                    ++count;
                }
    EXPECT_EQ(count, 48U);
    EXPECT_GT(chain_fraction, 1e-5);
    RecordProperty("resolved_position_chain_fraction", chain_fraction);
    RecordProperty("max_scaled_force_position_jacobian_error", max_r);
    RecordProperty("max_scaled_force_velocity_jacobian_error", max_v);
}

TEST(Spice, ApparentSunDerivativeFailuresPreserveUsableProvider) {
    Fixture fixture;
    linear_sun_kernel(fixture);
    const auto provider = fixture.load();
    const auto good = provider->apparent_sun_from_spacecraft_differential(core::Epoch{0}, {}, {});
    EXPECT_THROW((void)provider->apparent_sun_from_spacecraft_differential(
                     core::Epoch{0}, {std::numeric_limits<double>::quiet_NaN(), 0, 0}, {}),
                 std::invalid_argument);
    EXPECT_THROW(
        (void)provider->apparent_sun_from_spacecraft_differential(core::Epoch{0}, {}, {4e8, 0, 0}),
        std::invalid_argument);
    EXPECT_THROW(
        (void)provider->apparent_sun_from_spacecraft_differential(core::Epoch{0}, {2e15, 0, 0}, {}),
        core::EphemerisCoverageError);
    EXPECT_THROW((void)provider->apparent_sun_from_spacecraft_differential(
                     core::Epoch{-3599000000000LL}, {}, {}),
                 core::EphemerisCoverageError);
    const auto again = provider->apparent_sun_from_spacecraft_differential(core::Epoch{0}, {}, {});
    EXPECT_EQ(again.position_m, good.position_m);
    EXPECT_EQ(again.position_jacobian, good.position_jacobian);
    EXPECT_EQ(again.velocity_jacobian_s, good.velocity_jacobian_s);
    EXPECT_GT(maximum(good.velocity_jacobian_s), 100);
}

TEST(Spice, ApparentSunPartialsRemainSmoothAtZeroAndParallelBarycentricVelocity) {
    Fixture fixture;
    linear_sun_kernel(fixture);
    const auto provider = fixture.load();
    const core::Epoch epoch{0};
    const SunVector r{7e6, 2e6, -1e6};
    const auto earth = provider->geometric_state(399, 0, epoch);
    const double et = earth.tdb.seconds_past_j2000, rate = reference_rate(et, false);
    const auto ray = reference_apparent(et, r, {}, rate, "LT");
    const double distance = std::hypot(ray[0], ray[1], ray[2]);
    for (double speed : {0., -.5, .5}) {
        SCOPED_TRACE(speed);
        SunVector v{};
        for (unsigned i = 0; i < 3; ++i)
            v[i] = (speed * 299792458. * ray[i] / distance - earth.velocity_mps[i]) / rate;
        const auto actual = provider->apparent_sun_from_spacecraft_differential(epoch, r, v);
        EXPECT_GT(maximum(actual.velocity_jacobian_s), 100);
        for (unsigned i = 0; i < 3; ++i)
            EXPECT_NEAR(actual.position_m[i], ray[i], 1e-3);
        const auto reference = [&](const SunVector &rr, const SunVector &vv) {
            return reference_apparent(et, rr, vv, rate);
        };
        for (bool velocity : {false, true}) {
            const double step = velocity ? 128 : 50000;
            const auto fine = independent_sun_difference(reference, r, v, step, velocity);
            const auto coarse = independent_sun_difference(reference, r, v, 2 * step, velocity);
            const auto &matrix = velocity ? actual.velocity_jacobian_s : actual.position_jacobian;
            for (unsigned i = 0; i < 3; ++i)
                for (unsigned j = 0; j < 3; ++j) {
                    EXPECT_NEAR(coarse[i][j], fine[i][j], 3e-9 * maximum(fine) + 1e-10);
                    EXPECT_NEAR(matrix[i][j], fine[i][j], 5e-9 * maximum(fine) + 1e-10);
                }
        }
    }
}
