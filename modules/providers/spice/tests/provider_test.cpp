#include <cmath>
#include <cspice/SpiceUsr.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <orbitforge/providers/spice/provider.hpp>
#include <picosha2.h>
#ifdef ORBITFORGE_SPICE_TEST_ERFA
#include <erfa.h>
#include <orbitforge/providers/erfa/provider.hpp>
#endif

using namespace orbitforge;
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
    spkopn_c(path.string().c_str(), "OrbitForge synthetic Chebyshev cubic", 0, &handle);
    spice_ok();
    const double coefficients[]{7000 + offset, 10, 2, 0.5, -1200, 0.2, 3, 0, 500, -0.3, 0, 0.6};
    spkw02_c(handle, 100001, 399, "J2000", -3600, 3600, "OrbitForge synthetic cubic", 7200, 1, 3,
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
        root = std::filesystem::path(ORBITFORGE_SPICE_TEST_DIR) /
               ::testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::create_directories(root);
        manifest = root / "manifest.json";
        workspace = root / "runtime";
        std::filesystem::copy_file(
            std::filesystem::path(ORBITFORGE_SPICE_SAMPLE).parent_path() / "naif0012.tls",
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
                {"source_sha256", hash}, {"producer", "OrbitForge test suite"}};
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
    for (int body : {3, 10, 399, 301}) {
        for (const auto &interval : {std::pair{-3600., -100.}, std::pair{100., 3600.}}) {
            spkw02_c(handle, body, body < 11 ? 0 : 3, "J2000", interval.first, interval.second,
                     "synthetic segment", interval.second - interval.first, 1, 3, coefficients,
                     interval.first);
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

#ifdef ORBITFORGE_SPICE_TEST_ERFA
TEST(Spice, RealSnapshotSunMoonAndIndependentTdbModel) {
    const auto workspace = std::filesystem::path(ORBITFORGE_SPICE_TEST_DIR) / "real-runtime";
    const auto provider = providers::spice::load_snapshot(ORBITFORGE_SPICE_SAMPLE, workspace);
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
