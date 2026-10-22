#include <orbelle/providers/sgp4/provider.hpp>
#include <orbelle/runtime/sgp4.hpp>

namespace orbelle::runtime {
application::Session make_sgp4_session(const core::Tle &tle, const core::ITimeProvider &time,
                                       std::string object_id, std::uint64_t input_revision) {
    return {providers::sgp4::make_sgp4(tle, time),
            {std::move(object_id), input_revision,
             "sgp4-vallado-2023-05-09-wgs72-improved-teme-tai-v1;utc=" + time.data_info().sha256}};
}
} // namespace orbelle::runtime
