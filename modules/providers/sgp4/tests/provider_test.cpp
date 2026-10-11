#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <orbitforge/providers/sgp4/provider.hpp>
#include <sstream>

using namespace orbitforge;
namespace {
// Verification files specify minutes relative to each TLE epoch. This clock assigns
// that epoch to zero, isolating the propagation model from UTC conversion tests.
class RelativeClock final : public core::ITimeProvider {
  public:
    core::Epoch from_utc(const core::UtcDateTime &) const override { return core::Epoch{0}; }
    core::UtcDateTime to_utc(core::Epoch) const override { throw std::logic_error("unused"); }
    int tai_minus_utc_seconds(core::Epoch) const override { throw std::logic_error("unused"); }
    core::JulianDate julian_date(core::Epoch, core::TimeScale) const override {
        throw std::logic_error("unused");
    }
    const core::TimeDataInfo &data_info() const noexcept override { return info; }
    core::TimeDataInfo info{"relative-test-clock", {}, {}, 0, 0};
};
core::Tle vanguard() {
    return {"1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753",
            "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667"};
}
void checksum(std::string &line) {
    int sum = 0;
    for (std::size_t i = 0; i < 68; ++i)
        sum += line[i] >= '0' && line[i] <= '9' ? line[i] - '0' : line[i] == '-' ? 1 : 0;
    line[68] = static_cast<char>('0' + sum % 10);
}
} // namespace
TEST(Sgp4, Matches157PublishedNearEarthDeepSpaceAndResonanceStates) {
    const RelativeClock time;
    std::size_t states = 0;
    for (const auto *id :
         {"00005", "04632", "06251", "08195", "14128", "24208", "25954", "28057"}) {
        SCOPED_TRACE(id);
        std::ifstream file(std::filesystem::path{ORBITFORGE_SGP4_REFERENCE} /
                           (std::string(id) + ".txt"));
        ASSERT_TRUE(file);
        core::Tle tle;
        std::getline(file, tle.line1);
        std::getline(file, tle.line2);
        const auto provider = providers::sgp4::make_sgp4(tle, time);
        double minutes = 0;
        while (file >> minutes) {
            SCOPED_TRACE(minutes);
            std::array<double, 3> r{}, v{};
            for (auto &x : r)
                ASSERT_TRUE(file >> x);
            for (auto &x : v)
                ASSERT_TRUE(file >> x);
            const auto epoch = core::Epoch{static_cast<std::int64_t>(minutes * 60e9)};
            const auto state = provider->state_at(epoch);
            EXPECT_EQ(state.frame, core::FrameId::teme);
            EXPECT_EQ(state.epoch, epoch);
            for (std::size_t i = 0; i < 3; ++i) {
                EXPECT_NEAR(state.position_m[i], r[i] * 1000, .001);
                EXPECT_NEAR(state.velocity_mps[i], v[i] * 1000, .000002);
            }
            ++states;
        }
    }
    EXPECT_EQ(states, 157U);
}
TEST(Sgp4, DeepSpaceQueriesAreOrderIndependentAndThreadSafe) {
    std::ifstream file(std::filesystem::path{ORBITFORGE_SGP4_REFERENCE} / "08195.txt");
    core::Tle tle;
    std::getline(file, tle.line1);
    std::getline(file, tle.line2);
    const auto provider = providers::sgp4::make_sgp4(tle, RelativeClock{});
    const auto epoch = core::Epoch{86'400'000'000'000};
    const auto expected = provider->state_at(epoch);
    std::vector<std::future<core::CartesianState>> queries;
    for (int i = 0; i < 16; ++i)
        queries.push_back(std::async(std::launch::async, [&, i] {
            (void)provider->state_at(core::Epoch{(i % 2 ? -1 : 1) * 123'456'000'000'000LL});
            return provider->state_at(epoch);
        }));
    for (auto &query : queries) {
        const auto state = query.get();
        EXPECT_EQ(state.position_m, expected.position_m);
        EXPECT_EQ(state.velocity_mps, expected.velocity_mps);
    }
}
TEST(Tle, ParsesExactNanosecondsAlpha5AndCenturyBoundary) {
    auto tle = vanguard();
    const auto elements = core::parse_tle(tle);
    EXPECT_EQ(elements.epoch_utc, (core::UtcDateTime{2000, 6, 27, 18, 50, 19, 733568000}));
    EXPECT_EQ(elements.catalog_id, "00005");
    tle.line1.replace(2, 5, "E8493");
    tle.line2.replace(2, 5, "E8493");
    tle.line1.replace(18, 14, "56001.00000001");
    checksum(tle.line1);
    checksum(tle.line2);
    EXPECT_EQ(core::parse_tle(tle).epoch_utc, (core::UtcDateTime{2056, 1, 1, 0, 0, 0, 864000}));
    tle.line1.replace(18, 2, "57");
    checksum(tle.line1);
    EXPECT_EQ(core::parse_tle(tle).epoch_utc.year, 1957);
}
TEST(Tle, RejectsMalformedMismatchedAndOutOfRangeInputs) {
    const auto original = vanguard();
    for (const auto &bad : {core::Tle{original.line1 + " ", original.line2},
                            core::Tle{original.line1.substr(1), original.line2}})
        EXPECT_THROW((void)core::parse_tle(bad), std::invalid_argument);
    auto bad = original;
    bad.line1[68] = '0';
    EXPECT_THROW((void)core::parse_tle(bad), std::invalid_argument);
    bad = original;
    bad.line2.replace(2, 5, "00006");
    checksum(bad.line2);
    EXPECT_THROW((void)core::parse_tle(bad), std::invalid_argument);
    for (const auto *epoch : {"01366.00000000", "00-01.00000000", "00367.00000000"}) {
        bad = original;
        bad.line1.replace(18, 14, epoch);
        checksum(bad.line1);
        EXPECT_THROW((void)core::parse_tle(bad), std::invalid_argument);
    }
    for (const auto *inclination : {"181.0000", "     nan", " 1.0e001"}) {
        bad = original;
        bad.line2.replace(8, 8, inclination);
        checksum(bad.line2);
        EXPECT_THROW((void)core::parse_tle(bad), std::invalid_argument);
    }
    bad = original;
    bad.line2.replace(52, 11, " 0.00000000");
    checksum(bad.line2);
    EXPECT_THROW((void)core::parse_tle(bad), std::invalid_argument);
    for (char invalid : {'b', '@', '\t'}) {
        bad = original;
        bad.line1[14] = invalid;
        checksum(bad.line1);
        EXPECT_THROW((void)core::parse_tle(bad), std::invalid_argument);
    }
    bad = original;
    bad.line2.replace(17, 8, "360.0001");
    checksum(bad.line2);
    EXPECT_THROW((void)core::parse_tle(bad), std::invalid_argument);
}
TEST(Tle, AcceptsNoradDayZeroBlankExponentFieldsAndSpacePadding) {
    auto tle = vanguard();
    tle.line1.replace(2, 5, "    5");
    tle.line2.replace(2, 5, "    5");
    tle.line1.replace(18, 14, "98  0.50000001");
    tle.line1.replace(44, 8, "        ");
    tle.line1.replace(53, 8, "        ");
    tle.line2.replace(17, 8, "360.0000");
    checksum(tle.line1);
    checksum(tle.line2);
    const auto elements = core::parse_tle(tle);
    EXPECT_EQ(elements.catalog_id, "00005");
    EXPECT_EQ(elements.epoch_utc, (core::UtcDateTime{1997, 12, 31, 12, 0, 0, 864000}));
    EXPECT_EQ(elements.bstar, 0);
    EXPECT_EQ(elements.mean_motion_ddot_over_six, 0);
    EXPECT_EQ(elements.raan_rad, 0);
    auto equivalent = tle;
    equivalent.line1.replace(18, 14, "97365.50000001");
    equivalent.line1.replace(44, 8, " 00000-0");
    equivalent.line1.replace(53, 8, " 00000+0");
    equivalent.line2.replace(17, 8, "  0.0000");
    checksum(equivalent.line1);
    checksum(equivalent.line2);
    EXPECT_DOUBLE_EQ(elements.epoch_days_since_1950,
                     core::parse_tle(equivalent).epoch_days_since_1950);
    const RelativeClock time;
    const auto actual = providers::sgp4::make_sgp4(tle, time)->state_at(core::Epoch{0});
    const auto expected = providers::sgp4::make_sgp4(equivalent, time)->state_at(core::Epoch{0});
    EXPECT_EQ(actual.position_m, expected.position_m);
    EXPECT_EQ(actual.velocity_mps, expected.velocity_mps);
    tle.line1.replace(18, 14, " 0  1.00000000");
    checksum(tle.line1);
    EXPECT_EQ(core::parse_tle(tle).epoch_utc, (core::UtcDateTime{2000, 1, 1, 0, 0, 0, 0}));
}
TEST(Sgp4, DecayedSatelliteFailsInsteadOfReturningAnInvalidState) {
    auto tle = vanguard();
    tle.line2.replace(26, 7, "0000001");
    tle.line2.replace(52, 11, "18.00000000");
    checksum(tle.line2);
    EXPECT_THROW((void)providers::sgp4::make_sgp4(tle, RelativeClock{}), core::PropagationError);
}
