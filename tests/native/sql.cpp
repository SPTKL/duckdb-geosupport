#include "duckdb.h"
#include "nlohmann/json.hpp"
#include "picosha2/picosha2.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/utsname.h>
#include <unistd.h>
using json = nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
int checks = 0;
void Check(bool ok, const std::string &label) { if (!ok) throw std::runtime_error(label); ++checks; }
std::string Quote(const std::string &s) {
    std::string out = "'"; for (char c : s) { out += c; if (c == '\'') out += c; } return out + "'";
}
std::string Read(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read fixture " + path);
    return {std::istreambuf_iterator<char>(file), {}};
}
struct Result {
    duckdb_result result{};
    Result(duckdb_connection con, const std::string &sql) {
        if (duckdb_query(con, sql.c_str(), &result) != DuckDBSuccess) {
            std::string message = duckdb_result_error(&result); duckdb_destroy_result(&result);
            throw std::runtime_error(message + "\nSQL: " + sql);
        }
    }
    ~Result() { duckdb_destroy_result(&result); }
    std::string Value(idx_t col = 0, idx_t row = 0) {
        if (duckdb_value_is_null(&result, col, row)) return "<NULL>";
        char *s = duckdb_value_varchar(&result, col, row); std::string out(s); duckdb_free(s); return out;
    }
};
struct DB {
    duckdb_database db = nullptr;
    duckdb_connection con = nullptr;
    DB(const std::string &extension, const std::string &path = ":memory:") {
        duckdb_config config; duckdb_create_config(&config);
        duckdb_set_config(config, "allow_unsigned_extensions", "true");
        char *error = nullptr;
        auto state = duckdb_open_ext(path.c_str(), &db, config, &error); duckdb_destroy_config(&config);
        if (state != DuckDBSuccess) { std::string message = error ? error : "duckdb_open failed"; duckdb_free(error); throw std::runtime_error(message); }
        if (duckdb_connect(db, &con) != DuckDBSuccess) throw std::runtime_error("duckdb_connect failed");
        Exec("LOAD " + Quote(extension));
    }
    ~DB() { duckdb_disconnect(&con); duckdb_close(&db); }
    void Exec(const std::string &sql) { Result r(con, sql); }
    std::string Value(const std::string &sql) { return Result(con, sql).Value(); }
    void Assert(const std::string &expression) { Check(Value("SELECT (" + expression + ")::BOOLEAN") == "true", expression); }
    void Fails(const std::string &sql, const std::string &match) {
        try { Exec(sql); } catch (const std::exception &e) {
            Check(std::string(e.what()).find(match) != std::string::npos, "Expected " + match + ", got " + e.what()); return;
        }
        throw std::runtime_error("Expected SQL error: " + sql);
    }
};
const std::string Address = "gs_address('125','Worth St','Mn',release := '26c')";
void Unit(const std::string &extension) {
    std::string temp = (fs::temp_directory_path() / "gs-sql-unit-XXXXXX").string();
    if (!mkdtemp(temp.data())) throw std::runtime_error("mkdtemp");
    auto home = fs::path(temp) / "store"; setenv("GEOSUPPORT_HOME", home.c_str(), 1);
    DB db(extension);
    db.Assert("(SELECT count(*) FROM gs_available_releases()) = 2");
    db.Assert("(SELECT count(*) FROM gs_installed_releases()) = 0");
    db.Assert("(SELECT count(*) FROM gs_runtime_info()) = 0");
    db.Exec("EXPLAIN SELECT * FROM gs_install('unknown')");
    db.Exec("EXPLAIN SELECT " + Address);
    Check(!fs::exists(home), "EXPLAIN has side effects");
    db.Fails("SELECT " + Address, "not installed");
    Check(!fs::exists(home), "Missing release creates store");
    db.Fails("SELECT gs_address('125','Worth St','Mn')", "Select a release");
    db.Fails("CALL gs_load(NULL)", "cannot be NULL");
    db.Fails("SELECT gs_call('BL','bad json',release := '26c')", "parse_error");
    fs::remove_all(temp);
}
void E2E(const std::string &extension) {
    Check(std::getenv("GEOSUPPORT_HOME") != nullptr, "GEOSUPPORT_HOME is required for real-engine tests");
    DB db(extension);
    for (const std::string release : {"26b", "26c"}) {
        std::cout << "Installing/verifying real " << release << std::endl;
        db.Exec("CALL gs_install(" + Quote(release) + ")");
        auto id = db.Value("SELECT bundle_id FROM gs_load(" + Quote(release) + ")");
        Check(id.size() == 64, "Bundle identity length");
        Check(db.Value("SELECT bundle_id FROM gs_install(" + Quote(release) + ")") == id, "Install idempotency");
        db.Exec("CALL gs_verify(" + Quote(id) + ")");
        auto geo = json::parse(db.Value("SELECT to_json(gs_address('125','Worth St','Mn',release := " + Quote(id) + "))"));
        Check(geo.at("bbl") == "1001680032" && geo.at("release") == release && geo.at("bundle_id") == id, "Known address identity");
        Check(geo.at("grc") == "00" || geo.at("grc") == "01", "Known address GRC");
        Check(geo.at("latitude").is_number() && geo.at("longitude").is_number(), "Typed coordinates");
        auto raw = json::parse(geo.at("raw").get<std::string>());
        Check(raw.at("BOROUGH BLOCK LOT (BBL)").at("BOROUGH BLOCK LOT (BBL)") == geo.at("bbl"), "Raw/typed BBL consistency");
    }
    db.Assert("(SELECT count(*) FROM gs_runtime_info()) = 2");
    db.Exec("SET VARIABLE geosupport_release='26b'");
    db.Exec("PREPARE pinned AS SELECT (gs_address('125','Worth St','Mn')).release");
    db.Exec("SET VARIABLE geosupport_release='26c'");
    Check(db.Value("EXECUTE pinned") == "26b", "Prepared release pin");
    db.Assert("(gs_address('125','Worth St','Mn')).release = '26c'");
    duckdb_connection second; duckdb_connect(db.db, &second);
    duckdb_result failed{};
    Check(duckdb_query(second, "SELECT gs_address('125','Worth St','Mn')", &failed) == DuckDBError, "Connection default leaked");
    Check(std::string(duckdb_result_error(&failed)).find("Select a release") != std::string::npos, "Wrong second connection failure");
    duckdb_destroy_result(&failed); duckdb_disconnect(&second);
    db.Exec("CREATE TEMP TABLE geocoded AS SELECT i, gs_address('125','Worth St','Mn',release := CASE WHEN i%2=0 THEN '26b' ELSE '26c' END) AS geo FROM range(5000) r(i)");
    db.Assert("(SELECT count(*)=5000 AND count(DISTINCT geo.bundle_id)=2 AND bool_and(geo.bbl='1001680032') FROM geocoded)");
    db.Assert("gs_call(NULL,'{}',release := '26c') IS NULL");
    db.Assert("gs_call('BL',NULL,release := '26c') IS NULL");
    db.Assert("gs_call('BL','{}',release := '26c', mode := NULL) IS NULL");
    db.Assert("(gs_address(NULL,'CITY HALL','Mn',release := '26c')).raw IS NOT NULL");
    db.Assert("(gs_address('999999','NOT A REAL STREET','Mn',release := '26c')).grc NOT IN ('00','01')");
    db.Assert("length((gs_address('999999','NOT A REAL STREET','Mn',release := '26c')).message)>0");
    db.Assert("(gs_bbl('1001680032',release := '26c')).bbl = '1001680032'");
    db.Assert("(gs_bin((" + Address + ").bin,release := '26c')).bbl = '1001680032'");
    for (const auto &mode : {"regular", "extended", "long", "tpad", "long+tpad", "extended+tpad"})
        db.Assert("(gs_bbl('1001680032',release := '26c',mode := " + Quote(mode) + ")).bbl = '1001680032'");
    auto fixture_bytes = Read("tests/fixtures/requests.json");
    auto requests = json::parse(fixture_bytes);
    auto provenance = json::parse(Read("tests/fixtures/expected-provenance.json"));
    Check(picosha2::hash256_hex_string(fixture_bytes) == provenance.at("requests_sha256"), "Reference request hash");
    for (const std::string release : {"26b", "26c"}) {
        auto golden_bytes = Read("tests/fixtures/expected-" + release + ".json");
        auto golden = json::parse(golden_bytes);
        const auto &reference = provenance.at("releases").at(release);
        Check(picosha2::hash256_hex_string(golden_bytes) == reference.at("output_sha256"), "Reference output hash for " + release);
        Check(db.Value("SELECT bundle_id FROM gs_load(" + Quote(release) + ")") == reference.at("bundle_id"), "Reference bundle identity for " + release);
        Check(golden.is_array() && golden.size() == requests.size(), "Golden fixture row count for " + release);
        for (size_t i = 0; i < requests.size(); i++) {
            const auto &request = requests[i];
            auto sql = "gs_call(" + Quote(request.at("function")) + "," + Quote(request.at("params").dump()) + ",release := " + Quote(release) + ",mode := " + Quote(request.at("mode")) + ")";
            auto value = json::parse(db.Value("SELECT to_json(" + sql + ")"));
            auto raw = json::parse(value.at("raw").get<std::string>());
            Check(value.at("release") == release && raw == golden[i],
                "Full-output golden mismatch " + release + " request " + std::to_string(i) + ": " + json::diff(golden[i], raw).dump().substr(0, 4000));
        }
    }
    db.Fails("SELECT gs_call('1B','{\"unknown\":\"x\"}',release := '26c')", "Unknown input");
    db.Fails("SELECT gs_call('1B','{\"house_number\":\"111111111111111111111111111111111111\"}',release := '26c')", "field width");
    db.Fails("SELECT gs_call('bogus','{}',release := '26c')", "function");
    db.Fails("SELECT gs_call('1B','{}',release := '26c',mode := 'bogus')", "mode");
    db.Fails("SELECT gs_address('125','Wörth St','Mn',release := '26c')", "ASCII");
    db.Assert("(" + Address + ").bbl='1001680032'");
    db.Exec("CALL gs_unload('26b'); CALL gs_unload('26c')");
    db.Assert("(SELECT count(*) FROM gs_runtime_info())=0");
    db.Assert("(SELECT count(*) FROM geocoded)=5000");
    db.Assert("(" + Address + ").bbl='1001680032'");
    // Parallel connections on one database stress the shared manager's serialization.
    std::vector<std::future<void>> jobs;
    for (int i = 0; i < 4; i++) jobs.emplace_back(std::async(std::launch::async, [&] {
        duckdb_connection connection; duckdb_connect(db.db, &connection);
        try {
            Result result(connection, "SELECT count(*) FROM (SELECT gs_address(i::VARCHAR,'Broadway','Mn',release := '26c') AS geo FROM range(1,51) r(i)) WHERE geo.raw IS NOT NULL");
            if (result.Value() != "50") throw std::runtime_error("Concurrent row count");
        } catch (...) { duckdb_disconnect(&connection); throw; }
        duckdb_disconnect(&connection);
    }));
    for (auto &job : jobs) { job.get(); ++checks; }
}
double Seconds(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }
void Benchmark(const std::string &extension, const std::string &output) {
    DB db(extension);
    auto start = Clock::now();
    auto id = db.Value("SELECT bundle_id FROM gs_load('26c')");
    double startup = Seconds(start);
    db.Exec("SET threads=1");
    db.Exec("CREATE TEMP TABLE requests AS SELECT i::VARCHAR AS house FROM range(1,2001) r(i)");
    const std::string sql = "SELECT sum(length((gs_address(house,'Broadway','Mn',release := '26c')).raw)) FROM requests";
    std::vector<double> cold, warm;
    std::string checksum;
    // Three independent cold caches, each followed by a measured warm pass.
    for (int i = 0; i < 3; i++) {
        db.Exec("CALL gs_unload('26c'); CALL gs_load('26c')");
        start = Clock::now(); auto value = db.Value(sql); cold.push_back(Seconds(start));
        Check(value != "<NULL>" && std::stoull(value)>0, "Empty benchmark result");
        if (checksum.empty()) checksum = value; else Check(checksum == value, "Cold benchmark checksum changed");
        start = Clock::now(); value = db.Value(sql); warm.push_back(Seconds(start));
        Check(value == checksum, "Warm benchmark checksum changed");
    }
    // Column input prevents constant folding the whole call at bind time.
    db.Exec("CREATE TEMP TABLE repeated AS SELECT '125'::VARCHAR AS house FROM range(10000)");
    start = Clock::now();
    db.Exec("CREATE TEMP TABLE repeated_results AS SELECT gs_address(house,'Worth St','Mn',release := '26c') AS geo FROM repeated");
    double repeated = Seconds(start);
    db.Assert("(SELECT count(*)=10000 AND bool_and(geo.bbl='1001680032') FROM repeated_results)");
    auto median = [](std::vector<double> values) { std::sort(values.begin(), values.end()); return values.at(values.size()/2); };
    utsname host{}; uname(&host);
    json report = {{"duckdb", duckdb_library_version()}, {"host", std::string(host.sysname)+" "+host.release+" "+host.machine},
        {"runtime", "local-native"}, {"threads", 1}, {"bundle_id", id}, {"rows", 2000},
        {"load_verify_startup_seconds", startup}, {"cold_seconds", cold}, {"warm_seconds", warm},
        {"cold_median_rows_per_second", 2000/median(cold)}, {"warm_median_rows_per_second", 2000/median(warm)},
        {"repeated_materialization_rows", 10000}, {"repeated_materialization_seconds", repeated},
        {"output_bytes", std::stoull(checksum)},
        {"limitations", "Single-threaded SQL; 2000 distinct house numbers on Broadway including rejects. Warm runs measure the worker result cache; repeated rows measure deduplication and materialization. Not a citywide capacity estimate. No timing threshold on shared CI hosts."}};
    std::ofstream out(output); out << report.dump(2) << '\n'; out.close();
    Check(bool(out), "Cannot save benchmark"); std::cout << report.dump(2) << '\n';
}
int main(int argc, char **argv) {
    try {
        if (argc < 3) throw std::runtime_error("Usage: sql-tests unit|e2e|benchmark EXTENSION [REPORT]");
        const std::string mode = argv[1];
        if (mode == "unit") Unit(argv[2]);
        else if (mode == "e2e") E2E(argv[2]);
        else if (mode == "benchmark" && argc == 4) Benchmark(argv[2], argv[3]);
        else throw std::runtime_error("Invalid arguments");
        std::cout << "PASS native SQL " << mode << ": " << checks << " checks\n";
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
