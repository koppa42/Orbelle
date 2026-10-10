#include <orbitforge/runtime/data_catalog.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <stdexcept>

#ifdef ORBITFORGE_HAS_DATA_CATALOG
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <locale>
#include <mutex>
#include <nlohmann/json.hpp>
#include <picosha2.h>
#include <sqlite3.h>
#include <sstream>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <thread>
#endif

namespace orbitforge::runtime {
namespace {
using Json = nlohmann::json;
void check(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
std::string utf8(const std::filesystem::path &path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}
bool hash_valid(const std::string &value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
           });
}
std::string read(const std::filesystem::path &path, std::size_t limit = 16 * 1024 * 1024) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    check(input.is_open(), "Cannot read catalog file: " + utf8(path));
    const auto size = input.tellg();
    check(size >= 0 && static_cast<std::uint64_t>(size) <= limit,
          "Catalog file exceeds size limit");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(input) && input.peek() == std::char_traits<char>::eof(),
          "Cannot read a stable catalog file");
    return bytes;
}
void write(const std::filesystem::path &path, const std::string &bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
}
void publish(const std::filesystem::path &source, const std::filesystem::path &destination) {
    for (unsigned attempt = 0;; ++attempt) {
        std::error_code error;
        std::filesystem::rename(source, destination, error);
        if (!error)
            return;
#ifdef _WIN32
        // A short-lived handle in another process can deny publication on Windows.
        // Retry only sharing/access errors; never replace publication with a partial copy.
        if (attempt < 5 && error.category() == std::system_category() &&
            (error.value() == ERROR_ACCESS_DENIED || error.value() == ERROR_SHARING_VIOLATION ||
             error.value() == ERROR_LOCK_VIOLATION)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20U << attempt));
            continue;
        }
#else
        (void)attempt;
#endif
        throw std::filesystem::filesystem_error("Cannot publish catalog data", source, destination,
                                                error);
    }
}
struct Stage {
    std::filesystem::path path;
    explicit Stage(const std::filesystem::path &root) {
        std::filesystem::create_directories(root);
        for (unsigned i = 0; i < 10000; ++i) {
            auto candidate = root / ("stage-" + std::to_string(i));
            if (std::filesystem::create_directory(candidate)) {
                path = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("Cannot allocate data staging directory");
    }
    ~Stage() {
        if (path.empty())
            return;
        // Only exact direct children created by this operation; never recursive deletion.
        std::error_code error;
        for (auto it = std::filesystem::directory_iterator(path, error);
             !error && it != std::filesystem::directory_iterator(); it.increment(error))
            std::filesystem::remove(it->path(), error);
        std::filesystem::remove(path, error);
    }
};
struct Statement {
    sqlite3 *db;
    sqlite3_stmt *value = nullptr;
    Statement(sqlite3 *connection, const char *sql) : db(connection) {
        check(sqlite3_prepare_v2(db, sql, -1, &value, nullptr) == SQLITE_OK, sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(value); }
    void text(int index, const std::string &text) {
        check(text.size() <= 65536, "Catalog field exceeds limit");
        check(sqlite3_bind_text(value, index, text.data(), static_cast<int>(text.size()),
                                SQLITE_TRANSIENT) == SQLITE_OK,
              sqlite3_errmsg(db));
    }
    void integer(int index, std::int64_t number) {
        check(sqlite3_bind_int64(value, index, number) == SQLITE_OK, sqlite3_errmsg(db));
    }
    bool next() {
        const int code = sqlite3_step(value);
        check(code == SQLITE_ROW || code == SQLITE_DONE, sqlite3_errmsg(db));
        return code == SQLITE_ROW;
    }
    std::string text(int index) const {
        const auto *data = sqlite3_column_text(value, index);
        return data ? reinterpret_cast<const char *>(data) : "";
    }
};
void sql(sqlite3 *db, const char *command) {
    check(sqlite3_exec(db, command, nullptr, nullptr, nullptr) == SQLITE_OK, sqlite3_errmsg(db));
}
struct Transaction {
    sqlite3 *db;
    explicit Transaction(sqlite3 *connection) : db(connection) { sql(db, "BEGIN IMMEDIATE"); }
    ~Transaction() { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { sql(db, "COMMIT"); }
};
} // namespace
struct DataCatalog::Impl {
    std::filesystem::path root;
    sqlite3 *db = nullptr;
    mutable std::recursive_mutex mutex;
    ~Impl() {
        if (db)
            sqlite3_close(db);
    }
    std::filesystem::path directory(const std::string &key) const {
        check(hash_valid(key), "Invalid catalog snapshot hash");
        return root / "snapshots" / key;
    }
    CatalogSnapshot describe(const std::string &key, const core::EopDataInfo &info) const {
        CatalogSnapshot result{key,
                               info.id,
                               info.source_url,
                               info.first_epoch,
                               info.last_epoch,
                               directory(key) / "manifest.json"};
        Statement receipt(db, "SELECT kind,acquired_at FROM acquisitions WHERE lock_hash=?");
        receipt.text(1, key);
        if (receipt.next()) {
            result.acquisition = static_cast<DataAcquisition>(sqlite3_column_int(receipt.value, 0));
            if (sqlite3_column_type(receipt.value, 1) != SQLITE_NULL)
                result.acquired_at_unix_seconds = sqlite3_column_int64(receipt.value, 1);
        }
        return result;
    }
    std::unique_ptr<core::IEarthOrientationProvider> verify(const std::string &key) const {
        const auto folder = directory(key);
        check(std::filesystem::canonical(folder).parent_path() == root / "snapshots",
              "Catalog snapshot resolves outside its directory");
        const auto record = Json::parse(read(folder / "record.json", 65536));
        check((record.at("schema") == 1 || record.at("schema") == 2) &&
                  record.at("lock_sha256") == key,
              "Catalog record identity mismatch");
        const auto bytes = read(folder / "manifest.json", 65536);
        check(picosha2::hash256_hex_string(bytes) ==
                  record.at("manifest_sha256").get<std::string>(),
              "Catalog manifest SHA256 mismatch");
        auto clock = make_builtin_time_provider();
        if (record.at("schema") == 2) {
            check(std::filesystem::canonical(folder / "leap-seconds.dat").parent_path() == folder,
                  "Time data resolves outside its snapshot directory");
            clock = load_time_snapshot(folder / "leap-seconds.dat");
            check(clock->data_info().sha256 == record.at("time_sha256").get<std::string>(),
                  "Time data SHA256 mismatch");
        }
        auto provider = load_eop_snapshot_with_time(folder / "manifest.json", *clock);
        check(application::eop_data_lock(provider->data_info()).sha256() == key,
              "Catalog snapshot does not match its pinned data/time identity");
        const auto blob = root / "blobs" / "sha256" / provider->data_info().sha256;
        check(picosha2::hash256_hex_string(read(blob)) == provider->data_info().sha256,
              "Catalog content blob SHA256 mismatch");
        return provider;
    }
    void index(const std::string &key, const core::EopDataInfo &info) {
        Statement row(db, "INSERT OR IGNORE INTO snapshots VALUES(?, ?, ?, ?, ?)");
        row.text(1, key);
        row.text(2, info.id);
        row.text(3, info.source_url);
        row.integer(4, info.first_epoch.tai_nanoseconds());
        row.integer(5, info.last_epoch.tai_nanoseconds());
        row.next();
        const auto lock = application::eop_data_lock(info);
        for (const auto &ref : lock.references()) {
            Statement dep(db, "INSERT OR IGNORE INTO dependencies VALUES(?, ?, ?, ?, ?, ?)");
            dep.text(1, key);
            dep.text(2, ref.role);
            dep.text(3, ref.snapshot_id);
            dep.text(4, ref.content_sha256);
            dep.text(5, ref.interpretation_version);
            dep.text(6, ref.configuration_sha256);
            dep.next();
        }
    }
    void blob(const std::string &bytes, const std::string &hash,
              const std::filesystem::path &stage) {
        check(hash_valid(hash) && picosha2::hash256_hex_string(bytes) == hash,
              "Invalid blob SHA256");
        const auto target = root / "blobs" / "sha256" / hash;
        if (std::filesystem::exists(target)) {
            check(read(target) == bytes, "Existing content blob is damaged; refusing replacement");
            return;
        }
        const auto pending = stage / "blob.pending";
        write(pending, bytes);
        publish(pending, target);
    }
};
bool data_catalog_available() noexcept { return true; }
DataCatalog::DataCatalog(std::filesystem::path root) : impl_(std::make_unique<Impl>()) {
    std::filesystem::create_directories(root);
    impl_->root = std::filesystem::canonical(root);
    for (const auto &part : {"snapshots", "blobs/sha256", "staging", "time"})
        std::filesystem::create_directories(impl_->root / part);
    check(sqlite3_open_v2(utf8(impl_->root / "catalog.sqlite").c_str(), &impl_->db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                          nullptr) == SQLITE_OK,
          "Cannot open data catalog");
    sqlite3_busy_timeout(impl_->db, 10000);
    sql(impl_->db, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL");
    Transaction transaction(impl_->db);
    Statement version(impl_->db, "PRAGMA user_version");
    version.next();
    const auto schema = sqlite3_column_int(version.value, 0);
    version.next();
    check(schema >= 0 && schema <= 2, "Unsupported data catalog schema");
    sql(impl_->db,
        "CREATE TABLE IF NOT EXISTS snapshots(lock_hash TEXT PRIMARY KEY, id TEXT NOT NULL, "
        "source_url TEXT NOT NULL, first_ns INTEGER NOT NULL, last_ns INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS dependencies(lock_hash TEXT REFERENCES snapshots(lock_hash), "
        "role TEXT, snapshot_id TEXT, content_hash TEXT, interpretation TEXT, configuration_hash "
        "TEXT, PRIMARY KEY(lock_hash,role));"
        "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS acquisitions(lock_hash TEXT PRIMARY KEY REFERENCES "
        "snapshots(lock_hash), kind INTEGER NOT NULL, acquired_at INTEGER); PRAGMA user_version=2");
    // Recover only successfully published directories left by a process that stopped
    // between file publication and the SQLite commit; invalid records stay unindexed.
    for (const auto &entry : std::filesystem::directory_iterator(impl_->root / "snapshots")) {
        const auto key = entry.path().filename().string();
        if (!entry.is_directory() || !hash_valid(key))
            continue;
        Statement exists(impl_->db, "SELECT 1 FROM snapshots WHERE lock_hash=?");
        exists.text(1, key);
        if (exists.next())
            continue;
        try {
            impl_->index(key, impl_->verify(key)->data_info());
        } catch (const std::exception &) {
        }
    }
    transaction.commit();
}
DataCatalog::~DataCatalog() = default;
const std::filesystem::path &DataCatalog::root() const noexcept { return impl_->root; }
CatalogSnapshot DataCatalog::install_eop(const std::filesystem::path &manifest_path,
                                         const std::filesystem::path &time_file) {
    std::lock_guard guard(impl_->mutex);
    const auto clock =
        time_file.empty() ? make_builtin_time_provider() : load_time_snapshot(time_file);
    // Validate copied bytes before publication, even if the source changes after the first load.
    (void)load_eop_snapshot_with_time(manifest_path, *clock);
    const auto manifest_bytes = read(manifest_path, 65536);
    const auto manifest = Json::parse(manifest_bytes);
    const auto filename = std::filesystem::path(manifest.at("data_file").get<std::string>());
    check(!filename.empty() && !filename.has_parent_path() && filename != "manifest.json" &&
              filename != "record.json" && filename != "blob.pending" &&
              filename != "leap-seconds.dat",
          "Invalid snapshot data filename");
    const auto source = std::filesystem::canonical(manifest_path).parent_path();
    const auto data = std::filesystem::canonical(source / filename);
    check(data.parent_path() == source, "EOP data resolves outside its source directory");
    const auto data_bytes = read(data);
    Stage stage(impl_->root / "staging");
    write(stage.path / "manifest.json", manifest_bytes);
    write(stage.path / filename, data_bytes);
    if (!time_file.empty()) {
        write(stage.path / "leap-seconds.dat", read(time_file, 65536));
        check(load_time_snapshot(stage.path / "leap-seconds.dat")->data_info().sha256 ==
                  clock->data_info().sha256,
              "Time data changed during installation");
    }
    const auto provider = load_eop_snapshot_with_time(stage.path / "manifest.json", *clock);
    const auto &info = provider->data_info();
    const auto key = application::eop_data_lock(info).sha256();
    Transaction transaction(impl_->db);
    const auto destination = impl_->directory(key);
    if (std::filesystem::exists(destination)) {
        const auto existing = impl_->verify(key);
        impl_->index(key, existing->data_info());
        transaction.commit();
        return impl_->describe(key, existing->data_info());
    }
    impl_->blob(data_bytes, info.sha256, stage.path);
    std::filesystem::remove(stage.path / filename);
    std::error_code link_error;
    std::filesystem::create_hard_link(impl_->root / "blobs" / "sha256" / info.sha256,
                                      stage.path / filename, link_error);
    if (link_error)
        write(stage.path / filename, data_bytes);
    write(stage.path / "record.json",
          Json{{"schema", time_file.empty() ? 1 : 2},
               {"kind", "earth-orientation"},
               {"lock_sha256", key},
               {"manifest_sha256", picosha2::hash256_hex_string(manifest_bytes)},
               {"time_sha256", clock->data_info().sha256}}
              .dump(2));
    publish(stage.path, destination);
    stage.path.clear();
    impl_->index(key, info);
    transaction.commit();
    return impl_->describe(key, info);
}
std::vector<CatalogSnapshot> DataCatalog::snapshots() const {
    std::lock_guard guard(impl_->mutex);
    Statement rows(impl_->db,
                   "SELECT s.lock_hash,id,source_url,first_ns,last_ns,a.kind,a.acquired_at "
                   "FROM snapshots s LEFT JOIN acquisitions a ON s.lock_hash=a.lock_hash "
                   "ORDER BY COALESCE(a.acquired_at,0),s.rowid");
    std::vector<CatalogSnapshot> result;
    while (rows.next()) {
        const auto key = rows.text(0);
        result.push_back({key, rows.text(1), rows.text(2),
                          core::Epoch(sqlite3_column_int64(rows.value, 3)),
                          core::Epoch(sqlite3_column_int64(rows.value, 4)),
                          impl_->directory(key) / "manifest.json",
                          static_cast<DataAcquisition>(sqlite3_column_int(rows.value, 5)),
                          sqlite3_column_type(rows.value, 6) == SQLITE_NULL
                              ? std::nullopt
                              : std::optional<std::int64_t>{sqlite3_column_int64(rows.value, 6)}});
    }
    return result;
}
void DataCatalog::record_acquisition(const std::string &key, DataAcquisition kind,
                                     std::optional<std::int64_t> acquired_at) {
    std::lock_guard guard(impl_->mutex);
    check(hash_valid(key) && kind >= DataAcquisition::bundled &&
              kind <= DataAcquisition::downloaded,
          "Invalid data acquisition receipt");
    // Re-registering bundled resources on launch must never rewrite a real receipt.
    Statement row(impl_->db,
                  "INSERT INTO acquisitions VALUES(?,?,?) ON CONFLICT(lock_hash) DO UPDATE SET "
                  "kind=excluded.kind,acquired_at=excluded.acquired_at WHERE excluded.kind<>1");
    row.text(1, key);
    row.integer(2, static_cast<int>(kind));
    if (kind != DataAcquisition::bundled)
        row.integer(3, acquired_at.value_or(std::chrono::duration_cast<std::chrono::seconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count()));
    row.next();
}
std::unique_ptr<core::IEarthOrientationProvider>
DataCatalog::load_eop(const std::string &key) const {
    std::lock_guard guard(impl_->mutex);
    Statement exists(impl_->db, "SELECT 1 FROM snapshots WHERE lock_hash=?");
    exists.text(1, key);
    check(exists.next(), "Pinned EOP data is not installed: " + key);
    return impl_->verify(key);
}
std::unique_ptr<core::IEarthOrientationProvider>
DataCatalog::resolve_eop(const application::DataLock &lock) const {
    auto provider = load_eop(lock.sha256());
    check(application::eop_data_lock(provider->data_info()) == lock,
          "Data dependency lock mismatch");
    return provider;
}
std::string DataCatalog::setting(const std::string &key) const {
    std::lock_guard guard(impl_->mutex);
    Statement row(impl_->db, "SELECT value FROM settings WHERE key=?");
    row.text(1, key);
    return row.next() ? row.text(0) : "";
}
void DataCatalog::set_setting(const std::string &key, const std::string &value) {
    std::lock_guard guard(impl_->mutex);
    Statement row(
        impl_->db,
        "INSERT INTO settings VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    row.text(1, key);
    row.text(2, value);
    row.next();
}
void DataCatalog::set_settings(const std::map<std::string, std::string> &values) {
    std::lock_guard guard(impl_->mutex);
    Transaction transaction(impl_->db);
    for (const auto &[key, value] : values)
        set_setting(key, value);
    transaction.commit();
}
std::filesystem::path DataCatalog::time_file(const std::string &hash) const {
    std::lock_guard guard(impl_->mutex);
    check(hash_valid(hash), "Invalid UTC content hash");
    const auto file = impl_->root / "time" / (hash + ".dat");
    check(std::filesystem::canonical(file).parent_path() == impl_->root / "time",
          "UTC data resolves outside its catalog directory");
    check(load_time_snapshot(file)->data_info().sha256 == hash, "UTC content SHA256 mismatch");
    return file;
}
std::filesystem::path DataCatalog::install_time(const std::filesystem::path &source) {
    std::lock_guard guard(impl_->mutex);
    const auto expected = load_time_snapshot(source)->data_info().sha256;
    Stage stage(impl_->root / "staging");
    write(stage.path / "time.dat", read(source, 65536));
    const auto info = load_time_snapshot(stage.path / "time.dat")->data_info();
    check(info.sha256 == expected, "UTC file changed during installation");
    const auto target = impl_->root / "time" / (info.sha256 + ".dat");
    if (std::filesystem::exists(target))
        return time_file(info.sha256);
    publish(stage.path / "time.dat", target);
    return target;
}
CatalogSnapshot DataCatalog::import_c04(const std::filesystem::path &source, const std::string &url,
                                        const std::string &retrieved,
                                        const std::filesystem::path &time_file) {
    std::lock_guard guard(impl_->mutex);
    check(!url.empty() && !retrieved.empty(), "Source URL and retrieval date are required");
    const auto bytes = read(source);
    std::istringstream lines(bytes);
    std::string line;
    int first = -1, last = -1;
    while (std::getline(lines, line)) {
        const auto start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos || line[start] == '#')
            continue;
        std::istringstream row(line);
        row.imbue(std::locale::classic());
        int year = 0, month = 0, day = 0, hour = 0;
        double mjd = 0;
        check(static_cast<bool>(row >> year >> month >> day >> hour >> mjd) && std::isfinite(mjd) &&
                  mjd >= 37665 && mjd <= 100000 && mjd == std::floor(mjd),
              "Invalid C04 date fields");
        if (first < 0)
            first = static_cast<int>(mjd);
        last = static_cast<int>(mjd);
    }
    const auto clock =
        time_file.empty() ? make_builtin_time_provider() : load_time_snapshot(time_file);
    // Earlier records are retained in the full original file, but do not supply
    // the measured polar rates required by our C04 state interpolation.
    first = std::max({first, 45700, clock->data_info().utc_valid_from_mjd});
    last = std::min(last, clock->data_info().utc_valid_until_mjd - 1);
    check(last > first, "C04 lacks two complete daily records within supported coverage");
    const auto hash = picosha2::hash256_hex_string(bytes);
    const Json manifest{{"schema_version", 1},
                        {"snapshot_id", "iers-c04-all-" + hash.substr(0, 16)},
                        {"format", "iers-20-c04-0h-dxdy"},
                        {"data_file", "eop.c04"},
                        {"sha256", hash},
                        {"source_url", url},
                        {"source_sha256", hash},
                        {"retrieved_on", retrieved},
                        {"first_mjd", first},
                        {"last_mjd", last},
                        {"reference_frame", "ITRF2020"},
                        {"interpolation", "c04-hermite-tai-v1"}};
    Stage stage(impl_->root / "staging");
    write(stage.path / "manifest.json", manifest.dump(2));
    write(stage.path / "eop.c04", bytes);
    return install_eop(stage.path / "manifest.json", time_file);
}
CatalogSnapshot DataCatalog::import_finals(const std::filesystem::path &source,
                                           const std::string &url, const std::string &retrieved,
                                           const std::filesystem::path &time_file) {
    std::lock_guard guard(impl_->mutex);
    check(!url.empty() && !retrieved.empty(), "Source URL and retrieval date are required");
    const auto bytes = read(source);
    std::istringstream lines(bytes);
    std::string line;
    int first = -1, last = -1, previous = -1;
    bool incomplete_tail = false;
    const auto mjd_zero = std::chrono::sys_days(std::chrono::year(1858) / 11 / 17);
    while (std::getline(lines, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        check(line.size() >= 134, "Truncated finals2000A source row");
        const auto number = [&](std::size_t offset, std::size_t count) {
            std::istringstream field(line.substr(offset, count));
            field.imbue(std::locale::classic());
            double value = 0;
            check(static_cast<bool>(field >> value) && std::isfinite(value),
                  "Invalid finals2000A numeric field");
            field >> std::ws;
            check(field.eof(), "Invalid finals2000A numeric field");
            return value;
        };
        const auto day_number = number(7, 8);
        check(day_number >= 41317 && day_number <= 100000 && day_number == std::floor(day_number),
              "Invalid finals2000A MJD");
        const auto mjd = static_cast<int>(day_number);
        check(previous < 0 || mjd == previous + 1,
              "Missing, duplicate or unordered finals2000A row");
        previous = mjd;
        const auto calendar_field = [&](std::size_t offset) {
            const double value = number(offset, 2);
            check(value >= 0 && value <= 99 && value == std::floor(value),
                  "Invalid finals2000A date");
            return static_cast<int>(value);
        };
        const auto date = std::chrono::year(calendar_field(0) + (mjd <= 51543 ? 1900 : 2000)) /
                          static_cast<unsigned>(calendar_field(2)) /
                          static_cast<unsigned>(calendar_field(4));
        check(date.ok() && (std::chrono::sys_days(date) - mjd_zero).count() == mjd,
              "finals2000A date/MJD mismatch");
        // A component is either entirely absent in the prediction tail or fully
        // specified. A damaged field inside an otherwise present group is an error.
        const auto group = [&](std::size_t flag, std::size_t end,
                               std::initializer_list<std::pair<std::size_t, std::size_t>> fields) {
            if (line.substr(flag, end - flag).find_first_not_of(" \t\r") == std::string::npos)
                return false;
            check(line[flag] == 'I' || line[flag] == 'P', "Invalid finals2000A source flag");
            std::size_t index = 0;
            for (const auto &[offset, count] : fields) {
                const auto value = number(offset, count);
                check(index++ % 2 == 0 || value >= 0, "Negative finals2000A uncertainty");
            }
            return true;
        };
        const bool polar = group(16, 55, {{18, 9}, {27, 9}, {37, 9}, {46, 9}});
        const bool ut1 = group(57, 78, {{58, 10}, {68, 10}});
        const bool pole = group(95, 134, {{97, 9}, {106, 9}, {116, 9}, {125, 9}});
        if (!(polar && ut1 && pole)) {
            incomplete_tail = true;
            continue;
        }
        check(!incomplete_tail, "Incomplete finals2000A record before complete daily records");
        if (first < 0)
            first = mjd;
        last = mjd;
    }
    const auto clock =
        time_file.empty() ? make_builtin_time_provider() : load_time_snapshot(time_file);
    first = std::max(first, clock->data_info().utc_valid_from_mjd);
    last = std::min(last, clock->data_info().utc_valid_until_mjd - 1);
    check(last > first, "finals2000A lacks two complete daily records within time-data coverage");
    const auto hash = picosha2::hash256_hex_string(bytes);
    const Json manifest{{"schema_version", 1},
                        {"snapshot_id", "iers-finals2000a-all-" + hash.substr(0, 16)},
                        {"format", "iers-finals2000a"},
                        {"data_file", "finals2000A.all"},
                        {"sha256", hash},
                        {"source_url", url},
                        {"source_sha256", hash},
                        {"retrieved_on", retrieved},
                        {"first_mjd", first},
                        {"last_mjd", last},
                        {"reference_frame", "ITRS"},
                        {"interpolation", "finals2000a-hermite-tai-v1"}};
    Stage stage(impl_->root / "staging");
    write(stage.path / "manifest.json", manifest.dump(2));
    write(stage.path / "finals2000A.all", bytes);
    return install_eop(stage.path / "manifest.json", time_file);
}
} // namespace orbitforge::runtime
#else
namespace orbitforge::runtime {
struct DataCatalog::Impl {
    std::filesystem::path root;
};
namespace {
[[noreturn]] void unavailable() { throw std::runtime_error("Data catalog support is not enabled"); }
} // namespace
bool data_catalog_available() noexcept { return false; }
DataCatalog::DataCatalog(std::filesystem::path) { unavailable(); }
DataCatalog::~DataCatalog() = default;
const std::filesystem::path &DataCatalog::root() const noexcept { return impl_->root; }
CatalogSnapshot DataCatalog::install_eop(const std::filesystem::path &,
                                         const std::filesystem::path &) {
    unavailable();
}
CatalogSnapshot DataCatalog::import_finals(const std::filesystem::path &, const std::string &,
                                           const std::string &, const std::filesystem::path &) {
    unavailable();
}
CatalogSnapshot DataCatalog::import_c04(const std::filesystem::path &, const std::string &,
                                        const std::string &, const std::filesystem::path &) {
    unavailable();
}
std::filesystem::path DataCatalog::install_time(const std::filesystem::path &) { unavailable(); }
std::filesystem::path DataCatalog::time_file(const std::string &) const { unavailable(); }
std::vector<CatalogSnapshot> DataCatalog::snapshots() const { unavailable(); }
void DataCatalog::record_acquisition(const std::string &, DataAcquisition,
                                     std::optional<std::int64_t>) {
    unavailable();
}
std::unique_ptr<core::IEarthOrientationProvider> DataCatalog::load_eop(const std::string &) const {
    unavailable();
}
std::unique_ptr<core::IEarthOrientationProvider>
DataCatalog::resolve_eop(const application::DataLock &) const {
    unavailable();
}
std::string DataCatalog::setting(const std::string &) const { unavailable(); }
void DataCatalog::set_setting(const std::string &, const std::string &) { unavailable(); }
void DataCatalog::set_settings(const std::map<std::string, std::string> &) { unavailable(); }
} // namespace orbitforge::runtime
#endif
