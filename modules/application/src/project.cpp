#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <orbitforge/application/project.hpp>
#include <random>
#include <set>
#include <system_error>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace orbitforge::application {
namespace {
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
constexpr std::size_t max_file = 64 * 1024 * 1024, max_text = 16384, max_objects = 10000;
constexpr std::array<std::uint8_t, 8> magic{'O', 'R', 'B', 'F', 'P', 'R', 'J', '\n'};
constexpr std::uint32_t scene_tag = 0x454e4353, objects_tag = 0x534a424f, data_tag = 0x41544144,
                        lock_tag = 0x4b434f4c;
[[noreturn]] void invalid(const char *message) { throw ProjectError(message); }
void check(bool condition, const char *message) {
    if (!condition)
        invalid(message);
}
bool valid_utf8(const std::string &text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i++]);
        if (c == 0)
            return false;
        if (c < 0x80)
            continue;
        unsigned count = 0, code = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) {
            count = 1;
            code = c & 0x1f;
            minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            count = 2;
            code = c & 0x0f;
            minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            count = 3;
            code = c & 7;
            minimum = 0x10000;
        } else
            return false;
        if (count > text.size() - i)
            return false;
        while (count--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80)
                return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            return false;
    }
    return true;
}
void text_check(const std::string &value, bool required = false) {
    check(value.size() <= max_text && valid_utf8(value), "Invalid or oversized UTF-8 project text");
    if (required)
        check(!value.empty() && !std::all_of(value.begin(), value.end(),
                                             [](unsigned char c) { return std::isspace(c) != 0; }),
              "Project names and object IDs must not be blank");
}
bool hash_valid(const std::string &value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffff;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return ~crc;
}
struct Writer {
    std::vector<std::uint8_t> bytes;
    void raw(std::span<const std::uint8_t> data) {
        check(data.size() <= max_file - bytes.size(), "Project exceeds 64 MiB");
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    void number(std::uint64_t value, unsigned width = 8) {
        std::array<std::uint8_t, 8> data{};
        for (unsigned i = 0; i < width; ++i)
            data[i] = static_cast<std::uint8_t>(value >> (8 * i));
        raw(std::span(data).first(width));
    }
    void text(const std::string &value) {
        text_check(value);
        number(value.size(), 4);
        raw({reinterpret_cast<const std::uint8_t *>(value.data()), value.size()});
    }
    void epoch(core::Epoch value) { number(std::bit_cast<std::uint64_t>(value.tai_nanoseconds())); }
    void real(double value) { number(std::bit_cast<std::uint64_t>(value)); }
    void section(std::uint32_t tag, const Writer &data, unsigned version = 1) {
        number(tag, 4);
        number(version, 4);
        number(data.bytes.size());
        number(crc32(data.bytes), 4);
        raw(data.bytes);
    }
};
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::span<const std::uint8_t> raw(std::size_t length) {
        check(length <= bytes.size(), "Truncated project file");
        const auto result = bytes.first(length);
        bytes = bytes.subspan(length);
        return result;
    }
    std::uint64_t number(unsigned width = 8) {
        const auto data = raw(width);
        std::uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= static_cast<std::uint64_t>(data[i]) << (8 * i);
        return value;
    }
    std::string text() {
        const auto size = number(4);
        check(size <= max_text, "Oversized project string");
        const auto value = raw(static_cast<std::size_t>(size));
        std::string result(reinterpret_cast<const char *>(value.data()), value.size());
        text_check(result);
        return result;
    }
    core::Epoch epoch() { return core::Epoch{std::bit_cast<std::int64_t>(number())}; }
    double real() { return std::bit_cast<double>(number()); }
    bool boolean() {
        const auto value = number(1);
        check(value <= 1, "Invalid project boolean");
        return value != 0;
    }
    void done() const { check(bytes.empty(), "Unexpected trailing project data"); }
};
void write_data(Writer &out, const core::EopDataInfo &data) {
    out.text(data.id);
    out.text(data.sha256);
    out.text(data.source_url);
    out.text(data.reference_frame);
    out.text(data.interpolation);
    out.epoch(data.first_epoch);
    out.epoch(data.last_epoch);
    out.text(data.time_data.id);
    out.text(data.time_data.sha256);
    out.text(data.time_data.source_url);
    out.number(static_cast<std::uint32_t>(data.time_data.utc_valid_from_mjd), 4);
    out.number(static_cast<std::uint32_t>(data.time_data.utc_valid_until_mjd), 4);
}
core::EopDataInfo read_data(Reader &in) {
    core::EopDataInfo result{
        in.text(), in.text(),  in.text(),  in.text(),
        in.text(), in.epoch(), in.epoch(), {in.text(), in.text(), in.text(), 0, 0}};
    result.time_data.utc_valid_from_mjd = static_cast<int>(in.number(4));
    result.time_data.utc_valid_until_mjd = static_cast<int>(in.number(4));
    return result;
}
} // namespace
Project::Project(std::string project_name, core::Epoch begin, core::Epoch end)
    : name(std::move(project_name)), first(begin), last(end), current(begin) {}
DataLock Project::data_lock() const {
    try {
        return earth_orientation ? eop_data_lock(*earth_orientation) : DataLock{};
    } catch (const std::invalid_argument &error) {
        throw ProjectError(error.what());
    }
}
void validate_project(const Project &project) {
    text_check(project.name, true);
    const auto first = project.first.tai_nanoseconds(), last = project.last.tai_nanoseconds();
    check(first < last && project.last.seconds_since(project.first) < 9e9,
          "Invalid project analysis interval");
    check(project.current.tai_nanoseconds() >= first && project.current.tai_nanoseconds() <= last,
          "Project current time is outside its analysis interval");
    check(project.satellites.size() <= max_objects, "Project exceeds 10000 satellites");
    std::set<std::string> ids;
    for (const auto &satellite : project.satellites) {
        const auto &input = satellite.orbit;
        text_check(input.object_id, true);
        text_check(satellite.display_name, !satellite.display_name.empty());
        check(ids.insert(input.object_id).second, "Duplicate project object ID");
        check(input.first == project.first && input.last == project.last,
              "Satellite interval must match the project");
        check(input.propagator == OrbitPropagator::two_body ||
                  input.propagator == OrbitPropagator::j2_secular,
              "Unsupported project propagator");
        check(input.initial.origin == core::OriginId::earth &&
                  input.initial.frame == core::FrameId::gcrf &&
                  (input.input_frame == core::FrameId::gcrf ||
                   input.input_frame == core::FrameId::itrf),
              "Unsupported project origin or reference frame");
        for (const auto value : input.initial.position_m)
            check(std::isfinite(value), "Nonfinite project position");
        for (const auto value : input.initial.velocity_mps)
            check(std::isfinite(value), "Nonfinite project velocity");
        check(input.step_seconds >= 1 && input.step_seconds <= 60, "Invalid display step");
        const auto &display = satellite.display;
        check(display.extent >= 0 && display.extent <= 2 && std::isfinite(display.past_minutes) &&
                  std::isfinite(display.future_minutes) && display.past_minutes >= 0 &&
                  display.future_minutes >= 0 && display.past_minutes <= 10080 &&
                  display.future_minutes <= 10080 &&
                  (display.extent != 2 || display.past_minutes + display.future_minutes > 0),
              "Invalid project track extent");
        text_check(satellite.model_path);
        check(satellite.model_kind >= 0 && satellite.model_kind <= 2 &&
                  std::isfinite(satellite.model_diameter_m) && satellite.model_diameter_m > 0 &&
                  (satellite.model_kind != 2 || !satellite.model_path.empty()),
              "Invalid project model settings");
    }
    if (project.earth_orientation) {
        const auto &data = *project.earth_orientation;
        for (const auto *value :
             {&data.id, &data.reference_frame, &data.interpolation, &data.time_data.id})
            text_check(*value, true);
        text_check(data.source_url);
        text_check(data.time_data.source_url);
        check(hash_valid(data.sha256) && hash_valid(data.time_data.sha256), "Invalid data SHA256");
        check(data.first_epoch.tai_nanoseconds() <= first &&
                  data.last_epoch.tai_nanoseconds() >= last &&
                  data.time_data.utc_valid_from_mjd >= 0 &&
                  data.time_data.utc_valid_until_mjd > data.time_data.utc_valid_from_mjd,
              "Project data does not cover the analysis interval");
    }
}
bool matching_project_data(const core::EopDataInfo &a, const core::EopDataInfo &b) {
    return a.id == b.id && a.sha256 == b.sha256 && a.reference_frame == b.reference_frame &&
           a.interpolation == b.interpolation && a.first_epoch == b.first_epoch &&
           a.last_epoch == b.last_epoch && a.time_data.id == b.time_data.id &&
           a.time_data.sha256 == b.time_data.sha256 &&
           a.time_data.utc_valid_from_mjd == b.time_data.utc_valid_from_mjd &&
           a.time_data.utc_valid_until_mjd == b.time_data.utc_valid_until_mjd;
}
namespace {
std::size_t satellite_index(const Project &project, const std::string &id) {
    const auto found = std::find_if(project.satellites.begin(), project.satellites.end(),
                                    [&](const auto &s) { return s.orbit.object_id == id; });
    check(found != project.satellites.end(), "Project object ID does not exist");
    return static_cast<std::size_t>(found - project.satellites.begin());
}
} // namespace
Project Project::renamed_satellite(const std::string &id, std::string new_name) const {
    text_check(new_name, true);
    auto result = *this;
    result.satellites.at(satellite_index(*this, id)).display_name = std::move(new_name);
    validate_project(result);
    return result;
}
Project Project::without_satellite(const std::string &id) const {
    auto result = *this;
    result.satellites.erase(result.satellites.begin() +
                            static_cast<std::ptrdiff_t>(satellite_index(*this, id)));
    validate_project(result);
    return result;
}
Project Project::with_satellite_copy(const std::string &id, std::string new_id,
                                     std::string new_name) const {
    text_check(new_name, true);
    auto satellite = satellites.at(satellite_index(*this, id));
    satellite.orbit.object_id = std::move(new_id);
    satellite.display_name = std::move(new_name);
    auto result = *this;
    result.satellites.push_back(std::move(satellite));
    validate_project(result);
    return result;
}
std::vector<std::uint8_t> encode_project(const Project &project) {
    validate_project(project);
    Writer scene, objects, payload, file;
    scene.text(project.name);
    scene.epoch(project.first);
    scene.epoch(project.last);
    scene.epoch(project.current);
    scene.text(std::string(core::central_body(core::OriginId::earth).model));
    payload.section(scene_tag, scene);
    objects.number(project.satellites.size(), 4);
    for (const auto &satellite : project.satellites) {
        const auto &input = satellite.orbit;
        objects.text(input.object_id);
        objects.text(satellite.display_name);
        objects.number(input.propagator == OrbitPropagator::two_body ? 1 : 2, 4);
        objects.number(input.input_frame == core::FrameId::gcrf ? 1 : 2, 4);
        objects.epoch(input.initial.epoch);
        for (const auto value : input.initial.position_m)
            objects.real(value);
        for (const auto value : input.initial.velocity_mps)
            objects.real(value);
        objects.number(static_cast<unsigned>(input.step_seconds), 4);
        objects.number(static_cast<unsigned>(satellite.display.extent), 4);
        objects.real(satellite.display.past_minutes);
        objects.real(satellite.display.future_minutes);
        objects.number(satellite.display.track_2d, 1);
        objects.number(satellite.display.labels_2d, 1);
        objects.number(satellite.display.track_3d, 1);
        objects.number(static_cast<unsigned>(satellite.model_kind), 4);
        objects.real(satellite.model_diameter_m);
        objects.text(satellite.model_path);
    }
    payload.section(objects_tag, objects, 2);
    if (project.earth_orientation) {
        Writer data;
        write_data(data, *project.earth_orientation);
        payload.section(data_tag, data);
    }
    const auto lock = project.data_lock();
    Writer locked;
    locked.raw(lock.canonical_bytes());
    locked.text(lock.sha256());
    payload.section(lock_tag, locked);
    file.raw(magic);
    file.number(3, 4);
    file.number(payload.bytes.size());
    file.number(crc32(payload.bytes), 4);
    file.raw(payload.bytes);
    return std::move(file.bytes);
}
Project decode_project(std::span<const std::uint8_t> bytes) {
    check(bytes.size() <= max_file, "Project exceeds 64 MiB");
    Reader file{bytes};
    check(std::ranges::equal(file.raw(magic.size()), magic), "Not an OrbitForge project file");
    const auto version = file.number(4);
    check(version >= 1 && version <= 3, "Unsupported OrbitForge project version");
    const auto length = file.number();
    const auto checksum = file.number(4);
    check(length == file.bytes.size(), "Project length mismatch");
    check(crc32(file.bytes) == checksum, "Project checksum mismatch");
    std::map<std::uint32_t, std::span<const std::uint8_t>> sections;
    while (!file.bytes.empty()) {
        const auto tag = static_cast<std::uint32_t>(file.number(4));
        check(tag == scene_tag || tag == objects_tag || tag == data_tag ||
                  (version >= 3 && tag == lock_tag),
              "Unsupported project section");
        check(file.number(4) == (tag == objects_tag && version >= 2 ? 2U : 1U),
              "Unsupported project section version");
        const auto size = file.number();
        const auto section_crc = file.number(4);
        check(size <= file.bytes.size(), "Truncated project section");
        const auto section = file.raw(static_cast<std::size_t>(size));
        check(crc32(section) == section_crc, "Project section checksum mismatch");
        check(sections.emplace(tag, section).second, "Duplicate project section");
    }
    check(sections.contains(scene_tag) && sections.contains(objects_tag),
          "Missing project section");
    Reader scene{sections.at(scene_tag)};
    const auto name = scene.text();
    const auto first = scene.epoch(), last = scene.epoch();
    Project project{name, first, last};
    project.current = scene.epoch();
    check(scene.text() == core::central_body(core::OriginId::earth).model,
          "Unsupported project central-body model");
    scene.done();
    Reader objects{sections.at(objects_tag)};
    const auto count = objects.number(4);
    check(count <= max_objects, "Project exceeds 10000 satellites");
    for (std::uint64_t i = 0; i < count; ++i) {
        ProjectSatellite satellite;
        auto &input = satellite.orbit;
        input.object_id = objects.text();
        if (version >= 2)
            satellite.display_name = objects.text();
        const auto propagator = objects.number(4), frame = objects.number(4);
        check(propagator == 1 || propagator == 2, "Unsupported project propagator");
        check(frame == 1 || frame == 2, "Unsupported input reference frame");
        input.propagator =
            propagator == 1 ? OrbitPropagator::two_body : OrbitPropagator::j2_secular;
        input.input_frame = frame == 1 ? core::FrameId::gcrf : core::FrameId::itrf;
        input.initial.epoch = objects.epoch();
        for (auto &value : input.initial.position_m)
            value = objects.real();
        for (auto &value : input.initial.velocity_mps)
            value = objects.real();
        input.first = first;
        input.last = last;
        input.step_seconds = static_cast<int>(objects.number(4));
        satellite.display.extent = static_cast<int>(objects.number(4));
        satellite.display.past_minutes = objects.real();
        satellite.display.future_minutes = objects.real();
        satellite.display.track_2d = objects.boolean();
        satellite.display.labels_2d = objects.boolean();
        satellite.display.track_3d = objects.boolean();
        satellite.model_kind = static_cast<int>(objects.number(4));
        satellite.model_diameter_m = objects.real();
        satellite.model_path = objects.text();
        project.satellites.push_back(std::move(satellite));
    }
    objects.done();
    if (sections.contains(data_tag)) {
        Reader data{sections.at(data_tag)};
        project.earth_orientation = read_data(data);
        data.done();
    }
    validate_project(project);
    if (version >= 3) {
        check(sections.contains(lock_tag), "Missing scenario data lock");
        Reader locked{sections.at(lock_tag)};
        check(locked.text() == "orbitforge-data-lock-v1", "Unsupported data lock encoding");
        const auto reference_count = locked.number(4);
        check(reference_count <= 64, "Data lock exceeds 64 references");
        std::vector<DataReference> references;
        for (std::uint64_t i = 0; i < reference_count; ++i)
            references.push_back(
                {locked.text(), locked.text(), locked.text(), locked.text(), locked.text()});
        try {
            const DataLock lock(std::move(references));
            check(lock.sha256() == locked.text(), "Scenario data lock SHA256 mismatch");
            check(lock == project.data_lock(),
                  "Scenario data references disagree with project metadata");
        } catch (const std::invalid_argument &error) {
            throw ProjectError(error.what());
        }
        locked.done();
    }
    return project;
}
Project load_project(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    check(input.is_open(), "Cannot open project file");
    const auto size = input.tellg();
    check(size >= 0 && size <= static_cast<std::streamoff>(max_file), "Invalid project file size");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(input), "Cannot read complete project file");
    check(input.peek() == std::char_traits<char>::eof(), "Project changed while reading");
    return decode_project(bytes);
}
void save_project(const std::filesystem::path &path, const Project &project, bool overwrite) {
    const auto bytes = encode_project(project);
    const auto target = std::filesystem::absolute(path);
    check(!target.filename().empty(), "A project filename is required");
    std::filesystem::path temporary;
    std::random_device random;
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto candidate = target.parent_path() / (".orbitforge-save-" + std::to_string(random()) +
                                                 "-" + std::to_string(random()));
        if (std::filesystem::create_directory(candidate)) {
            temporary = std::move(candidate);
            break;
        }
    }
    check(!temporary.empty(), "Cannot create project temporary directory");
    const auto staging = temporary / "project";
    const auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        std::filesystem::remove(temporary, ignored);
    };
    try {
        std::ofstream output(staging, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.close();
        check(static_cast<bool>(output), "Cannot write complete project file");
#ifdef _WIN32
        const auto handle = CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        check(handle != INVALID_HANDLE_VALUE, "Cannot flush project file");
        const bool flushed = FlushFileBuffers(handle) != 0;
        CloseHandle(handle);
        check(flushed, "Cannot flush project file");
        check(MoveFileExW(staging.c_str(), target.c_str(),
                          MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0)) !=
                  0,
              "Cannot publish project file (destination exists, is locked, or is not writable)");
#else
        const auto descriptor = ::open(staging.c_str(), O_RDONLY);
        check(descriptor >= 0, "Cannot flush project file");
        const auto flushed = ::fsync(descriptor);
        ::close(descriptor);
        check(flushed == 0, "Cannot flush project file");
        if (overwrite)
            std::filesystem::rename(staging, target);
        else
            check(::link(staging.c_str(), target.c_str()) == 0,
                  "Cannot publish project file (destination exists or is not writable)");
#endif
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}
} // namespace orbitforge::application
