#include "duckdb_extension.h"
#include "nlohmann/json.hpp"
#include "native.hpp"
#include <memory>
#include <stdexcept>
#include <string>

using json = nlohmann::json;
DUCKDB_EXTENSION_EXTERN

namespace {
using Runtime = gs::Manager;

struct FunctionData { std::shared_ptr<Runtime> runtime; std::string operation; };
struct BoundData { json request; };
struct ScanData { json rows; size_t offset = 0; bool requested = false; };
template<class T> void Delete(void *p) { delete static_cast<T *>(p); }

void String(duckdb_vector vector, idx_t row, const std::string &value) {
    duckdb_vector_assign_string_element_len(vector, row, value.data(), value.size());
}
void Null(duckdb_vector vector, idx_t row) {
    duckdb_vector_ensure_validity_writable(vector);
    duckdb_validity_set_row_invalid(duckdb_vector_get_validity(vector), row);
}
json Input(duckdb_vector vector, idx_t row) {
    auto validity = duckdb_vector_get_validity(vector);
    if (validity && !duckdb_validity_row_is_valid(validity, row)) return nullptr;
    auto strings = static_cast<duckdb_string_t *>(duckdb_vector_get_data(vector));
    return std::string(duckdb_string_t_data(&strings[row]), duckdb_string_t_length(strings[row]));
}

const char *FIELDS[] = {"bbl", "bin", "latitude", "longitude", "grc", "grc2", "message", "message2", "release", "engine_version", "bundle_id", "raw"};
constexpr idx_t FIELD_COUNT = sizeof(FIELDS) / sizeof(FIELDS[0]);

void Geocode(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    try {
        auto extra = static_cast<FunctionData *>(duckdb_scalar_function_get_extra_info(info));
        json rows = json::array();
        idx_t count = duckdb_data_chunk_get_size(input);
        for (idx_t row = 0; row < count; row++) {
            auto function = Input(duckdb_data_chunk_get_vector(input, 0), row);
            auto params = Input(duckdb_data_chunk_get_vector(input, 1), row);
            auto release = Input(duckdb_data_chunk_get_vector(input, 2), row);
            auto mode = Input(duckdb_data_chunk_get_vector(input, 3), row);
            if (release.is_null()) throw std::runtime_error("Select a release: SET VARIABLE geosupport_release = '26c', or pass release := '26c'");
            if (function.is_null() || params.is_null() || mode.is_null()) { rows.push_back(nullptr); continue; }
            rows.push_back({{"function", function}, {"params", json::parse(params.get<std::string>())}, {"release", release}, {"mode", mode}});
        }
        json results = extra->runtime->Request({{"op", "batch"}, {"rows", rows}});
        if (!results.is_array() || results.size() != count) throw std::runtime_error("Runtime row count mismatch");
        for (idx_t row = 0; row < count; row++) {
            if (results[row].is_null()) { Null(output, row); continue; }
            for (idx_t col = 0; col < FIELD_COUNT; col++) {
                auto vector = duckdb_struct_vector_get_child(output, col);
                const auto &value = results[row].at(FIELDS[col]);
                if (value.is_null()) Null(vector, row);
                else if (col == 2 || col == 3) static_cast<double *>(duckdb_vector_get_data(vector))[row] = value.get<double>();
                else String(vector, row, value.get<std::string>());
            }
        }
    } catch (const std::exception &error) { duckdb_scalar_function_set_error(info, error.what()); }
}

void Bind(duckdb_bind_info info) {
    try {
        auto extra = static_cast<FunctionData *>(duckdb_bind_get_extra_info(info));
        auto bound = std::make_unique<BoundData>();
        json args = json::array();
        for (idx_t i = 0; i < duckdb_bind_get_parameter_count(info); i++) {
            auto value = duckdb_bind_get_parameter(info, i);
            bool is_null = duckdb_is_null_value(value);
            char *str = is_null ? nullptr : duckdb_get_varchar(value);
            std::string arg = str ? str : "";
            if (str) duckdb_free(str);
            duckdb_destroy_value(&value);
            if (is_null) throw std::runtime_error("Lifecycle arguments cannot be NULL");
            args.push_back(arg);
        }
        bound->request = {{"op", extra->operation}, {"args", args}};
        auto type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
        for (auto name : {"release", "bundle_id", "engine_version", "platform", "details"}) duckdb_bind_add_result_column(info, name, type);
        duckdb_destroy_logical_type(&type);
        duckdb_bind_set_bind_data(info, bound.release(), Delete<BoundData>);
        // Do not start processes or download anything at bind/EXPLAIN time.
    } catch (const std::exception &error) { duckdb_bind_set_error(info, error.what()); }
}
void Init(duckdb_init_info info) {
    duckdb_init_set_init_data(info, new ScanData(), Delete<ScanData>);
    duckdb_init_set_max_threads(info, 1);
}
void Scan(duckdb_function_info info, duckdb_data_chunk output) {
    try {
        auto extra = static_cast<FunctionData *>(duckdb_function_get_extra_info(info));
        auto bound = static_cast<BoundData *>(duckdb_function_get_bind_data(info));
        auto state = static_cast<ScanData *>(duckdb_function_get_init_data(info));
        if (!state->requested) {
            state->rows = extra->runtime->Request(bound->request);
            state->requested = true;
        }
        idx_t count = 0;
        const char *names[] = {"release", "bundle_id", "engine_version", "platform"};
        while (state->offset < state->rows.size() && count < duckdb_vector_size()) {
            const auto &row = state->rows[state->offset++];
            for (idx_t col = 0; col < 4; col++) {
                auto vector = duckdb_data_chunk_get_vector(output, col);
                if (!row.contains(names[col]) || row[names[col]].is_null()) Null(vector, count);
                else String(vector, count, row[names[col]].get<std::string>());
            }
            String(duckdb_data_chunk_get_vector(output, 4), count, row.dump());
            count++;
        }
        duckdb_data_chunk_set_size(output, count);
    } catch (const std::exception &error) { duckdb_function_set_error(info, error.what()); }
}

void RegisterTable(duckdb_connection connection, std::shared_ptr<Runtime> runtime, const char *name, const char *op, int argc) {
    auto function = duckdb_create_table_function();
    duckdb_table_function_set_name(function, name);
    auto type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    for (int i = 0; i < argc; i++) duckdb_table_function_add_parameter(function, type);
    duckdb_destroy_logical_type(&type);
    duckdb_table_function_set_extra_info(function, new FunctionData{runtime, op}, Delete<FunctionData>);
    duckdb_table_function_set_bind(function, Bind);
    duckdb_table_function_set_init(function, Init);
    duckdb_table_function_set_function(function, Scan);
    auto result = duckdb_register_table_function(connection, function);
    duckdb_destroy_table_function(&function);
    if (result != DuckDBSuccess) throw std::runtime_error(std::string("Cannot register ") + name);
}

void Register(duckdb_connection connection) {
    auto runtime = std::make_shared<Runtime>();
    RegisterTable(connection, runtime, "gs_available_releases", "available", 0);
    RegisterTable(connection, runtime, "gs_installed_releases", "installed", 0);
    RegisterTable(connection, runtime, "gs_runtime_info", "runtime_info", 0);
    RegisterTable(connection, runtime, "gs_install", "install", 1);
    RegisterTable(connection, runtime, "gs_import", "import", 2);
    RegisterTable(connection, runtime, "gs_verify", "verify", 1);
    RegisterTable(connection, runtime, "gs_load", "load", 1);
    RegisterTable(connection, runtime, "gs_unload", "unload", 1);
    auto function = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(function, "_gs_call");
    auto varchar = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    for (int i = 0; i < 4; i++) duckdb_scalar_function_add_parameter(function, varchar);
    duckdb_destroy_logical_type(&varchar);
    duckdb_logical_type types[FIELD_COUNT];
    for (idx_t i = 0; i < FIELD_COUNT; i++) types[i] = duckdb_create_logical_type(i == 2 || i == 3 ? DUCKDB_TYPE_DOUBLE : DUCKDB_TYPE_VARCHAR);
    auto structure = duckdb_create_struct_type(types, FIELDS, FIELD_COUNT);
    for (auto &type : types) duckdb_destroy_logical_type(&type);
    duckdb_scalar_function_set_return_type(function, structure);
    duckdb_destroy_logical_type(&structure);
    duckdb_scalar_function_set_extra_info(function, new FunctionData{runtime, "batch"}, Delete<FunctionData>);
    duckdb_scalar_function_set_special_handling(function);
    duckdb_scalar_function_set_volatile(function);
    duckdb_scalar_function_set_function(function, Geocode);
    auto status = duckdb_register_scalar_function(connection, function);
    duckdb_destroy_scalar_function(&function);
    if (status != DuckDBSuccess) throw std::runtime_error("Cannot register _gs_call");
    const char *sql = R"SQL(
        CREATE MACRO gs_call(function_code, inputs, release := NULL, mode := 'regular') AS
            _gs_call(CAST(function_code AS VARCHAR), CAST(inputs AS VARCHAR), CAST(coalesce(release, getvariable('geosupport_release')) AS VARCHAR), CAST(mode AS VARCHAR));
        CREATE MACRO gs_address(house_number, street_name, borough, release := NULL) AS
            gs_call('1B', to_json(struct_pack(house_number := house_number, street_name := street_name, borough_code := borough)), release := release);
        CREATE MACRO gs_bbl(bbl, release := NULL, mode := 'regular') AS
            gs_call('BL', to_json(struct_pack(bbl := bbl)), release := release, mode := mode);
        CREATE MACRO gs_bin(bin, release := NULL, mode := 'regular') AS
            gs_call('BN', to_json(struct_pack(bin := bin)), release := release, mode := mode);
    )SQL";
    duckdb_result result;
    status = duckdb_query(connection, sql, &result);
    std::string error = status == DuckDBSuccess ? "" : duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    if (status != DuckDBSuccess) throw std::runtime_error(error);
}
} // namespace

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_connection connection, duckdb_extension_info info, struct duckdb_extension_access *access) {
    try { Register(connection); return true; }
    catch (const std::exception &error) { access->set_error(info, error.what()); return false; }
}
