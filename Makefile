# Production build and native validation: C/C++, Make, shell, and DuckDB only.
.DELETE_ON_ERROR:
BUILD ?= build/native
DEPS ?= $(abspath build/deps)
DUCKDB ?= $(DEPS)/duckdb
EXTENSION_VERSION := 0.2.0
CXXFLAGS ?= -O2
CFLAGS ?= -O2
CPPFLAGS += -Ivendor/duckdb -Ivendor -Isrc -I$(BUILD)/generated
NATIVE_FLAGS = -std=c++17 -Wall -Wextra -Werror -Wno-missing-field-initializers -fPIC -fvisibility=hidden
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SHARED = -dynamiclib
PLATFORM_LIBS =
else
SHARED = -shared
PLATFORM_LIBS = -ldl -pthread
CRYPTO_LIBS = -lcrypto
endif
ZIP_OBJECTS = $(addprefix $(BUILD)/,miniz.o miniz_zip.o miniz_tinfl.o)
STORE_OBJECTS = $(BUILD)/common.o $(BUILD)/store.o $(ZIP_OBJECTS)

.PHONY: all build worker fetch test integration benchmark package clean
all: build
build: $(BUILD)/geosupport.duckdb_extension $(BUILD)/geosupport-store
worker: $(BUILD)/geosupport-worker
fetch:
	bash scripts/fetch-duckdb.sh $(DEPS)

$(BUILD) $(BUILD)/generated:
	mkdir -p $@
$(BUILD)/native-build-tool: scripts/native-build-tool.cpp vendor/nlohmann/json.hpp vendor/picosha2/picosha2.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(NATIVE_FLAGS) -Ivendor $< -o $@
$(BUILD)/generated/resources.hpp: resources/layouts.json $(BUILD)/native-build-tool | $(BUILD)/generated
	$(BUILD)/native-build-tool json $< $@ LAYOUTS_JSON
$(BUILD)/generated/catalog.hpp: catalog/releases.json $(BUILD)/native-build-tool | $(BUILD)/generated
	$(BUILD)/native-build-tool json $< $@ CATALOG_JSON
$(BUILD)/generated/worker.hpp: $(BUILD)/geosupport-worker $(BUILD)/native-build-tool | $(BUILD)/generated
	$(BUILD)/native-build-tool worker $< $@
$(BUILD)/%.o: src/%.cpp src/native.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(NATIVE_FLAGS) -DDUCKDB_EXTENSION_NAME=geosupport -c $< -o $@
$(BUILD)/engine.o: $(BUILD)/generated/resources.hpp
$(BUILD)/store.o: $(BUILD)/generated/catalog.hpp
$(BUILD)/manager.o: $(BUILD)/generated/worker.hpp
$(ZIP_OBJECTS): $(BUILD)/%.o: vendor/miniz/%.c | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -DMINIZ_NO_DEFLATE_APIS -DMINIZ_NO_ZLIB_APIS -c $< -o $@
$(BUILD)/common-worker.o: src/common.cpp src/native.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(NATIVE_FLAGS) -DGS_WORKER_ONLY -c $< -o $@
$(BUILD)/geosupport-worker: $(BUILD)/common-worker.o $(BUILD)/engine.o $(BUILD)/worker_main.o
	$(CXX) $^ $(PLATFORM_LIBS) -o $@
$(BUILD)/geosupport-store: $(STORE_OBJECTS) $(BUILD)/store_main.o
	$(CXX) $^ -lcurl $(CRYPTO_LIBS) -o $@
$(BUILD)/geosupport.so: $(BUILD)/geosupport.o $(BUILD)/manager.o $(STORE_OBJECTS)
	$(CXX) $(SHARED) $^ -lcurl $(PLATFORM_LIBS) $(CRYPTO_LIBS) -o $@
$(BUILD)/geosupport.duckdb_extension: $(BUILD)/geosupport.so $(BUILD)/native-build-tool
	$(BUILD)/native-build-tool metadata $< $@ "$$($(DUCKDB) -no-stdin -csv -noheader -c 'PRAGMA platform;')" $(EXTENSION_VERSION)
$(BUILD)/store-tests: tests/native/store.cpp $(STORE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(NATIVE_FLAGS) $^ -lcurl $(PLATFORM_LIBS) $(CRYPTO_LIBS) -o $@
$(BUILD)/sql-tests: tests/native/sql.cpp vendor/duckdb/duckdb.h vendor/nlohmann/json.hpp vendor/picosha2/picosha2.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(NATIVE_FLAGS) $< -L$(DEPS) -Wl,-rpath,$(DEPS) -lduckdb -pthread -o $@
test: build $(BUILD)/store-tests $(BUILD)/sql-tests
	$(BUILD)/store-tests
	$(BUILD)/sql-tests unit $(abspath $(BUILD)/geosupport.duckdb_extension)
integration: build $(BUILD)/sql-tests
	$(BUILD)/sql-tests e2e $(abspath $(BUILD)/geosupport.duckdb_extension)
benchmark: build $(BUILD)/sql-tests
	$(BUILD)/sql-tests benchmark $(abspath $(BUILD)/geosupport.duckdb_extension) $(abspath $(BUILD)/benchmark.json)
package: build
	tar -czf $(BUILD)/geosupport-$(EXTENSION_VERSION)-$$(uname -s)-$$(uname -m).tar.gz -C $(BUILD) geosupport.duckdb_extension -C $(CURDIR) README.md LICENSE THIRD_PARTY_NOTICES.md docs vendor/duckdb/LICENSE vendor/nlohmann/LICENSE vendor/miniz/LICENSE vendor/picosha2/LICENSE resources/PYTHON_GEOSUPPORT_LICENSE
clean:
	rm -rf $(BUILD)
