#include "DbEngine.h"
#include "DbValues.h"
#include "NativeConnections.h"
#include "SqlProvider.h"
#include <winsqlite/winsqlite3.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <windows.h>
#include <climits>
#include <charconv>

#pragma comment(lib, "winsqlite3.lib")

namespace native_app {

namespace fs = std::filesystem;
using json = nlohmann::json;

static std::string QuoteIdentifier(const std::string& str) {
    std::string q = "\"";
    for (char c : str) {
        if (c == '"') q += "\"\"";
        else q += c;
    }
    q += "\"";
    return q;
}

DbEngine::DbEngine(const std::string& dataDir, bool seedSamples) : dataDir_(fs::absolute(fs::u8path(dataDir)).u8string()) {
    fs::create_directories(fs::u8path(dataDir_));
    if (seedSamples) {
        InitSamples();
    }
    for (const auto& entry : fs::directory_iterator(fs::u8path(dataDir_))) {
        if (entry.is_regular_file() && entry.path().extension() == ".db") {
            const std::string id = entry.path().stem().u8string();
            if (!databases_.count(id)) {
                DatabaseInfo info;
                info.id = id; info.name = id; info.path = entry.path().u8string();
                databases_[id] = info;
            }
        }
    }
    const auto catalogPath = fs::u8path(dataDir_) / ".databases.json";
    if (fs::exists(catalogPath)) {
        try {
            std::ifstream catalogFile(catalogPath);
            const auto catalog = json::parse(catalogFile);
            if (!catalog.is_array()) throw std::runtime_error("Invalid catalog");
            for (const auto& record : catalog) {
                DatabaseInfo info;
                info.id = record.at("id").get<std::string>(); info.name = record.at("name").get<std::string>();
                info.path = record.at("path").get<std::string>(); info.isSample = record.value("isSample", false);
                if(record.value("detached",false)){databases_.erase(info.id);detachedFiles_[info.id]=info;continue;}
                databases_[info.id] = info;
                if (record.value("attached", false)) attachedIds_.insert(info.id);
            }
        } catch (...) { throw std::runtime_error("Cannot read the database catalog. Restore a valid backup; the existing file was preserved."); }
    }
    connections_ = std::make_unique<NativeConnections>(dataDir_);
}
DbEngine::~DbEngine() = default;
std::vector<DatabaseDriverInfo> DbEngine::ListDrivers() { return connections_->Drivers(); }
ConnectionProfile DbEngine::GetConnectionProfile(const std::string& id) { if(connections_->Has(id))return connections_->Profile(id);std::lock_guard<std::recursive_mutex> lock(mutex_);const auto found=databases_.find(id);if(found==databases_.end())throw std::runtime_error("Connection not found.");ConnectionProfile profile;profile.id=id;profile.name=found->second.name;profile.path=found->second.path;return profile; }
std::string DbEngine::TestConnection(const ConnectionProfile& p, const std::atomic_bool* cancellation) { return connections_->Test(p,cancellation); }
DatabaseInfo DbEngine::SaveConnection(const ConnectionProfile& p, bool connect, const std::atomic_bool* cancellation) { return connections_->Save(p,connect,cancellation); }
bool DbEngine::DisconnectDatabase(const std::string& id) { if(!connections_->Has(id)){auto profile=GetConnectionProfile(id);connections_->Save(profile,false,nullptr);}return connections_->Disconnect(id); }
bool DbEngine::RemoveConnection(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);const auto local=databases_.find(id);const bool profile=connections_->Has(id);if(local==databases_.end()&&!profile)return false;
    auto previous=databases_;auto previousDetached=detachedFiles_;auto previousAttached=attachedIds_;
    if(local!=databases_.end()){detachedFiles_[id]=local->second;databases_.erase(local);attachedIds_.erase(id);}else if(profile){const auto value=connections_->Profile(id);if(value.type=="sqlite"&&fs::u8path(value.path).parent_path()==fs::u8path(dataDir_)){DatabaseInfo info;info.id=fs::u8path(value.path).stem().u8string();info.name=value.name;info.path=value.path;detachedFiles_[info.id]=info;databases_.erase(info.id);}}
    try{SaveCatalog();if(profile)connections_->Remove(id);}catch(...){databases_=std::move(previous);detachedFiles_=std::move(previousDetached);attachedIds_=std::move(previousAttached);try{SaveCatalog();}catch(...){}throw;}return true;
}
bool DbEngine::IsReadOnly(const std::string& id) { return connections_->Has(id) && connections_->Profile(id).readOnly; }

void DbEngine::SaveCatalog() {
    json records = json::array();
    for (const auto& pair : databases_) records.push_back({{"id", pair.second.id}, {"name", pair.second.name}, {"path", pair.second.path},
        {"isSample", pair.second.isSample}, {"attached", attachedIds_.count(pair.first) != 0}});
    for (const auto& pair : detachedFiles_) records.push_back({{"id",pair.first},{"name",pair.second.name},{"path",pair.second.path},{"detached",true}});
    const auto target = fs::u8path(dataDir_) / ".databases.json";
    const auto pending = fs::path(target.wstring() + L".pending");
    std::ofstream file(pending, std::ios::binary);
    file << records.dump(2); file.flush();
    const bool valid = static_cast<bool>(file); file.close();
    if (!valid || !file || !MoveFileExW(pending.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code error; fs::remove(pending, error);
        throw std::runtime_error("Cannot save database profiles. Check folder access and free disk space.");
    }
}

std::string DbEngine::GetDbPath(const std::string& dbId) {
    if (connections_->Has(dbId)) { const auto p=connections_->Profile(dbId); if(p.type!="sqlite"||!fs::is_regular_file(fs::u8path(p.path))) throw std::runtime_error("Database file is unavailable. Reconnect its file."); return p.path; }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = databases_.find(dbId);
    if (it == databases_.end() || !fs::is_regular_file(fs::u8path(it->second.path)))
        throw std::runtime_error("Database is unavailable. Select an existing database or reconnect its file.");
    return it->second.path;
}

static int OpenDatabase(const std::string& path, sqlite3** db, bool readOnly = false) {
    const int rc = sqlite3_open_v2(path.c_str(), db, readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_busy_timeout(*db, 3000);
        sqlite3_exec(*db, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
    }
    return rc;
}

static bool Cancelled(const std::atomic_bool* cancellation) { return cancellation && cancellation->load(); }
static void CheckCancellation(const std::atomic_bool* cancellation) {
    if (Cancelled(cancellation)) throw std::runtime_error("Query cancelled.");
}
static std::unique_lock<std::recursive_mutex> MutationLock(std::recursive_mutex& mutex,const std::atomic_bool* cancellation){std::unique_lock<std::recursive_mutex> lock(mutex,std::defer_lock);while(!lock.try_lock()){CheckCancellation(cancellation);Sleep(5);}CheckCancellation(cancellation);return lock;}
static void ConfigureCancellation(sqlite3* db, const std::atomic_bool* cancellation) {
    if (!cancellation) return;
    CheckCancellation(cancellation);
    sqlite3_progress_handler(db, 1000, [](void* context) {
        return static_cast<const std::atomic_bool*>(context)->load() ? 1 : 0;
    }, const_cast<std::atomic_bool*>(cancellation));
    // Preserve a finite lock timeout while allowing cancellation during a wait.
    sqlite3_busy_handler(db, [](void* context, int attempts) {
        if (static_cast<const std::atomic_bool*>(context)->load() || attempts >= 300) return 0;
        Sleep(10); return 1;
    }, const_cast<std::atomic_bool*>(cancellation));
}
static void CheckSqlite(sqlite3* db, int result, const std::atomic_bool* cancellation) {
    CheckCancellation(cancellation);
    if (result != SQLITE_OK && result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
}
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
static Statement Prepare(sqlite3* db, const std::string& sql, const std::atomic_bool* cancellation) {
    CheckCancellation(cancellation);
    sqlite3_stmt* raw = nullptr;
    const int result = sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr);
    Statement statement(raw, &sqlite3_finalize);
    CheckSqlite(db, result, cancellation);
    return statement;
}
static std::string SearchPattern(const std::string& value) {
    std::string escaped = "%";
    for (char c : value) { if (c == '\\' || c == '%' || c == '_') escaped += '\\'; escaped += c; }
    return escaped + "%";
}

static DbValue SqliteValue(sqlite3_stmt* statement, int column) {
    const int type = sqlite3_column_type(statement, column);
    if (type == SQLITE_NULL) return {DbValueType::Null, ""};
    if (type == SQLITE_INTEGER) return {DbValueType::Integer, std::to_string(sqlite3_column_int64(statement, column))};
    if (type == SQLITE_FLOAT) {
        char buffer[64];
        const auto formatted = std::to_chars(buffer, buffer + sizeof(buffer), sqlite3_column_double(statement, column));
        if (formatted.ec != std::errc()) throw std::runtime_error("Cannot format SQLite numeric value.");
        std::string text(buffer, formatted.ptr);
        if (text == "-0") text = "-0.0"; // Keep the sign when the typed value is parsed as JSON.
        return {DbValueType::Real, text};
    }
    if (type == SQLITE_BLOB) {
        const auto* value = static_cast<const char*>(sqlite3_column_blob(statement, column));
        return {DbValueType::Blob, value ? std::string(value, sqlite3_column_bytes(statement, column)) : ""};
    }
    const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
    return {DbValueType::Text, value ? std::string(value, sqlite3_column_bytes(statement, column)) : ""};
}

void DbEngine::InitSamples() {
    std::string ecomPath = (fs::u8path(dataDir_) / "ecommerce.db").u8string();
    bool ecomExists = fs::exists(ecomPath);
    DatabaseInfo ecom;
    ecom.id = "ecommerce";
    ecom.name = "E-Commerce Store (Sample)";
    ecom.path = ecomPath;
    ecom.isSample = true;
    databases_["ecommerce"] = ecom;
    if (!ecomExists) SeedEcommerce(ecomPath);

    std::string devPath = (fs::u8path(dataDir_) / "dev_studio.db").u8string();
    bool devExists = fs::exists(devPath);
    DatabaseInfo dev;
    dev.id = "dev_studio";
    dev.name = "Dev & API Metrics (Sample)";
    dev.path = devPath;
    dev.isSample = true;
    databases_["dev_studio"] = dev;
    if (!devExists) SeedDevStudio(devPath);

    // Discover any other .db files
    for (const auto& entry : fs::directory_iterator(fs::u8path(dataDir_))) {
        if (entry.is_regular_file() && entry.path().extension() == ".db") {
            std::string stem = entry.path().stem().u8string();
            if (databases_.find(stem) == databases_.end()) {
                DatabaseInfo info;
                info.id = stem;
                std::string title = stem;
                std::replace(title.begin(), title.end(), '_', ' ');
                if (!title.empty()) title[0] = static_cast<char>(toupper(static_cast<unsigned char>(title[0])));
                info.name = title;
                info.path = entry.path().u8string();
                info.isSample = false;
                databases_[stem] = info;
            }
        }
    }
}

void DbEngine::SeedEcommerce(const std::string& path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) { if (db) sqlite3_close(db); throw std::runtime_error("Cannot initialize sample database."); }

    const char* ddl = R"SQL(
    CREATE TABLE IF NOT EXISTS categories (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL UNIQUE,
        slug TEXT NOT NULL,
        description TEXT,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    );

    CREATE TABLE IF NOT EXISTS products (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        category_id INTEGER NOT NULL,
        name TEXT NOT NULL,
        sku TEXT UNIQUE NOT NULL,
        price REAL NOT NULL CHECK(price >= 0),
        stock INTEGER NOT NULL DEFAULT 0,
        status TEXT NOT NULL DEFAULT 'active',
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        FOREIGN KEY (category_id) REFERENCES categories(id) ON DELETE CASCADE
    );

    CREATE TABLE IF NOT EXISTS customers (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        first_name TEXT NOT NULL,
        last_name TEXT NOT NULL,
        email TEXT UNIQUE NOT NULL,
        city TEXT,
        country TEXT,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    );

    CREATE TABLE IF NOT EXISTS orders (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        customer_id INTEGER NOT NULL,
        order_number TEXT UNIQUE NOT NULL,
        total_amount REAL NOT NULL CHECK(total_amount >= 0),
        order_status TEXT DEFAULT 'pending',
        payment_method TEXT,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        FOREIGN KEY (customer_id) REFERENCES customers(id) ON DELETE CASCADE
    );

    CREATE TABLE IF NOT EXISTS order_items (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        order_id INTEGER NOT NULL,
        product_id INTEGER NOT NULL,
        quantity INTEGER NOT NULL CHECK(quantity > 0),
        unit_price REAL NOT NULL,
        total_price REAL NOT NULL,
        FOREIGN KEY (order_id) REFERENCES orders(id) ON DELETE CASCADE,
        FOREIGN KEY (product_id) REFERENCES products(id) ON DELETE CASCADE
    );

    CREATE TABLE IF NOT EXISTS reviews (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        product_id INTEGER NOT NULL,
        customer_id INTEGER NOT NULL,
        rating INTEGER NOT NULL CHECK(rating BETWEEN 1 AND 5),
        comment TEXT,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        FOREIGN KEY (product_id) REFERENCES products(id) ON DELETE CASCADE,
        FOREIGN KEY (customer_id) REFERENCES customers(id) ON DELETE CASCADE
    );

    CREATE INDEX IF NOT EXISTS idx_products_category ON products(category_id);
    CREATE INDEX IF NOT EXISTS idx_orders_customer ON orders(customer_id);
    CREATE INDEX IF NOT EXISTS idx_order_items_order ON order_items(order_id);
    CREATE INDEX IF NOT EXISTS idx_reviews_product ON reviews(product_id);

    INSERT INTO categories (name, slug, description) VALUES
    ('Electronics', 'electronics', 'Smartphones, laptops, monitors, accessories'),
    ('Audio & Sound', 'audio-sound', 'Headphones, speakers, studio microphones'),
    ('Office & Desk', 'office-desk', 'Ergonomic chairs, desks, lighting, cable management'),
    ('Wearables', 'wearables', 'Smartwatches, fitness bands, trackers');

    INSERT INTO products (category_id, name, sku, price, stock, status) VALUES
    (1, 'UltraBook Pro 16"', 'ELEC-UB16-PRO', 1899.99, 45, 'active'),
    (1, '4K Ultra-Wide Monitor 34"', 'ELEC-MN34-4K', 649.50, 30, 'active'),
    (1, 'Mechanical Keyboard RGB', 'ELEC-KB-RGB', 129.99, 110, 'active'),
    (2, 'Noise-Cancelling Headphones Pro', 'AUD-NC-001', 299.00, 75, 'active'),
    (2, 'Wireless Desktop Soundbar', 'AUD-SB-DESK', 89.95, 60, 'active'),
    (2, 'USB-C Studio Microphone', 'AUD-MIC-USBC', 149.00, 40, 'active'),
    (3, 'Ergonomic Mesh Chair V2', 'OFF-CHR-MESH', 420.00, 25, 'active'),
    (3, 'Motorized Standing Desk 60x30', 'OFF-DSK-MOT60', 580.00, 18, 'active'),
    (4, 'Apex Watch Series 8', 'WEAR-APX-S8', 349.99, 55, 'active'),
    (4, 'Fitness Tracker Band Pro', 'WEAR-TRK-PRO', 79.99, 120, 'active');

    INSERT INTO customers (first_name, last_name, email, city, country) VALUES
    ('Alex', 'Chen', 'alex.chen@example.com', 'San Francisco', 'USA'),
    ('Sophia', 'Miller', 'sophia.m@example.com', 'Seattle', 'USA'),
    ('Liam', 'Novak', 'liam.novak@example.com', 'Austin', 'USA'),
    ('Emma', 'Wilson', 'emma.w@example.com', 'New York', 'USA'),
    ('Lucas', 'Silva', 'lucas.silva@example.com', 'Toronto', 'Canada'),
    ('Olivia', 'Taylor', 'olivia.t@example.com', 'London', 'UK');

    INSERT INTO orders (customer_id, order_number, total_amount, order_status, payment_method) VALUES
    (1, 'ORD-2026-1001', 2029.98, 'completed', 'credit_card'),
    (2, 'ORD-2026-1002', 299.00, 'processing', 'paypal'),
    (3, 'ORD-2026-1003', 1000.00, 'completed', 'credit_card'),
    (4, 'ORD-2026-1004', 129.99, 'pending', 'apple_pay'),
    (5, 'ORD-2026-1005', 429.98, 'completed', 'credit_card');

    INSERT INTO order_items (order_id, product_id, quantity, unit_price, total_price) VALUES
    (1, 1, 1, 1899.99, 1899.99),
    (1, 3, 1, 129.99, 129.99),
    (2, 4, 1, 299.00, 299.00),
    (3, 7, 1, 420.00, 420.00),
    (3, 8, 1, 580.00, 580.00),
    (4, 3, 1, 129.99, 129.99),
    (5, 9, 1, 349.99, 349.99),
    (5, 10, 1, 79.99, 79.99);

    INSERT INTO reviews (product_id, customer_id, rating, comment) VALUES
    (1, 1, 5, 'Incredible build quality and blistering fast performance!'),
    (3, 1, 4, 'Very satisfying tactile switches, vibrant backlight.'),
    (4, 2, 5, 'ANC is top tier. Blocks out street and airplane noise effortlessly.'),
    (7, 3, 5, 'My lower back pain disappeared after two weeks of using this chair.'),
    (9, 5, 4, 'Great battery life and clear OLED screen. Step counter is accurate.');
    )SQL";

    char* errMsg = nullptr;
    const int result = sqlite3_exec(db, ddl, nullptr, nullptr, &errMsg);
    if (errMsg) sqlite3_free(errMsg);
    sqlite3_close(db);
    if (result != SQLITE_OK) throw std::runtime_error("Cannot initialize sample database tables.");
}

void DbEngine::SeedDevStudio(const std::string& path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) { if (db) sqlite3_close(db); throw std::runtime_error("Cannot initialize sample database."); }

    const char* ddl = R"SQL(
    CREATE TABLE IF NOT EXISTS api_services (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL UNIQUE,
        base_url TEXT NOT NULL,
        environment TEXT DEFAULT 'development',
        status TEXT DEFAULT 'operational'
    );

    CREATE TABLE IF NOT EXISTS endpoints (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        service_id INTEGER NOT NULL,
        path TEXT NOT NULL,
        method TEXT NOT NULL CHECK(method IN ('GET', 'POST', 'PUT', 'DELETE', 'PATCH', 'OPTIONS')),
        summary TEXT,
        rate_limit INTEGER DEFAULT 100,
        auth_required BOOLEAN DEFAULT 1,
        FOREIGN KEY (service_id) REFERENCES api_services(id) ON DELETE CASCADE
    );

    CREATE TABLE IF NOT EXISTS api_keys (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        key_hash TEXT UNIQUE NOT NULL,
        label TEXT NOT NULL,
        role TEXT DEFAULT 'developer',
        is_active BOOLEAN DEFAULT 1,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    );

    CREATE TABLE IF NOT EXISTS request_metrics (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        endpoint_id INTEGER NOT NULL,
        status_code INTEGER NOT NULL,
        latency_ms REAL NOT NULL,
        ip_address TEXT,
        timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        FOREIGN KEY (endpoint_id) REFERENCES endpoints(id) ON DELETE CASCADE
    );

    CREATE INDEX IF NOT EXISTS idx_endpoints_service ON endpoints(service_id);
    CREATE INDEX IF NOT EXISTS idx_metrics_endpoint ON request_metrics(endpoint_id);

    INSERT INTO api_services (name, base_url, environment, status) VALUES
    ('Authentication Gateway', 'https://auth.company.internal/v1', 'production', 'operational'),
    ('Payments API', 'https://payments.company.internal/v2', 'production', 'operational'),
    ('Product Catalog Service', 'https://catalog.company.internal/v1', 'staging', 'operational');

    INSERT INTO endpoints (service_id, path, method, summary, rate_limit, auth_required) VALUES
    (1, '/auth/login', 'POST', 'Issue JWT token with user credentials', 20, 0),
    (1, '/auth/refresh', 'POST', 'Refresh expiring session token', 50, 1),
    (1, '/auth/me', 'GET', 'Fetch authenticated user profile', 200, 1),
    (2, '/checkout/charge', 'POST', 'Process one-time credit card charge', 10, 1),
    (2, '/checkout/refund', 'POST', 'Issue payment refund', 5, 1),
    (3, '/items', 'GET', 'Search and filter active catalog products', 500, 0),
    (3, '/items/{id}', 'GET', 'Retrieve single product by ID', 1000, 0);

    INSERT INTO api_keys (key_hash, label, role, is_active) VALUES
    ('ak_live_79a2b8e40f1190bc', 'Production Web App Key', 'admin', 1),
    ('ak_test_3c17fa918b45cd2a', 'Staging QA Automation', 'developer', 1),
    ('ak_read_90f4e18321cb88ee', 'Third-Party Analytics Bot', 'read_only', 1);

    INSERT INTO request_metrics (endpoint_id, status_code, latency_ms, ip_address) VALUES
    (1, 200, 42.5, '192.168.1.10'),
    (1, 401, 15.2, '192.168.1.15'),
    (3, 200, 18.7, '10.0.4.12'),
    (4, 200, 120.4, '10.0.4.12'),
    (6, 200, 8.9, '172.16.0.4'),
    (6, 200, 9.4, '172.16.0.8'),
    (7, 200, 6.1, '172.16.0.9');
    )SQL";

    char* errMsg = nullptr;
    const int result = sqlite3_exec(db, ddl, nullptr, nullptr, &errMsg);
    if (errMsg) sqlite3_free(errMsg);
    sqlite3_close(db);
    if (result != SQLITE_OK) throw std::runtime_error("Cannot initialize sample database tables.");
}

std::vector<DatabaseInfo> DbEngine::ListDatabases() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<DatabaseInfo> list;
    for (auto& pair : databases_) {
        if(connections_->Has(pair.first))continue;
        DatabaseInfo info = pair.second;
        if (fs::exists(fs::u8path(info.path))) {
            info.sizeBytes = fs::file_size(fs::u8path(info.path));
        }
        // Count tables
        sqlite3* db = nullptr;
        if (OpenDatabase(info.path, &db) == SQLITE_OK) {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%';", -1, &stmt, nullptr) == SQLITE_OK) {
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    info.tableCount = sqlite3_column_int(stmt, 0);
                }
                sqlite3_finalize(stmt);
            }
        }
        if (db) sqlite3_close(db);
        list.push_back(info);
    }
    auto profiles=connections_->List(); list.insert(list.end(),profiles.begin(),profiles.end());
    return list;
}

DatabaseInfo DbEngine::CreateDatabase(const std::string& name, const std::string& preset, const std::string& destinationPath) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (name.empty()) throw std::runtime_error("Database name cannot be empty.");
    if (preset != "blank" && preset != "ecommerce" && preset != "dev_studio") throw std::runtime_error("Choose a blank, E-Commerce, or Developer Studio preset.");
    std::string slug;
    for (char c : name) {
        if (isalnum(static_cast<unsigned char>(c))) slug += static_cast<char>(tolower(static_cast<unsigned char>(c)));
        else if (c == ' ' || c == '-' || c == '_') slug += '_';
    }
    while (!slug.empty() && slug.back() == '_') slug.pop_back();
    if (slug.empty()) slug = "database";

    std::string dbId = slug;
    int suffix = 2;
    while (databases_.find(dbId) != databases_.end() || fs::exists(fs::u8path(dataDir_) / (dbId + ".db"))) {
        dbId = slug + "_" + std::to_string(suffix++);
    }

    const auto selected = destinationPath.empty() ? (fs::u8path(dataDir_) / (dbId + ".db")) : fs::u8path(destinationPath);
    if (!selected.is_absolute() || !fs::is_directory(selected.parent_path())) throw std::runtime_error("Choose an absolute database file path in an existing folder.");
    // Reserve the exact selected path exclusively and keep it protected from
    // replacement until SQLite has initialized it. Existing files are never
    // overwritten, including a file created by another application in a race.
    HANDLE reservation = CreateFileW(selected.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reservation == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create the database file. Choose a new filename and check folder access.");
    struct FileReservation { HANDLE handle; ~FileReservation() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); } } fileOwner{reservation};
    std::string dbPath = selected.u8string();
    try {
    if (preset == "ecommerce") {
        SeedEcommerce(dbPath);
    } else if (preset == "dev_studio") {
        SeedDevStudio(dbPath);
    } else {
        sqlite3* db = nullptr;
        const int rc = sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        if (db) sqlite3_close(db);
        if (rc != SQLITE_OK) throw std::runtime_error("Cannot create database. Check folder access and free disk space.");
    }
    } catch (...) {
        CloseHandle(fileOwner.handle); fileOwner.handle = INVALID_HANDLE_VALUE;
        std::error_code error; fs::remove(selected, error); throw;
    }

    DatabaseInfo info;
    info.id = dbId;
    info.name = name;
    info.path = dbPath;
    info.isSample = false;
    databases_[dbId] = info;
    if (fs::weakly_canonical(selected.parent_path()) != fs::weakly_canonical(fs::u8path(dataDir_))) attachedIds_.insert(dbId);
    try { SaveCatalog(); }
    catch (...) {
        databases_.erase(dbId); attachedIds_.erase(dbId);
        CloseHandle(fileOwner.handle); fileOwner.handle = INVALID_HANDLE_VALUE;
        std::error_code error; fs::remove(selected, error); throw;
    }
    return info;
}

DatabaseInfo DbEngine::AttachDatabase(const std::string& name, const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!fs::is_regular_file(fs::u8path(path))) throw std::runtime_error("Select an existing SQLite database file.");
    sqlite3* db = nullptr;
    if (OpenDatabase(path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        throw std::runtime_error("Cannot open the selected SQLite database.");
    }
    sqlite3_stmt* validation = nullptr;
    const bool valid = sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master LIMIT 1;", -1, &validation, nullptr) == SQLITE_OK;
    if (validation) sqlite3_finalize(validation);
    sqlite3_close(db);
    if (!valid) throw std::runtime_error("The selected file is not a valid SQLite database.");
    std::string slug;
    for (char c : name) {
        if (isalnum(static_cast<unsigned char>(c))) slug += static_cast<char>(tolower(static_cast<unsigned char>(c)));
        else if (c == ' ' || c == '-' || c == '_') slug += '_';
    }
    while (!slug.empty() && slug.back() == '_') slug.pop_back();
    if (slug.empty()) slug = "attached_db";
    std::string dbId = slug;
    int suffix = 2;
    while (databases_.find(dbId) != databases_.end()) dbId = slug + "_" + std::to_string(suffix++);
    DatabaseInfo info;
    info.id = dbId; info.name = name; info.path = path; info.isSample = false;
    databases_[dbId] = info;
    attachedIds_.insert(dbId);
    SaveCatalog();
    return info;
}

bool DbEngine::DeleteDatabase(const std::string& dbId) {
    if(connections_->Has(dbId))return connections_->Remove(dbId);
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = databases_.find(dbId);
    if (it == databases_.end()) return false;
    if (it->second.isSample) throw std::runtime_error("Sample databases cannot be deleted.");

    std::string path = it->second.path;
    std::error_code ec;
    // Attached user files are detached. Only direct children of the managed
    // data directory may be deleted by the database manager.
    const auto resolved = fs::weakly_canonical(fs::u8path(path), ec);
    if (ec) return false;
    const auto managed = fs::weakly_canonical(fs::u8path(dataDir_), ec);
    if (ec) return false;
    if (!attachedIds_.count(dbId) && resolved.parent_path() == managed && !fs::is_symlink(fs::u8path(path))) {
        if (!fs::remove(fs::u8path(path), ec) || ec) return false;
    }
    databases_.erase(it);
    attachedIds_.erase(dbId);
    SaveCatalog();
    return true;
}

DatabaseSchema DbEngine::GetSchema(const std::string& dbId, const std::atomic_bool* cancellation) {
    CheckCancellation(cancellation);
    if(auto provider=connections_->Provider(dbId))return provider->Schema(cancellation);
    auto lock=MutationLock(mutex_,cancellation);
    DatabaseSchema schema;
    schema.databaseId = dbId;
    const auto path = GetDbPath(dbId);
    schema.databaseName = connections_->Has(dbId) ? connections_->Profile(dbId).name : databases_.at(dbId).name;
    schema.capabilities=ProviderCapabilities("sql",IsReadOnly(dbId),"sqlite");
    sqlite3* raw = nullptr;
    const int opened = OpenDatabase(path, &raw, IsReadOnly(dbId));
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, &sqlite3_close);
    if (opened != SQLITE_OK) throw std::runtime_error("Cannot open database for schema inspection.");
    ConfigureCancellation(db.get(), cancellation);
    auto text = [](sqlite3_stmt* statement, int column) {
        const char* value = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
        return std::string(value ? value : "");
    };
    auto tables = Prepare(db.get(), "SELECT type,name,sql FROM sqlite_master WHERE type IN ('table','view') AND name NOT LIKE 'sqlite_%' ORDER BY name;", cancellation);
    int tableResult = SQLITE_OK;
    while ((tableResult = sqlite3_step(tables.get())) == SQLITE_ROW) {
        CheckCancellation(cancellation);
        TableMeta table;
        table.type = text(tables.get(), 0); table.name = text(tables.get(), 1); table.ddl = text(tables.get(), 2);
        table.editable = table.type == "table" && !IsReadOnly(dbId);
        auto columns = Prepare(db.get(), "PRAGMA table_info(" + QuoteIdentifier(table.name) + ");", cancellation);
        int result = SQLITE_OK;
        while ((result = sqlite3_step(columns.get())) == SQLITE_ROW) {
            CheckCancellation(cancellation);
            ColumnMeta column;
            column.cid = sqlite3_column_int(columns.get(), 0); column.name = text(columns.get(), 1);
            column.type = text(columns.get(), 2); column.notnull = sqlite3_column_int(columns.get(), 3) != 0;
            column.dfltValue = text(columns.get(), 4); column.pk = sqlite3_column_int(columns.get(), 5) != 0;
            if (column.pk) table.primaryKeys.push_back(column.name);
            table.columns.push_back(column);
        }
        CheckSqlite(db.get(), result, cancellation);
        auto foreignKeys = Prepare(db.get(), "PRAGMA foreign_key_list(" + QuoteIdentifier(table.name) + ");", cancellation);
        while ((result = sqlite3_step(foreignKeys.get())) == SQLITE_ROW) {
            CheckCancellation(cancellation);
            ForeignKeyMeta key;
            key.id = sqlite3_column_int(foreignKeys.get(), 0); key.seq = sqlite3_column_int(foreignKeys.get(), 1);
            key.targetTable = text(foreignKeys.get(), 2); key.fromColumn = text(foreignKeys.get(), 3); key.toColumn = text(foreignKeys.get(), 4);
            key.onUpdate = text(foreignKeys.get(), 5); key.onDelete = text(foreignKeys.get(), 6); table.foreignKeys.push_back(key);
        }
        CheckSqlite(db.get(), result, cancellation);
        auto indexes = Prepare(db.get(), "PRAGMA index_list(" + QuoteIdentifier(table.name) + ");", cancellation);
        while ((result = sqlite3_step(indexes.get())) == SQLITE_ROW) {
            CheckCancellation(cancellation);
            IndexMeta index;
            index.name = text(indexes.get(), 1); index.unique = sqlite3_column_int(indexes.get(), 2) != 0; index.origin = text(indexes.get(), 3);
            auto indexedColumns = Prepare(db.get(), "PRAGMA index_info(" + QuoteIdentifier(index.name) + ");", cancellation);
            int indexedResult = SQLITE_OK;
            while ((indexedResult = sqlite3_step(indexedColumns.get())) == SQLITE_ROW) {
                CheckCancellation(cancellation);
                const auto column = text(indexedColumns.get(), 2); index.columns.push_back(column.empty() ? "(expression)" : column);
            }
            CheckSqlite(db.get(), indexedResult, cancellation);
            table.indexes.push_back(index);
        }
        CheckSqlite(db.get(), result, cancellation);
        if (table.type == "table") {
            auto count = Prepare(db.get(), "SELECT COUNT(*) FROM " + QuoteIdentifier(table.name) + ";", cancellation);
            const int countResult = sqlite3_step(count.get());
            if (countResult == SQLITE_ROW) table.rowCount = sqlite3_column_int64(count.get(), 0);
            else CheckSqlite(db.get(), countResult, cancellation);
        }
        schema.tables.push_back(std::move(table));
    }
    CheckSqlite(db.get(), tableResult, cancellation);
    return schema;
}

ERDiagram DbEngine::GetErDiagram(const std::string& dbId, const std::atomic_bool* cancellation) {
    const DatabaseSchema schema = GetSchema(dbId, cancellation);
    ERDiagram er; er.databaseId = dbId;
    for (const auto& table : schema.tables) {
        CheckCancellation(cancellation);
        if (table.type != "table") continue;
        ERNode node; node.id = table.name; node.name = table.name; node.columns = table.columns; node.rowCount = table.rowCount;
        er.nodes.push_back(node);
        for (const auto& key : table.foreignKeys) {
            CheckCancellation(cancellation);
            ERLink link;
            link.id = table.name + "." + key.fromColumn + "->" + key.targetTable + "." + key.toColumn;
            link.source = table.name; link.sourceCol = key.fromColumn; link.target = key.targetTable; link.targetCol = key.toColumn;
            link.onUpdate = key.onUpdate; link.onDelete = key.onDelete; er.links.push_back(link);
        }
    }
    return er;
}
QueryResult DbEngine::ExecuteQuery(const std::string& dbId, const std::string& sql, int maxRows, const std::atomic_bool* cancellation) {
    try { if(auto provider=connections_->Provider(dbId))return provider->Query(sql,maxRows,cancellation); if(IsReadOnly(dbId))RequireReadOnlySql(sql); }
    catch(const std::exception& error){ QueryResult result;result.query=sql;result.error=error.what();return result; }
    QueryResult qr;
    qr.query = sql;
    std::unique_lock<std::recursive_mutex> lock(mutex_,std::defer_lock);
    try { while(!lock.try_lock()){CheckCancellation(cancellation);Sleep(5);}CheckCancellation(cancellation); }
    catch(const std::exception& error){qr.error=error.what();return qr;}
    const auto start = std::chrono::steady_clock::now();
    std::string dbPath;
    try { dbPath = GetDbPath(dbId); }
    catch (const std::exception& ex) { qr.error = ex.what(); return qr; }
    sqlite3* db = nullptr;
    if (OpenDatabase(dbPath, &db, IsReadOnly(dbId)) != SQLITE_OK) {
        qr.error = db ? sqlite3_errmsg(db) : "Could not open database file.";
        if (db) sqlite3_close(db);
        return qr;
    }
    if (Cancelled(cancellation)) { sqlite3_close(db); qr.error = "Query cancelled."; return qr; }
    try { ConfigureCancellation(db, cancellation); }
    catch (const std::exception& error) { qr.error = error.what(); sqlite3_close(db); return qr; }

    // A batch is atomic unless it explicitly manages its own transactions.
    // Prepare token text with SQLite so comments and quoted literals cannot
    // accidentally turn off the automatic transaction.
    bool explicitTransaction = false;
    bool maintenance = false;
    const char* scan = sql.c_str();
    while (*scan) {
        const char* tail = nullptr;
        sqlite3_stmt* statement = nullptr;
        int rc = sqlite3_prepare_v2(db, scan, -1, &statement, &tail);
        if (statement) {
            const std::string text = sqlite3_sql(statement);
            std::string token;
            size_t pos = 0;
            while (pos < text.size()) {
                if (isspace(static_cast<unsigned char>(text[pos]))) { ++pos; continue; }
                if (text.compare(pos, 2, "--") == 0) { const auto end = text.find('\n', pos); pos = end == std::string::npos ? text.size() : end + 1; continue; }
                if (text.compare(pos, 2, "/*") == 0) { const auto end = text.find("*/", pos + 2); pos = end == std::string::npos ? text.size() : end + 2; continue; }
                break;
            }
            while (pos < text.size() && isalpha(static_cast<unsigned char>(text[pos]))) token += static_cast<char>(toupper(static_cast<unsigned char>(text[pos++])));
            explicitTransaction |= token == "BEGIN" || token == "COMMIT" || token == "END" || token == "ROLLBACK" || token == "SAVEPOINT" || token == "RELEASE";
            // VACUUM and several writable PRAGMAs must run outside an implicit
            // transaction. Maintenance batches use SQLite's native semantics.
            maintenance |= token == "VACUUM" || token == "PRAGMA";
            sqlite3_finalize(statement);
        }
        // Subsequent statements can reference tables created earlier; a
        // preparation failure is not validation of the whole batch.
        if (!tail || tail <= scan) break;
        scan = tail;
        (void)rc;
    }
    const bool automaticTransaction = !explicitTransaction && !maintenance;
    if (automaticTransaction && sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr) != SQLITE_OK) qr.error = sqlite3_errmsg(db);
    const char* pLeft = sql.c_str();
    sqlite3_stmt* stmt = nullptr;
    int64_t totalAffected = 0;
    maxRows = std::clamp(maxRows, 1, 100000);

    while (*pLeft && qr.error.empty()) {
        if (cancellation && cancellation->load()) { qr.error = "Query cancelled."; break; }
        while (*pLeft && isspace(static_cast<unsigned char>(*pLeft))) pLeft++;
        if (!*pLeft) break;

        const char* pTail = nullptr;
        int rc = sqlite3_prepare_v2(db, pLeft, -1, &stmt, &pTail);
        if (rc != SQLITE_OK) {
            qr.error = sqlite3_errmsg(db);
            break;
        }

        if (!stmt) {
            pLeft = pTail;
            continue;
        }

        int colCount = sqlite3_column_count(stmt);
        const bool readOnly = sqlite3_stmt_readonly(stmt) != 0;
        const int previousChanges = sqlite3_total_changes(db);
        int rcStep = SQLITE_OK;
        if (colCount > 0) {
            qr.columns.clear();
            qr.rows.clear();
            qr.typedRows.clear();
            for (int i = 0; i < colCount; ++i) {
                qr.columns.push_back(UniqueColumnLabel(sqlite3_column_name(stmt, i), qr.columns));
            }

            int count = 0;
            qr.truncated = false;
            while ((rcStep = sqlite3_step(stmt)) == SQLITE_ROW) {
                if (count < maxRows) {
                    std::map<std::string, std::string> row;
                    TypedRow typed;
                    for (int i = 0; i < colCount; ++i) {
                        const auto value = SqliteValue(stmt, i);
                        typed[qr.columns[i]] = value; row[qr.columns[i]] = DbValueDisplay(value);
                    }
                    qr.rows.push_back(row);
                    qr.typedRows.push_back(std::move(typed));
                } else {
                    qr.truncated = true;
                }
                count++;
                // Read-only queries need not scan millions of discarded rows.
                if (readOnly && qr.truncated) { rcStep = SQLITE_DONE; break; }
            }
            qr.rowCount = static_cast<int64_t>(qr.rows.size());
            if (rcStep == SQLITE_DONE) {
                QueryResultSet resultSet;
                resultSet.query = sqlite3_sql(stmt); resultSet.columns = qr.columns; resultSet.rows = qr.rows; resultSet.typedRows = qr.typedRows;
                resultSet.rowCount = qr.rowCount; resultSet.truncated = qr.truncated; qr.resultSets.push_back(std::move(resultSet));
            }
        } else { rcStep = sqlite3_step(stmt); }
        if (rcStep != SQLITE_DONE) qr.error = cancellation && cancellation->load() ? "Query cancelled." : sqlite3_errmsg(db);
        if (!readOnly && rcStep == SQLITE_DONE && sqlite3_total_changes(db) != previousChanges) totalAffected += sqlite3_changes(db);

        sqlite3_finalize(stmt);
        pLeft = pTail;
    }
    if (qr.error.empty() && automaticTransaction && sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) qr.error = sqlite3_errmsg(db);
    if (!qr.error.empty()) {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (Cancelled(cancellation)) qr.error = "Query cancelled.";
        qr.columns.clear(); qr.rows.clear(); qr.typedRows.clear(); qr.resultSets.clear(); qr.rowCount = 0; qr.truncated = false;
    } else qr.rowsAffected = totalAffected;
    auto end = std::chrono::steady_clock::now();
    qr.executionTimeMs = std::chrono::duration<double, std::milli>(end - start).count();

    sqlite3_close(db);
    return qr;
}

TableDataResult DbEngine::GetTableData(
    const std::string& dbId, const std::string& table, int page, int pageSize,
    const std::string& sortCol, const std::string& sortDir, const std::string& search,
    const std::string& filterCol, const std::string& filterVal, const std::atomic_bool* cancellation
) {
    TableDataResult result; result.table = table; result.page = std::max(1, page); result.pageSize = std::clamp(pageSize, 1, 200);
    try {
        CheckCancellation(cancellation);
        if(auto provider=connections_->Provider(dbId))return provider->Table(table,page,pageSize,sortCol,sortDir,search,filterCol,filterVal,cancellation);
        auto lock=MutationLock(mutex_,cancellation);
        sqlite3* raw = nullptr;
        const int opened = OpenDatabase(GetDbPath(dbId), &raw, IsReadOnly(dbId));
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, &sqlite3_close);
        if (opened != SQLITE_OK) throw std::runtime_error("Cannot open database for table browsing.");
        ConfigureCancellation(db.get(), cancellation);
        auto columns = Prepare(db.get(), "PRAGMA table_info(" + QuoteIdentifier(table) + ");", cancellation);
        int step = SQLITE_OK;
        while ((step = sqlite3_step(columns.get())) == SQLITE_ROW) {
            CheckCancellation(cancellation);
            ColumnMeta column; column.cid = sqlite3_column_int(columns.get(), 0);
            column.name = reinterpret_cast<const char*>(sqlite3_column_text(columns.get(), 1));
            const char* type = reinterpret_cast<const char*>(sqlite3_column_text(columns.get(), 2)); column.type = type ? type : "TEXT";
            column.notnull = sqlite3_column_int(columns.get(), 3) != 0;
            const char* defaultValue = reinterpret_cast<const char*>(sqlite3_column_text(columns.get(), 4)); column.dfltValue = defaultValue ? defaultValue : "";
            column.pk = sqlite3_column_int(columns.get(), 5) != 0;
            if (column.pk) result.primaryKeys.push_back(column.name);
            result.columns.push_back(column.name); result.columnMetadata.push_back(column);
        }
        CheckSqlite(db.get(), step, cancellation);
        auto validColumn = [&](const std::string& name) { return std::find(result.columns.begin(), result.columns.end(), name) != result.columns.end(); };
        if (result.columns.empty()) throw std::runtime_error("Table does not exist.");
        if ((!filterCol.empty() && !validColumn(filterCol)) || (!sortCol.empty() && !validColumn(sortCol))) throw std::runtime_error("Unknown filter or sort column.");
        std::string where;
        std::vector<std::string> parameters;
        if (!filterCol.empty()) { where = " WHERE CAST(" + QuoteIdentifier(filterCol) + " AS TEXT) LIKE ? ESCAPE '\\'"; parameters.push_back(SearchPattern(filterVal)); }
        if (!search.empty()) {
            where += where.empty() ? " WHERE (" : " AND (";
            for (size_t i = 0; i < result.columns.size(); ++i) {
                if (i) where += " OR ";
                where += "CAST(" + QuoteIdentifier(result.columns[i]) + " AS TEXT) LIKE ? ESCAPE '\\'"; parameters.push_back(SearchPattern(search));
            }
            where += ")";
        }
        auto bind = [&](sqlite3_stmt* statement) {
            for (size_t i = 0; i < parameters.size(); ++i) CheckSqlite(db.get(), sqlite3_bind_text(statement, static_cast<int>(i + 1), parameters[i].c_str(), -1, SQLITE_TRANSIENT), cancellation);
        };
        auto count = Prepare(db.get(), "SELECT COUNT(*) FROM " + QuoteIdentifier(table) + where + ";", cancellation);
        bind(count.get()); step = sqlite3_step(count.get());
        if (step == SQLITE_ROW) result.totalRows = sqlite3_column_int64(count.get(), 0);
        else CheckSqlite(db.get(), step, cancellation);
        CheckCancellation(cancellation);
        const auto pages = (result.totalRows + result.pageSize - 1) / result.pageSize;
        result.totalPages = static_cast<int>(std::clamp<int64_t>(pages, 1, INT_MAX));
        result.page = std::clamp(result.page, 1, result.totalPages);
        const int64_t offset = static_cast<int64_t>(result.page - 1) * result.pageSize;
        std::string ordering;
        if (!sortCol.empty()) ordering = " ORDER BY " + QuoteIdentifier(sortCol) + (_stricmp(sortDir.c_str(), "desc") == 0 ? " DESC" : " ASC");
        else if (!result.primaryKeys.empty()) ordering = " ORDER BY " + QuoteIdentifier(result.primaryKeys[0]) + " ASC";
        auto rows = Prepare(db.get(), "SELECT * FROM " + QuoteIdentifier(table) + where + ordering + " LIMIT ? OFFSET ?;", cancellation);
        bind(rows.get());
        CheckSqlite(db.get(), sqlite3_bind_int(rows.get(), static_cast<int>(parameters.size() + 1), result.pageSize), cancellation);
        CheckSqlite(db.get(), sqlite3_bind_int64(rows.get(), static_cast<int>(parameters.size() + 2), offset), cancellation);
        while ((step = sqlite3_step(rows.get())) == SQLITE_ROW) {
            CheckCancellation(cancellation);
            std::map<std::string, std::string> row;
            TypedRow typed;
            for (int i = 0; i < sqlite3_column_count(rows.get()); ++i) {
                const auto value = SqliteValue(rows.get(), i); const auto name = sqlite3_column_name(rows.get(), i);
                typed[name] = value; row[name] = DbValueDisplay(value);
            }
            result.rows.push_back(std::move(row));
            result.typedRows.push_back(std::move(typed));
        }
        CheckSqlite(db.get(), step, cancellation);
    } catch (const std::exception& error) {
        result.error = error.what(); result.rows.clear(); result.typedRows.clear(); result.totalRows = 0; result.totalPages = 1;
    }
    return result;
}
static void BindTyped(sqlite3* db, sqlite3_stmt* statement, const std::vector<DbValue>& values, const std::atomic_bool* cancellation) {
    for(size_t i=0;i<values.size();++i){CheckCancellation(cancellation);const auto& value=values[i];const int position=static_cast<int>(i+1);int result=SQLITE_ERROR;
        if(value.type==DbValueType::Null)result=sqlite3_bind_null(statement,position);
        else if(value.type==DbValueType::Integer)result=sqlite3_bind_int64(statement,position,DbValueJson(value).get<int64_t>());
        else if(value.type==DbValueType::Real)result=sqlite3_bind_double(statement,position,DbValueJson(value).get<double>());
        else if(value.type==DbValueType::Boolean)result=sqlite3_bind_int(statement,position,DbValueJson(value).get<bool>()?1:0);
        else if(value.type==DbValueType::Blob){if(value.text.size()>64*1024*1024)throw std::runtime_error("A binary value exceeds 64 MiB.");result=sqlite3_bind_blob(statement,position,value.text.data(),static_cast<int>(value.text.size()),SQLITE_TRANSIENT);}
        else{const auto text=value.type==DbValueType::Json?DbValueJson(value).dump():value.text;if(text.size()>64*1024*1024)throw std::runtime_error("A text value exceeds 64 MiB.");result=sqlite3_bind_text(statement,position,text.data(),static_cast<int>(text.size()),SQLITE_TRANSIENT);}CheckSqlite(db,result,cancellation);
    }
}
static std::string TypedKeyWhere(const TypedRow& key,std::vector<DbValue>& parameters) {std::string text;for(const auto& item:key){if(!text.empty())text+=" AND ";text+=QuoteIdentifier(item.first)+(item.second.type==DbValueType::Null?" IS NULL":" = ?");if(item.second.type!=DbValueType::Null)parameters.push_back(item.second);}return text;}
static void CheckTypedColumns(const TableMeta& metadata,const TypedRow& row){for(const auto& item:row){if(std::none_of(metadata.columns.begin(),metadata.columns.end(),[&](const auto& column){return column.name==item.first;}))throw std::runtime_error("Unknown table column.");DbValueJson(item.second);}}
static void CheckTypedKey(const TableMeta& metadata,const TypedRow& key){if(key.size()!=metadata.primaryKeys.size()||key.empty())throw std::runtime_error("Provide the complete primary key to identify one row.");for(const auto& name:metadata.primaryKeys)if(!key.count(name)||key.at(name).type==DbValueType::Null)throw std::runtime_error("Provide the complete non-null primary key to identify one row.");}
static TableMeta RowMetadata(DbEngine& engine,const std::string& id,const std::string& name,const std::atomic_bool* cancellation){const auto schema=engine.GetSchema(id,cancellation);const auto table=std::find_if(schema.tables.begin(),schema.tables.end(),[&](const auto& item){return item.name==name;});if(table==schema.tables.end())throw std::runtime_error("Unknown table or view.");return *table;}

bool DbEngine::InsertTableRow(const std::string& id,const std::string& table,const std::map<std::string,std::string>& row){try{return InsertTypedRow(id,table,TextRow(row));}catch(...){return false;}}
bool DbEngine::UpdateTableRow(const std::string& id,const std::string& table,const std::map<std::string,std::string>& key,const std::map<std::string,std::string>& row){try{return UpdateTypedRow(id,table,TextRow(key),TextRow(row));}catch(...){return false;}}
bool DbEngine::DeleteTableRow(const std::string& id,const std::string& table,const std::map<std::string,std::string>& key){try{return DeleteTypedRow(id,table,TextRow(key));}catch(...){return false;}}
bool DbEngine::InsertTypedRow(const std::string& id,const std::string& table,const TypedRow& row,const std::atomic_bool* cancellation){return InsertTypedReturningRow(id,table,row,cancellation).success;}
RowMutationResult DbEngine::InsertTypedReturningRow(const std::string& id,const std::string& table,const TypedRow& row,const std::atomic_bool* cancellation){CheckCancellation(cancellation);if(IsReadOnly(id))throw std::runtime_error("This connection is read-only.");RowMutationResult result;if(auto provider=connections_->Provider(id)){if(auto sql=dynamic_cast<SqlProvider*>(provider.get())){const auto inserted=sql->InsertResult(table,row,cancellation);result.success=inserted.rowsAffected>0||!inserted.typedRows.empty();result.rowsAffected=inserted.rowsAffected>0?inserted.rowsAffected:result.success?1:0;if(!inserted.typedRows.empty())result.returned=inserted.typedRows.front();else if(result.success){const auto metadata=sql->Metadata(table,cancellation);for(const auto& key:metadata.primaryKeys)if(row.count(key))result.returned[key]=row.at(key);}}else{result.success=provider->Insert(table,row,cancellation);result.rowsAffected=result.success?1:0;}return result;}
    auto lock=MutationLock(mutex_,cancellation);const auto metadata=RowMetadata(*this,id,table,cancellation);if(!metadata.editable)throw std::runtime_error("This table is read-only.");CheckTypedColumns(metadata,row);sqlite3* raw=nullptr;const auto opened=OpenDatabase(GetDbPath(id),&raw);std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,&sqlite3_close);if(opened!=SQLITE_OK)throw std::runtime_error("Cannot open database for row insertion.");ConfigureCancellation(db.get(),cancellation);
    std::string columns,markers;std::vector<DbValue> values;for(const auto& item:row){if(!columns.empty()){columns+=", ";markers+=", ";}columns+=QuoteIdentifier(item.first);markers+='?';values.push_back(item.second);}const auto sql="INSERT INTO "+QuoteIdentifier(table)+(row.empty()?" DEFAULT VALUES":" ("+columns+") VALUES ("+markers+")");auto statement=Prepare(db.get(),sql,cancellation);BindTyped(db.get(),statement.get(),values,cancellation);CheckSqlite(db.get(),sqlite3_step(statement.get()),cancellation);result.rowsAffected=sqlite3_changes(db.get());result.success=result.rowsAffected==1;
    if(result.success){const auto rowId=sqlite3_last_insert_rowid(db.get());if(metadata.primaryKeys.size()==1){const auto found=std::find_if(metadata.columns.begin(),metadata.columns.end(),[&](const auto& column){return column.name==metadata.primaryKeys[0];});if(found!=metadata.columns.end()&&_stricmp(found->type.c_str(),"INTEGER")==0&&!row.count(found->name))result.returned[found->name]={DbValueType::Integer,std::to_string(rowId)};else if(row.count(metadata.primaryKeys[0]))result.returned[metadata.primaryKeys[0]]=row.at(metadata.primaryKeys[0]);}else for(const auto& key:metadata.primaryKeys)if(row.count(key))result.returned[key]=row.at(key);}
    return result;
}
bool DbEngine::UpdateTypedRow(const std::string& id,const std::string& table,const TypedRow& key,const TypedRow& row,const std::atomic_bool* cancellation){CheckCancellation(cancellation);if(IsReadOnly(id))throw std::runtime_error("This connection is read-only.");if(auto provider=connections_->Provider(id))return provider->Update(table,key,row,cancellation);if(row.empty())return false;
    auto lock=MutationLock(mutex_,cancellation);const auto metadata=RowMetadata(*this,id,table,cancellation);if(!metadata.editable)throw std::runtime_error("This table is read-only.");CheckTypedKey(metadata,key);CheckTypedColumns(metadata,row);sqlite3* raw=nullptr;const auto opened=OpenDatabase(GetDbPath(id),&raw);std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,&sqlite3_close);if(opened!=SQLITE_OK)throw std::runtime_error("Cannot open database for row update.");ConfigureCancellation(db.get(),cancellation);
    std::string sets;std::vector<DbValue> values;for(const auto& item:row){if(!sets.empty())sets+=", ";sets+=QuoteIdentifier(item.first)+" = ?";values.push_back(item.second);}const auto where=TypedKeyWhere(key,values);auto statement=Prepare(db.get(),"UPDATE "+QuoteIdentifier(table)+" SET "+sets+" WHERE "+where,cancellation);BindTyped(db.get(),statement.get(),values,cancellation);CheckSqlite(db.get(),sqlite3_step(statement.get()),cancellation);return sqlite3_changes(db.get())==1;
}
bool DbEngine::DeleteTypedRow(const std::string& id,const std::string& table,const TypedRow& key,const std::atomic_bool* cancellation){CheckCancellation(cancellation);if(IsReadOnly(id))throw std::runtime_error("This connection is read-only.");if(auto provider=connections_->Provider(id))return provider->Delete(table,key,cancellation);
    auto lock=MutationLock(mutex_,cancellation);const auto metadata=RowMetadata(*this,id,table,cancellation);if(!metadata.editable)throw std::runtime_error("This table is read-only.");CheckTypedKey(metadata,key);sqlite3* raw=nullptr;const auto opened=OpenDatabase(GetDbPath(id),&raw);std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,&sqlite3_close);if(opened!=SQLITE_OK)throw std::runtime_error("Cannot open database for row deletion.");ConfigureCancellation(db.get(),cancellation);std::vector<DbValue> values;const auto where=TypedKeyWhere(key,values);auto statement=Prepare(db.get(),"DELETE FROM "+QuoteIdentifier(table)+" WHERE "+where,cancellation);BindTyped(db.get(),statement.get(),values,cancellation);CheckSqlite(db.get(),sqlite3_step(statement.get()),cancellation);return sqlite3_changes(db.get())==1;
}
TypedRow DbEngine::FindTypedRow(const std::string& id,const std::string& table,const TypedRow& key,const std::atomic_bool* cancellation){CheckCancellation(cancellation);if(auto provider=connections_->Provider(id)){if(auto sql=dynamic_cast<SqlProvider*>(provider.get()))return sql->Find(table,key,cancellation);if(provider->Profile().type=="mongodb"){const auto result=provider->Query(json{{"collection",table},{"operation","find_one"},{"filter",[&]{json object=json::object();for(const auto& item:key)object[item.first]=DbValueJson(item.second);return object;}()}}.dump(),1,cancellation);if(!result.error.empty())throw std::runtime_error(result.error);return result.typedRows.empty()?TypedRow{}:result.typedRows[0];}throw std::runtime_error("Direct record lookup is available for SQL and MongoDB connections.");}
    auto lock=MutationLock(mutex_,cancellation);const auto metadata=RowMetadata(*this,id,table,cancellation);CheckTypedKey(metadata,key);sqlite3* raw=nullptr;const auto opened=OpenDatabase(GetDbPath(id),&raw,IsReadOnly(id));std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,&sqlite3_close);if(opened!=SQLITE_OK)throw std::runtime_error("Cannot open database for record lookup.");ConfigureCancellation(db.get(),cancellation);std::vector<DbValue> values;const auto where=TypedKeyWhere(key,values);auto statement=Prepare(db.get(),"SELECT * FROM "+QuoteIdentifier(table)+" WHERE "+where+" LIMIT 1",cancellation);BindTyped(db.get(),statement.get(),values,cancellation);const auto step=sqlite3_step(statement.get());if(step!=SQLITE_ROW){CheckSqlite(db.get(),step,cancellation);return {};}
    TypedRow row;for(int column=0;column<sqlite3_column_count(statement.get());++column)row[sqlite3_column_name(statement.get(),column)]=SqliteValue(statement.get(),column);return row;
}

std::string DbEngine::ExportTableData(
    const std::string& dbId, const std::string& table, const std::string& format,
    const std::string& search, const std::string& sortCol, const std::string& sortDir, const std::atomic_bool* cancellation
) {
    CheckCancellation(cancellation);
    if(auto provider=connections_->Provider(dbId))return provider->Export(table,format,search,sortCol,sortDir,cancellation);
    auto lock=MutationLock(mutex_,cancellation);
    if (format != "csv" && format != "json" && format != "sql") throw std::runtime_error("Supported exports are CSV, JSON, and SQL.");
    const auto metadata = GetTableData(dbId, table, 1, 1, sortCol, sortDir, search, "", "", cancellation);
    if (!metadata.error.empty()) throw std::runtime_error(metadata.error);
    sqlite3* raw = nullptr;
    if (OpenDatabase(GetDbPath(dbId), &raw) != SQLITE_OK) { if (raw) sqlite3_close(raw); throw std::runtime_error("Cannot open database for export."); }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, &sqlite3_close);
    ConfigureCancellation(db.get(), cancellation);
    std::string query = "SELECT * FROM " + QuoteIdentifier(table);
    if (!search.empty()) {
        query += " WHERE (";
        for (size_t i = 0; i < metadata.columns.size(); ++i) {
            if (i) query += " OR ";
            query += "CAST(" + QuoteIdentifier(metadata.columns[i]) + " AS TEXT) LIKE ? ESCAPE '\\'";
        }
        query += ")";
    }
    const auto ordering = sortCol.empty() ? metadata.primaryKeys : std::vector<std::string>{sortCol};
    if (!ordering.empty()) {
        query += " ORDER BY ";
        for (size_t i = 0; i < ordering.size(); ++i) {
            if (i) query += ", ";
            query += QuoteIdentifier(ordering[i]) + (_stricmp(sortDir.c_str(), "desc") == 0 ? " DESC" : " ASC");
        }
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db.get(), query.c_str(), -1, &statement, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db.get()));
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(statement, &sqlite3_finalize);
    if (!search.empty()) {
        const auto pattern = SearchPattern(search);
        for (int i = 0; i < static_cast<int>(metadata.columns.size()); ++i) sqlite3_bind_text(stmt.get(), i + 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    }
    std::vector<TypedRow> rows;int result=SQLITE_OK;
    while((result=sqlite3_step(stmt.get()))==SQLITE_ROW){CheckCancellation(cancellation);TypedRow row;for(int column=0;column<sqlite3_column_count(stmt.get());++column)row[sqlite3_column_name(stmt.get(),column)]=SqliteValue(stmt.get(),column);rows.push_back(std::move(row));}
    CheckSqlite(db.get(),result,cancellation);CheckCancellation(cancellation);const auto text=ExportTypedRows(metadata.columns,rows,format,table,"sqlite");CheckCancellation(cancellation);return text;
}
std::string DbEngine::FormatQuery(const std::string& query, const std::string& kind) {
    if(kind=="document"||kind=="mongodb")return json::parse(query).dump(2);
    if(kind=="keyvalue"||kind=="redis"){
        const auto first=query.find_first_not_of(" \t\r\n");
        if(first==std::string::npos)return query;
        if(query[first]=='{')return json::parse(query).dump(2);
        auto result=query;for(size_t i=first;i<result.size()&&!isspace(static_cast<unsigned char>(result[i]));++i)result[i]=static_cast<char>(toupper(static_cast<unsigned char>(result[i])));return result;
    }
    static const std::set<std::string> keywords = {
        "SELECT", "FROM", "WHERE", "INSERT", "INTO", "UPDATE", "DELETE", "JOIN", "LEFT", "RIGHT", "INNER", "OUTER",
        "GROUP", "BY", "ORDER", "HAVING", "LIMIT", "OFFSET", "VALUES", "SET", "AND", "OR", "NOT", "IN", "AS", "ON",
        "CREATE", "TABLE", "DROP", "PRIMARY", "KEY", "FOREIGN", "REFERENCES", "CHECK", "DEFAULT", "CASCADE", "NULL"
    };
    std::string result;
    for (size_t pos = 0; pos < query.size();) {
        const size_t start = pos;
        if (query.compare(pos, 2, "--") == 0) {
            const auto end = query.find('\n', pos);
            pos = end == std::string::npos ? query.size() : end + 1;
        } else if (query.compare(pos, 2, "/*") == 0) {
            const auto end = query.find("*/", pos + 2);
            pos = end == std::string::npos ? query.size() : end + 2;
        } else if (query[pos] == '\'' || query[pos] == '"' || query[pos] == '`' || query[pos] == '[') {
            const char close = query[pos] == '[' ? ']' : query[pos];
            ++pos;
            while (pos < query.size()) {
                if (query[pos++] == close) {
                    if (pos < query.size() && query[pos] == close) { ++pos; continue; }
                    break;
                }
            }
        } else if (isalpha(static_cast<unsigned char>(query[pos])) || query[pos] == '_') {
            while (pos < query.size() && (isalnum(static_cast<unsigned char>(query[pos])) || query[pos] == '_')) ++pos;
            std::string token = query.substr(start, pos - start);
            std::string upper = token;
            std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(toupper(c)); });
            result += keywords.count(upper) ? upper : token;
            continue;
        } else ++pos;
        result += query.substr(start, pos - start);
    }
    return result;
}
QueryResult DbEngine::ExplainQuery(const std::string& dbId, const std::string& query, const std::atomic_bool* cancellation) {
    const auto p=connections_->Has(dbId)?connections_->Profile(dbId):ConnectionProfile{};
    if(p.type=="mongodb"||p.type=="redis"||!ProviderCapabilities("sql",p.readOnly,p.type).explain){QueryResult result;result.error="This provider does not support SQL EXPLAIN.";return result;}
    if(p.readOnly){try{RequireReadOnlySql(query);}catch(const std::exception& error){QueryResult result;result.error=error.what();return result;}}
    const auto explained=(p.type=="sqlite"?"EXPLAIN QUERY PLAN ":"EXPLAIN ")+query;
    return ExecuteQuery(dbId, explained,1000,cancellation);
}

} // namespace native_app
