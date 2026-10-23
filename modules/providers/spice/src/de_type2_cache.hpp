#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cspice/SpiceUsr.h>
#include <cstdint>
#include <optional>
#include <vector>

namespace orbelle::providers::spice::detail {
// Exact evaluation of the common J2000 Sun/Earth/Moon chains. These are the
// original SPK type-2 coefficients, not a fit to sampled ephemerides. See NAIF's
// SPK Required Reading, "Data Precedence" and "Type 2: Chebyshev (position only)".
// All access is under the provider's CSPICE mutex. No DAF handles are retained:
// they become invalid when another immutable snapshot activates its kernels.
class DeType2Cache {
    using State = std::array<double, 6>;
    struct Directory {
        double initial = 0, interval = 0;
        SpiceInt size = 0, count = 0;
    };
    struct Segment {
        std::size_t file;
        double first, last;
        SpiceInt centre, frame, type, begin, end;
        bool inspected = false;
        std::optional<Directory> directory;
    };
    struct Record {
        std::size_t file;
        SpiceInt begin, index;
        std::vector<double> data;
    };
    struct Value {
        int body, centre;
        double et;
        State state;
    };
    // Bound metadata as well as the record/value caches. Unusual large kernel
    // sets use the general CSPICE path instead of growing an unbounded index.
    std::array<std::vector<Segment>, 4> segments_;
    std::size_t segment_count_ = 0;
    bool indexed_ = true;
    std::array<std::optional<Record>, 16> records_;
    std::array<std::optional<Value>, 16> values_;
    std::size_t next_record_ = 0, next_value_ = 0;
    static int slot(int body) {
        switch (body) {
        case 10:
            return 0;
        case 399:
            return 1;
        case 301:
            return 2;
        case 3:
            return 3;
        default:
            return -1;
        }
    }
    template <typename Read>
    std::optional<State> primitive(int body, int centre, double et, const Read &read) {
        if (!indexed_)
            return {};
        for (const auto &value : values_)
            if (value && value->body == body && value->centre == centre && value->et == et)
                return value->state;
        auto &segments = segments_[slot(body)];
        // Last loaded file, then last segment in that file, wins. Do not skip an
        // unsupported higher-priority segment in favour of an older type-2 one.
        const auto found = std::find_if(segments.rbegin(), segments.rend(), [&](const auto &s) {
            return et >= s.first && et <= s.last;
        });
        if (found == segments.rend() || found->type != 2 || found->frame != 1 ||
            found->centre != centre)
            return {};
        auto &segment = *found;
        if (!segment.inspected) {
            if (segment.begin <= 0 || segment.end < segment.begin ||
                segment.end - segment.begin < 4)
                return {};
            double directory[4]{};
            read(segment.file, segment.end - 3, segment.end, directory);
            segment.inspected = true;
            // A bounded record supports degrees 0..128, including every DE440s
            // record. Validate floating directory entries before integer casts.
            if (!std::isfinite(directory[0]) || !std::isfinite(directory[1]) || directory[1] <= 0 ||
                !(directory[2] >= 5 && directory[2] <= 389) ||
                std::floor(directory[2]) != directory[2] || !(directory[3] >= 1) ||
                directory[3] > (segment.end - segment.begin - 3) / directory[2] ||
                std::floor(directory[3]) != directory[3])
                return {};
            const auto size = static_cast<SpiceInt>(directory[2]);
            const auto count = static_cast<SpiceInt>(directory[3]);
            if ((size - 2) % 3 != 0 ||
                static_cast<std::int64_t>(size) * count != segment.end - segment.begin - 3)
                return {};
            segment.directory = Directory{directory[0], directory[1], size, count};
        }
        if (!segment.directory)
            return {};
        const auto &d = *segment.directory;
        const double offset = (et - d.initial) / d.interval;
        if (!(offset >= 0 && offset <= d.count))
            return {};
        // The final endpoint belongs to the last record, as in SPKR02.
        const auto index = std::min(static_cast<SpiceInt>(offset), d.count - 1);
        const std::vector<double> *data = nullptr;
        for (const auto &record : records_)
            if (record && record->file == segment.file && record->begin == segment.begin &&
                record->index == index) {
                data = &record->data;
                break;
            }
        if (!data) {
            Record record{segment.file, segment.begin, index,
                          std::vector<double>(static_cast<std::size_t>(d.size))};
            const auto address = segment.begin + index * d.size;
            read(segment.file, address, address + d.size - 1, record.data.data());
            if (!(record.data[1] > 0) ||
                !std::all_of(record.data.begin(), record.data.end(),
                             [](double value) { return std::isfinite(value); }))
                return {};
            records_[next_record_] = std::move(record);
            data = &records_[next_record_]->data;
            next_record_ = (next_record_ + 1) % records_.size();
        }
        State result{};
        const auto coefficients = (d.size - 2) / 3;
        for (int axis = 0; axis < 3; ++axis)
            chbint_c(data->data() + 2 + axis * coefficients, coefficients - 1, data->data(), et,
                     &result[axis], &result[axis + 3]);
        if (!std::all_of(result.begin(), result.end(),
                         [](double value) { return std::isfinite(value); }))
            return {};
        values_[next_value_] = Value{body, centre, et, result};
        next_value_ = (next_value_ + 1) % values_.size();
        return result;
    }

  public:
    // Called in manifest file order and forward DAF segment order, once during
    // snapshot validation. Include unsupported descriptors to preserve priority.
    void add(std::size_t file, const double *bounds, const SpiceInt *descriptor) {
        const auto body = slot(descriptor[0]);
        if (body < 0 || !indexed_)
            return;
        if (++segment_count_ > 4096) {
            indexed_ = false;
            for (auto &segments : segments_)
                segments.clear();
            return;
        }
        segments_[body].push_back({file,
                                   bounds[0],
                                   bounds[1],
                                   descriptor[1],
                                   descriptor[2],
                                   descriptor[3],
                                   descriptor[4],
                                   descriptor[5],
                                   false,
                                   {}});
    }
    template <typename Read>
    std::optional<State> geometric(int target, int observer, double et, const Read &read) {
        if (target == 10 && observer == 0)
            return primitive(10, 0, et, read);
        if (target == 399 && observer == 0) {
            auto earth = primitive(399, 3, et, read);
            const auto emb = primitive(3, 0, et, read);
            if (earth && emb) {
                for (unsigned i = 0; i < 6; ++i)
                    (*earth)[i] += (*emb)[i];
                return earth;
            }
        }
        if ((target == 10 || target == 301) && observer == 399) {
            auto body = primitive(target, target == 10 ? 0 : 3, et, read);
            const auto earth =
                target == 10 ? geometric(399, 0, et, read) : primitive(399, 3, et, read);
            if (body && earth) {
                for (unsigned i = 0; i < 6; ++i)
                    (*body)[i] -= (*earth)[i];
                return body;
            }
        }
        return {};
    }
    template <typename Read>
    std::optional<std::array<double, 3>> apparent_sun(double et, const Read &read) {
        const auto earth = geometric(399, 0, et, read);
        const auto sun = primitive(10, 0, et, read);
        if (!earth || !sun)
            return {};
        std::array<double, 3> ray{}, result{};
        for (unsigned i = 0; i < 3; ++i)
            ray[i] = (*sun)[i] - (*earth)[i];
        // SPKEZP's J2000 LT+S path: one reception light-time iteration, then
        // stellar aberration with the observer's reception-time SSB velocity.
        const double emission = et - vnorm_c(ray.data()) / clight_c();
        const auto emitted = primitive(10, 0, emission, read);
        if (!emitted)
            return {};
        for (unsigned i = 0; i < 3; ++i)
            ray[i] = (*emitted)[i] - (*earth)[i];
        stelab_c(ray.data(), earth->data() + 3, result.data());
        return result;
    }
};
} // namespace orbelle::providers::spice::detail
