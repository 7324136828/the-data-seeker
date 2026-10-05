#include "DbEngine.h"
#include <winsqlite/winsqlite3.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <chrono>
#include <algorithm>
#include <cctype>

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

DbEngine::DbEngine(const std::string& dataDir, bool seedSamples) : dataDir_(dataDir) {
    fs::create_directories(dataDir_);
    if (seedSamples) {
        InitSamples();
    }
}

std::string DbEngine::GetDbPath(const std::string& dbId) {
    auto it = databases_.find(dbId);
    if (it != databases_.end()) return it->second.path;
    return (fs::path(dataDir_) / (dbId + ".db")).string();
}

void DbEngine::InitSamples() {
    std::string ecomPath = (fs::path(dataDir_) / "ecommerce.db").string();
    bool ecomExists = fs::exists(ecomPath);
    DatabaseInfo ecom;
    ecom.id = "ecommerce";
    ecom.name = "E-Commerce Store (Sample)";
    ecom.path = ecomPath;
    ecom.isSample = true;
    databases_["ecommerce"] = ecom;
    if (!ecomExists) SeedEcommerce(ecomPath);

    std::string devPath = (fs::path(dataDir_) / "dev_studio.db").string();
    bool devExists = fs::exists(devPath);
    DatabaseInfo dev;
    dev.id = "dev_studio";
    dev.name = "Dev & API Metrics (Sample)";
    dev.path = devPath;
    dev.isSample = true;
    databases_["dev_studio"] = dev;
    if (!devExists) SeedDevStudio(devPath);

    // Discover any other .db files
    for (const auto& entry : fs::directory_iterator(dataDir_)) {
        if (entry.is_regular_file() && entry.path().extension() == ".db") {
            std::string stem = entry.path().stem().string();
            if (databases_.find(stem) == databases_.end()) {
                DatabaseInfo info;
                info.id = stem;
                std::string title = stem;
                std::replace(title.begin(), title.end(), '_', ' ');
                if (!title.empty()) title[0] = static_cast<char>(toupper(static_cast<unsigned char>(title[0])));
                info.name = title;
                info.path = entry.path().string();
                info.isSample = false;
                databases_[stem] = info;
            }
        }
    }
}

void DbEngine::SeedEcommerce(const std::string& path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) return;

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
    sqlite3_exec(db, ddl, nullptr, nullptr, &errMsg);
    if (errMsg) sqlite3_free(errMsg);
    sqlite3_close(db);
}

void DbEngine::SeedDevStudio(const std::string& path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) return;

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
    sqlite3_exec(db, ddl, nullptr, nullptr, &errMsg);
    if (errMsg) sqlite3_free(errMsg);
    sqlite3_close(db);
}

std::vector<DatabaseInfo> DbEngine::ListDatabases() {
    std::vector<DatabaseInfo> list;
    for (auto& pair : databases_) {
        DatabaseInfo info = pair.second;
        if (fs::exists(info.path)) {
            info.sizeBytes = fs::file_size(info.path);
        }
        // Count tables
        sqlite3* db = nullptr;
        if (sqlite3_open(info.path.c_str(), &db) == SQLITE_OK) {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%';", -1, &stmt, nullptr) == SQLITE_OK) {
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    info.tableCount = sqlite3_column_int(stmt, 0);
                }
                sqlite3_finalize(stmt);
            }
            sqlite3_close(db);
        }
        list.push_back(info);
    }
    return list;
}

DatabaseInfo DbEngine::CreateDatabase(const std::string& name, const std::string& preset) {
    if (name.empty()) throw std::runtime_error("Database name cannot be empty.");
    std::string slug;
    for (char c : name) {
        if (isalnum(static_cast<unsigned char>(c))) slug += static_cast<char>(tolower(static_cast<unsigned char>(c)));
        else if (c == ' ' || c == '-' || c == '_') slug += '_';
    }
    while (!slug.empty() && slug.back() == '_') slug.pop_back();
    if (slug.empty()) slug = "database";

    std::string dbId = slug;
    int suffix = 2;
    while (databases_.find(dbId) != databases_.end() || fs::exists(fs::path(dataDir_) / (dbId + ".db"))) {
        dbId = slug + "_" + std::to_string(suffix++);
    }

    std::string dbPath = (fs::path(dataDir_) / (dbId + ".db")).string();
    if (preset == "ecommerce") {
        SeedEcommerce(dbPath);
    } else if (preset == "dev_studio") {
        SeedDevStudio(dbPath);
    } else {
        sqlite3* db = nullptr;
        if (sqlite3_open(dbPath.c_str(), &db) == SQLITE_OK) {
            sqlite3_close(db);
        }
    }

    DatabaseInfo info;
    info.id = dbId;
    info.name = name;
    info.path = dbPath;
    info.isSample = false;
    databases_[dbId] = info;
    return info;
}

bool DbEngine::DeleteDatabase(const std::string& dbId) {
    auto it = databases_.find(dbId);
    if (it == databases_.end()) return false;
    if (it->second.isSample) throw std::runtime_error("Sample databases cannot be deleted.");

    std::string path = it->second.path;
    databases_.erase(it);
    std::error_code ec;
    fs::remove(path, ec);
    return true;
}

DatabaseSchema DbEngine::GetSchema(const std::string& dbId) {
    DatabaseSchema schema;
    schema.databaseId = dbId;
    auto it = databases_.find(dbId);
    schema.databaseName = (it != databases_.end()) ? it->second.name : dbId;

    std::string dbPath = GetDbPath(dbId);
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return schema;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT type, name, sql FROM sqlite_master WHERE type IN ('table', 'view') AND name NOT LIKE 'sqlite_%' ORDER BY name;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            TableMeta tbl;
            tbl.type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            tbl.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const char* ddlPtr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            tbl.ddl = ddlPtr ? ddlPtr : "";
            tbl.editable = (tbl.type == "table");

            // Columns
            std::string pragmaCol = "PRAGMA table_info(" + QuoteIdentifier(tbl.name) + ");";
            sqlite3_stmt* cStmt = nullptr;
            if (sqlite3_prepare_v2(db, pragmaCol.c_str(), -1, &cStmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(cStmt) == SQLITE_ROW) {
                    ColumnMeta col;
                    col.cid = sqlite3_column_int(cStmt, 0);
                    col.name = reinterpret_cast<const char*>(sqlite3_column_text(cStmt, 1));
                    const char* typePtr = reinterpret_cast<const char*>(sqlite3_column_text(cStmt, 2));
                    col.type = typePtr ? typePtr : "TEXT";
                    col.notnull = (sqlite3_column_int(cStmt, 3) != 0);
                    const char* dfltPtr = reinterpret_cast<const char*>(sqlite3_column_text(cStmt, 4));
                    col.dfltValue = dfltPtr ? dfltPtr : "";
                    col.pk = (sqlite3_column_int(cStmt, 5) != 0);
                    if (col.pk) tbl.primaryKeys.push_back(col.name);
                    tbl.columns.push_back(col);
                }
                sqlite3_finalize(cStmt);
            }

            // Foreign keys
            std::string pragmaFk = "PRAGMA foreign_key_list(" + QuoteIdentifier(tbl.name) + ");";
            sqlite3_stmt* fStmt = nullptr;
            if (sqlite3_prepare_v2(db, pragmaFk.c_str(), -1, &fStmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(fStmt) == SQLITE_ROW) {
                    ForeignKeyMeta fk;
                    fk.id = sqlite3_column_int(fStmt, 0);
                    fk.seq = sqlite3_column_int(fStmt, 1);
                    fk.targetTable = reinterpret_cast<const char*>(sqlite3_column_text(fStmt, 2));
                    fk.fromColumn = reinterpret_cast<const char*>(sqlite3_column_text(fStmt, 3));
                    fk.toColumn = reinterpret_cast<const char*>(sqlite3_column_text(fStmt, 4));
                    const char* onUp = reinterpret_cast<const char*>(sqlite3_column_text(fStmt, 5));
                    fk.onUpdate = onUp ? onUp : "NO ACTION";
                    const char* onDel = reinterpret_cast<const char*>(sqlite3_column_text(fStmt, 6));
                    fk.onDelete = onDel ? onDel : "NO ACTION";
                    tbl.foreignKeys.push_back(fk);
                }
                sqlite3_finalize(fStmt);
            }

            // Indexes
            std::string pragmaIdx = "PRAGMA index_list(" + QuoteIdentifier(tbl.name) + ");";
            sqlite3_stmt* iStmt = nullptr;
            if (sqlite3_prepare_v2(db, pragmaIdx.c_str(), -1, &iStmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(iStmt) == SQLITE_ROW) {
                    IndexMeta idx;
                    idx.name = reinterpret_cast<const char*>(sqlite3_column_text(iStmt, 1));
                    idx.unique = (sqlite3_column_int(iStmt, 2) != 0);
                    const char* originPtr = reinterpret_cast<const char*>(sqlite3_column_text(iStmt, 3));
                    idx.origin = originPtr ? originPtr : "";

                    std::string pragmaIdxInfo = "PRAGMA index_info(" + QuoteIdentifier(idx.name) + ");";
                    sqlite3_stmt* iiStmt = nullptr;
                    if (sqlite3_prepare_v2(db, pragmaIdxInfo.c_str(), -1, &iiStmt, nullptr) == SQLITE_OK) {
                        while (sqlite3_step(iiStmt) == SQLITE_ROW) {
                            idx.columns.push_back(reinterpret_cast<const char*>(sqlite3_column_text(iiStmt, 2)));
                        }
                        sqlite3_finalize(iiStmt);
                    }
                    tbl.indexes.push_back(idx);
                }
                sqlite3_finalize(iStmt);
            }

            // Row count
            if (tbl.type == "table") {
                std::string countSql = "SELECT COUNT(*) FROM " + QuoteIdentifier(tbl.name) + ";";
                sqlite3_stmt* cntStmt = nullptr;
                if (sqlite3_prepare_v2(db, countSql.c_str(), -1, &cntStmt, nullptr) == SQLITE_OK) {
                    if (sqlite3_step(cntStmt) == SQLITE_ROW) {
                        tbl.rowCount = sqlite3_column_int64(cntStmt, 0);
                    }
                    sqlite3_finalize(cntStmt);
                }
            }

            schema.tables.push_back(tbl);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return schema;
}

ERDiagram DbEngine::GetErDiagram(const std::string& dbId) {
    DatabaseSchema schema = GetSchema(dbId);
    ERDiagram er;
    er.databaseId = dbId;

    for (const auto& tbl : schema.tables) {
        if (tbl.type != "table") continue;
        ERNode node;
        node.id = tbl.name;
        node.name = tbl.name;
        node.columns = tbl.columns;
        node.rowCount = tbl.rowCount;
        er.nodes.push_back(node);

        for (const auto& fk : tbl.foreignKeys) {
            ERLink link;
            link.id = tbl.name + "." + fk.fromColumn + "->" + fk.targetTable + "." + fk.toColumn;
            link.source = tbl.name;
            link.sourceCol = fk.fromColumn;
            link.target = fk.targetTable;
            link.targetCol = fk.toColumn;
            link.onUpdate = fk.onUpdate;
            link.onDelete = fk.onDelete;
            er.links.push_back(link);
        }
    }
    return er;
}

QueryResult DbEngine::ExecuteQuery(const std::string& dbId, const std::string& sql, int maxRows) {
    QueryResult qr;
    qr.query = sql;
    auto start = std::chrono::high_resolution_clock::now();

    std::string dbPath = GetDbPath(dbId);
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        qr.error = db ? sqlite3_errmsg(db) : "Could not open database file.";
        if (db) sqlite3_close(db);
        return qr;
    }

    // Split statements
    const char* pLeft = sql.c_str();
    sqlite3_stmt* stmt = nullptr;
    int totalAffected = 0;

    while (*pLeft) {
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
        if (colCount > 0) {
            qr.columns.clear();
            qr.rows.clear();
            for (int i = 0; i < colCount; ++i) {
                qr.columns.push_back(sqlite3_column_name(stmt, i));
            }

            int count = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (count < maxRows) {
                    std::map<std::string, std::string> row;
                    for (int i = 0; i < colCount; ++i) {
                        const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                        row[qr.columns[i]] = val ? val : "NULL";
                    }
                    qr.rows.push_back(row);
                } else {
                    qr.truncated = true;
                }
                count++;
            }
            qr.rowCount = static_cast<int64_t>(qr.rows.size());
        }

        int changes = sqlite3_changes(db);
        if (changes > 0) totalAffected += changes;

        sqlite3_finalize(stmt);
        pLeft = pTail;
    }

    qr.rowsAffected = totalAffected;
    auto end = std::chrono::high_resolution_clock::now();
    qr.executionTimeMs = std::chrono::duration<double, std::milli>(end - start).count();

    sqlite3_close(db);
    return qr;
}

TableDataResult DbEngine::GetTableData(
    const std::string& dbId,
    const std::string& table,
    int page,
    int pageSize,
    const std::string& sortCol,
    const std::string& sortDir,
    const std::string& search,
    const std::string& filterCol,
    const std::string& filterVal
) {
    TableDataResult res;
    res.table = table;
    res.page = std::max(1, page);
    res.pageSize = std::max(1, std::min(200, pageSize));

    std::string dbPath = GetDbPath(dbId);
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        res.error = "Could not open database.";
        if (db) sqlite3_close(db);
        return res;
    }

    // Inspect columns
    std::string pragmaCol = "PRAGMA table_info(" + QuoteIdentifier(table) + ");";
    sqlite3_stmt* cStmt = nullptr;
    if (sqlite3_prepare_v2(db, pragmaCol.c_str(), -1, &cStmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(cStmt) == SQLITE_ROW) {
            ColumnMeta col;
            col.cid = sqlite3_column_int(cStmt, 0);
            col.name = reinterpret_cast<const char*>(sqlite3_column_text(cStmt, 1));
            const char* typePtr = reinterpret_cast<const char*>(sqlite3_column_text(cStmt, 2));
            col.type = typePtr ? typePtr : "TEXT";
            col.pk = (sqlite3_column_int(cStmt, 5) != 0);
            if (col.pk) res.primaryKeys.push_back(col.name);
            res.columns.push_back(col.name);
            res.columnMetadata.push_back(col);
        }
        sqlite3_finalize(cStmt);
    }

    std::string whereClause;
    std::vector<std::string> bindParams;

    if (!filterCol.empty() && !filterVal.empty()) {
        whereClause = " WHERE CAST(" + QuoteIdentifier(filterCol) + " AS TEXT) LIKE ?";
        bindParams.push_back("%" + filterVal + "%");
    } else if (!search.empty() && !res.columns.empty()) {
        whereClause = " WHERE (";
        for (size_t i = 0; i < res.columns.size(); ++i) {
            if (i > 0) whereClause += " OR ";
            whereClause += "CAST(" + QuoteIdentifier(res.columns[i]) + " AS TEXT) LIKE ?";
            bindParams.push_back("%" + search + "%");
        }
        whereClause += ")";
    }

    // Count
    std::string countSql = "SELECT COUNT(*) FROM " + QuoteIdentifier(table) + whereClause + ";";
    sqlite3_stmt* cntStmt = nullptr;
    if (sqlite3_prepare_v2(db, countSql.c_str(), -1, &cntStmt, nullptr) == SQLITE_OK) {
        for (size_t i = 0; i < bindParams.size(); ++i) {
            sqlite3_bind_text(cntStmt, static_cast<int>(i + 1), bindParams[i].c_str(), -1, SQLITE_TRANSIENT);
        }
        if (sqlite3_step(cntStmt) == SQLITE_ROW) {
            res.totalRows = sqlite3_column_int64(cntStmt, 0);
        }
        sqlite3_finalize(cntStmt);
    }

    res.totalPages = std::max(1, static_cast<int>((res.totalRows + res.pageSize - 1) / res.pageSize));
    res.page = std::min(res.totalPages, std::max(1, res.page));
    int offset = (res.page - 1) * res.pageSize;

    std::string orderClause;
    if (!sortCol.empty()) {
        std::string dir = (_stricmp(sortDir.c_str(), "desc") == 0) ? "DESC" : "ASC";
        orderClause = " ORDER BY " + QuoteIdentifier(sortCol) + " " + dir;
    } else if (!res.primaryKeys.empty()) {
        orderClause = " ORDER BY " + QuoteIdentifier(res.primaryKeys[0]) + " ASC";
    }

    std::string selectSql = "SELECT * FROM " + QuoteIdentifier(table) + whereClause + orderClause + " LIMIT ? OFFSET ?;";
    sqlite3_stmt* sStmt = nullptr;
    if (sqlite3_prepare_v2(db, selectSql.c_str(), -1, &sStmt, nullptr) == SQLITE_OK) {
        int paramIdx = 1;
        for (size_t i = 0; i < bindParams.size(); ++i) {
            sqlite3_bind_text(sStmt, paramIdx++, bindParams[i].c_str(), -1, SQLITE_TRANSIENT);
        }
        sqlite3_bind_int(sStmt, paramIdx++, res.pageSize);
        sqlite3_bind_int(sStmt, paramIdx++, offset);

        int colCount = sqlite3_column_count(sStmt);
        while (sqlite3_step(sStmt) == SQLITE_ROW) {
            std::map<std::string, std::string> row;
            for (int i = 0; i < colCount; ++i) {
                const char* cName = sqlite3_column_name(sStmt, i);
                const char* val = reinterpret_cast<const char*>(sqlite3_column_text(sStmt, i));
                row[cName] = val ? val : "NULL";
            }
            res.rows.push_back(row);
        }
        sqlite3_finalize(sStmt);
    } else {
        res.error = sqlite3_errmsg(db);
    }

    sqlite3_close(db);
    return res;
}

bool DbEngine::InsertTableRow(const std::string& dbId, const std::string& table, const std::map<std::string, std::string>& rowData) {
    std::string dbPath = GetDbPath(dbId);
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }

    std::string cols;
    std::string vals;
    std::vector<std::string> params;

    for (const auto& pair : rowData) {
        if (!cols.empty()) { cols += ", "; vals += ", "; }
        cols += QuoteIdentifier(pair.first);
        vals += "?";
        params.push_back(pair.second);
    }

    std::string sql = "INSERT INTO " + QuoteIdentifier(table) + " (" + cols + ") VALUES (" + vals + ");";
    sqlite3_stmt* stmt = nullptr;
    bool success = false;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        for (size_t i = 0; i < params.size(); ++i) {
            sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
        }
        success = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
    return success;
}

bool DbEngine::UpdateTableRow(const std::string& dbId, const std::string& table, const std::map<std::string, std::string>& pkData, const std::map<std::string, std::string>& rowData) {
    std::string dbPath = GetDbPath(dbId);
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }

    std::string setClause;
    std::vector<std::string> params;
    for (const auto& pair : rowData) {
        if (!setClause.empty()) setClause += ", ";
        setClause += QuoteIdentifier(pair.first) + " = ?";
        params.push_back(pair.second);
    }

    std::string whereClause;
    for (const auto& pair : pkData) {
        if (!whereClause.empty()) whereClause += " AND ";
        whereClause += QuoteIdentifier(pair.first) + " = ?";
        params.push_back(pair.second);
    }

    std::string sql = "UPDATE " + QuoteIdentifier(table) + " SET " + setClause + " WHERE " + whereClause + ";";
    sqlite3_stmt* stmt = nullptr;
    bool success = false;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        for (size_t i = 0; i < params.size(); ++i) {
            sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
        }
        success = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
    return success;
}

bool DbEngine::DeleteTableRow(const std::string& dbId, const std::string& table, const std::map<std::string, std::string>& pkData) {
    std::string dbPath = GetDbPath(dbId);
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }

    std::string whereClause;
    std::vector<std::string> params;
    for (const auto& pair : pkData) {
        if (!whereClause.empty()) whereClause += " AND ";
        whereClause += QuoteIdentifier(pair.first) + " = ?";
        params.push_back(pair.second);
    }

    std::string sql = "DELETE FROM " + QuoteIdentifier(table) + " WHERE " + whereClause + ";";
    sqlite3_stmt* stmt = nullptr;
    bool success = false;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        for (size_t i = 0; i < params.size(); ++i) {
            sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
        }
        success = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
    return success;
}

std::string DbEngine::ExportTableData(
    const std::string& dbId,
    const std::string& table,
    const std::string& format,
    const std::string& search,
    const std::string& sortCol,
    const std::string& sortDir
) {
    TableDataResult data = GetTableData(dbId, table, 1, 100000, sortCol, sortDir, search);
    if (format == "csv") {
        std::ostringstream ss;
        for (size_t i = 0; i < data.columns.size(); ++i) {
            if (i > 0) ss << ",";
            ss << "\"" << data.columns[i] << "\"";
        }
        ss << "\r\n";
        for (const auto& row : data.rows) {
            for (size_t i = 0; i < data.columns.size(); ++i) {
                if (i > 0) ss << ",";
                auto it = row.find(data.columns[i]);
                std::string v = (it != row.end()) ? it->second : "";
                ss << "\"";
                for (char c : v) {
                    if (c == '"') ss << "\"\"";
                    else ss << c;
                }
                ss << "\"";
            }
            ss << "\r\n";
        }
        return ss.str();
    } else if (format == "sql") {
        std::ostringstream ss;
        for (const auto& row : data.rows) {
            ss << "INSERT INTO " << QuoteIdentifier(table) << " (";
            for (size_t i = 0; i < data.columns.size(); ++i) {
                if (i > 0) ss << ", ";
                ss << QuoteIdentifier(data.columns[i]);
            }
            ss << ") VALUES (";
            for (size_t i = 0; i < data.columns.size(); ++i) {
                if (i > 0) ss << ", ";
                auto it = row.find(data.columns[i]);
                std::string v = (it != row.end()) ? it->second : "NULL";
                if (v == "NULL") {
                    ss << "NULL";
                } else {
                    ss << "'";
                    for (char c : v) {
                        if (c == '\'') ss << "''";
                        else ss << c;
                    }
                    ss << "'";
                }
            }
            ss << ");\r\n";
        }
        return ss.str();
    } else { // JSON
        json arr = json::array();
        for (const auto& row : data.rows) {
            json item = json::object();
            for (const auto& pair : row) {
                item[pair.first] = pair.second;
            }
            arr.push_back(item);
        }
        return arr.dump(2);
    }
}

std::string DbEngine::FormatQuery(const std::string& query, const std::string& /*kind*/) {
    // Quick SQL keyword capitalizer and formatter
    static const std::vector<std::string> keywords = {
        "SELECT", "FROM", "WHERE", "INSERT INTO", "UPDATE", "DELETE FROM",
        "JOIN", "LEFT JOIN", "RIGHT JOIN", "INNER JOIN", "OUTER JOIN",
        "GROUP BY", "ORDER BY", "HAVING", "LIMIT", "OFFSET", "VALUES", "SET",
        "AND", "OR", "NOT", "IN", "AS", "ON", "CREATE TABLE", "DROP TABLE",
        "PRIMARY KEY", "FOREIGN KEY", "REFERENCES", "CHECK", "DEFAULT", "CASCADE"
    };

    std::string formatted = query;
    for (const auto& kw : keywords) {
        std::regex re(R"(\b)" + kw + R"(\b)", std::regex_constants::icase);
        formatted = std::regex_replace(formatted, re, kw);
    }
    return formatted;
}

QueryResult DbEngine::ExplainQuery(const std::string& dbId, const std::string& query) {
    return ExecuteQuery(dbId, "EXPLAIN QUERY PLAN " + query);
}

} // namespace native_app
