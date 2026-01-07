#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include <nlohmann/json.hpp>

#include "common/crypto.hpp"

namespace fs = std::filesystem;

namespace {

struct Question {
  std::string text;
  nlohmann::json options;
  std::string correct;
  std::string difficulty;
  std::string topic;
};

struct UserSeed {
  std::string username;
  std::string pass_hash;  // placeholder hash; replace with real hash later.
  std::string role;
  std::string full_name;
  std::string email;
};

void check_sql(int rc, sqlite3* db, const char* ctx) {
  if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
    std::string msg = sqlite3_errmsg(db);
    throw std::runtime_error(std::string(ctx) + ": " + msg);
  }
}

void exec_file(sqlite3* db, const fs::path& path) {
  FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f) throw std::runtime_error("Cannot open schema file: " + path.string());
  std::string sql;
  char buf[4096];
  while (size_t n = std::fread(buf, 1, sizeof(buf), f)) {
    sql.append(buf, n);
  }
  std::fclose(f);
  char* errmsg = nullptr;
  int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errmsg);
  if (rc != SQLITE_OK) {
    std::string msg = errmsg ? errmsg : "";
    sqlite3_free(errmsg);
    throw std::runtime_error("Schema exec failed: " + msg);
  }
}

void seed_users(sqlite3* db, const std::vector<UserSeed>& users) {
  const char* sql = "INSERT OR IGNORE INTO users(username, pass_hash, role, full_name, email, created_at) "
                    "VALUES(?,?,?,?,?, strftime('%s','now'));";
  sqlite3_stmt* stmt = nullptr;
  check_sql(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db, "prepare users");
  for (const auto& u : users) {
    sqlite3_reset(stmt);
    sqlite3_bind_text(stmt, 1, u.username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, u.pass_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, u.role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, u.full_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, u.email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
}

void seed_questions(sqlite3* db, const std::vector<Question>& questions) {
  const char* sql = "INSERT INTO questions(text, options_json, correct_option, difficulty, topic, created_at) "
                    "VALUES(?,?,?,?,?, strftime('%s','now'));";
  sqlite3_stmt* stmt = nullptr;
  check_sql(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db, "prepare questions");
  for (const auto& q : questions) {
    sqlite3_reset(stmt);
    auto opts = q.options.dump();
    sqlite3_bind_text(stmt, 1, q.text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, opts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, q.correct.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, q.difficulty.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, q.topic.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
      std::cerr << "Skip question: " << sqlite3_errmsg(db) << "\n";
    }
  }
  sqlite3_finalize(stmt);
}

std::optional<int> count_table(sqlite3* db, std::string_view table) {
  std::string sql = "SELECT COUNT(*) FROM " + std::string(table) + ";";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }
  int count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

void print_sample(sqlite3* db, std::string_view difficulty) {
  std::string sql = "SELECT id, text, topic FROM questions WHERE difficulty = ? LIMIT 1;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return;
  sqlite3_bind_text(stmt, 1, std::string(difficulty).c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    int id = sqlite3_column_int(stmt, 0);
    const unsigned char* txt = sqlite3_column_text(stmt, 1);
    const unsigned char* topic = sqlite3_column_text(stmt, 2);
    std::cout << "Sample " << difficulty << ": [" << id << "] "
              << (txt ? reinterpret_cast<const char*>(txt) : "")
              << " (topic " << (topic ? reinterpret_cast<const char*>(topic) : "") << ")\n";
  }
  sqlite3_finalize(stmt);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    bool reset = false;
    fs::path db_path = "data/quiz.db";
    fs::path schema_path = "data/schema.sql";
    if (argc > 1) db_path = argv[1];
    if (argc > 2) schema_path = argv[2];
    if (argc > 3 && std::string(argv[3]) == "--reset") reset = true;

    if (reset && fs::exists(db_path)) {
      fs::remove(db_path);
    }

    fs::create_directories(db_path.parent_path().empty() ? "." : db_path.parent_path());

    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) {
      throw std::runtime_error("Cannot open db: " + std::string(sqlite3_errmsg(db)));
    }

    exec_file(db, schema_path);

    // Start transaction for performance and consistency.
    check_sql(sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr), db, "begin tx");

    std::vector<UserSeed> users = {
        {"teacher", quiz::hash_password("teacher123"), "ADMIN", "Teacher Account", "teacher@example.com"},
        {"student1", quiz::hash_password("student123"), "STUDENT", "Student One", "s1@example.com"},
        {"student2", quiz::hash_password("student123"), "STUDENT", "Student Two", "s2@example.com"},
    };

  std::vector<Question> questions = {
        {"What is TCP used for?",
         {{"A", "Connection-oriented reliable transport"},
          {"B", "Connectionless unreliable transport"},
          {"C", "Routing decisions"},
          {"D", "Link-layer framing"}},
         "A", "EASY", "Networking"},
        {"Which layer handles end-to-end reliability?",
         {{"A", "Application"}, {"B", "Transport"}, {"C", "Network"}, {"D", "Physical"}},
         "B", "EASY", "Networking"},
        {"What is a SYN flood?",
         {{"A", "Authentication attack"},
          {"B", "TCP connection exhaustion"},
          {"C", "Buffer overflow"},
          {"D", "DNS cache poisoning"}},
         "B", "MEDIUM", "Security"},
        {"Select the correct subnet mask for /27",
         {{"A", "255.255.255.224"}, {"B", "255.255.255.240"}, {"C", "255.255.255.248"}, {"D", "255.255.255.192"}},
         "A", "MEDIUM", "IP"},
        {"Which algorithm is used in TLS for key exchange (commonly)?",
         {{"A", "RSA"}, {"B", "Diffie-Hellman/ECDHE"}, {"C", "AES"}, {"D", "ChaCha"}},
         "B", "HARD", "Security"},
        {"Explain purpose of congestion control in TCP.",
         {{"A", "Detect bit errors"},
          {"B", "Avoid overwhelming network paths"},
          {"C", "Encrypt payload"},
          {"D", "Assign IP addresses"}},
         "B", "HARD", "Networking"},
        // --- thêm nhiều câu hỏi ---
        {"What does DNS translate?",
         {{"A", "IP to MAC"}, {"B", "Domain to IP"}, {"C", "MAC to IP"}, {"D", "URL to MAC"}},
         "B", "EASY", "DNS"},
        {"Default HTTP port?",
         {{"A", "21"}, {"B", "53"}, {"C", "80"}, {"D", "110"}},
         "C", "EASY", "HTTP"},
        {"HTTPS adds which layer?",
         {{"A", "TCP"}, {"B", "TLS"}, {"C", "IPSec"}, {"D", "SSH"}},
         "B", "EASY", "Security"},
        {"Which protocol is connectionless?",
         {{"A", "TCP"}, {"B", "UDP"}, {"C", "SCTP"}, {"D", "HTTP"}},
         "B", "EASY", "Transport"},
        {"OSI layer for routing?",
         {{"A", "Network"}, {"B", "Transport"}, {"C", "Link"}, {"D", "Application"}},
         "A", "EASY", "OSI"},
        {"ARP resolves?",
         {{"A", "IP to MAC"}, {"B", "MAC to IP"}, {"C", "DNS to IP"}, {"D", "URL to MAC"}},
         "A", "EASY", "ARP"},
        {"ICMP is used for?",
         {{"A", "Routing tables"}, {"B", "Diagnostics/Errors"}, {"C", "DHCP"}, {"D", "HTTP"}},
         "B", "EASY", "ICMP"},
        {"NAT main purpose?",
         {{"A", "Encryption"}, {"B", "Port forwarding"}, {"C", "Address translation"}, {"D", "DHCP lease"}},
         "C", "EASY", "NAT"},
        {"Subnet /24 has how many usable hosts?",
         {{"A", "254"}, {"B", "256"}, {"C", "512"}, {"D", "1022"}},
         "A", "EASY", "IP"},
        {"CIDR of 255.255.255.0?",
         {{"A", "/16"}, {"B", "/24"}, {"C", "/25"}, {"D", "/26"}},
         "B", "EASY", "IP"},
        {"TCP three-way handshake order?",
         {{"A", "SYN-ACK-SYN"}, {"B", "ACK-SYN-SYN"}, {"C", "SYN-SYN/ACK-ACK"}, {"D", "SYN-ACK-ACK"}},
         "C", "MEDIUM", "TCP"},
        {"What does RTT stand for?",
         {{"A", "Round Trip Time"}, {"B", "Real Time Transfer"}, {"C", "Route Transit Time"}, {"D", "Random Transit Time"}},
         "A", "MEDIUM", "TCP"},
        {"Slow start does what?",
         {{"A", "Increase cwnd exponentially"}, {"B", "Reduce RTT"}, {"C", "Encrypt segments"}, {"D", "Drop packets"}},
         "A", "MEDIUM", "TCP"},
        {"Window scaling used when?",
         {{"A", "Small buffers"}, {"B", "High BDP links"}, {"C", "Short RTT"}, {"D", "UDP only"}},
         "B", "MEDIUM", "TCP"},
        {"DHCP handover uses which message first?",
         {{"A", "DISCOVER"}, {"B", "OFFER"}, {"C", "REQUEST"}, {"D", "ACK"}},
         "A", "MEDIUM", "DHCP"},
        {"HTTPS default port?",
         {{"A", "443"}, {"B", "8443"}, {"C", "22"}, {"D", "8080"}},
         "A", "MEDIUM", "HTTP"},
        {"REST typically uses?",
         {{"A", "SOAP"}, {"B", "HTTP verbs + JSON"}, {"C", "FTP"}, {"D", "MQTT"}},
         "B", "MEDIUM", "HTTP"},
        {"CDN stands for?",
         {{"A", "Content Delivery Network"}, {"B", "Control Data Node"}, {"C", "Cache Delivery Node"}, {"D", "Content Data Network"}},
         "A", "MEDIUM", "Web"},
        {"AJAX allows?",
         {{"A", "Page reload"}, {"B", "Async HTTP/JS"}, {"C", "Server push only"}, {"D", "Binary only"}},
         "B", "MEDIUM", "Web"},
        {"WebSocket advantage?",
         {{"A", "Stateless"}, {"B", "Full-duplex"}, {"C", "UDP only"}, {"D", "No handshake"}},
         "B", "MEDIUM", "Web"},
        {"TLS provides?",
         {{"A", "Integrity + Confidentiality"}, {"B", "Routing"}, {"C", "Compression only"}, {"D", "DHCP lease"}},
         "A", "MEDIUM", "Security"},
        {"HSTS does what?",
         {{"A", "Force HTTPS"}, {"B", "Disable TLS"}, {"C", "Allow only HTTP/1"}, {"D", "Disable cookies"}},
         "A", "MEDIUM", "Security"},
        {"Common DDoS vector?",
         {{"A", "SYN flood"}, {"B", "DNS caching"}, {"C", "FTP bounce"}, {"D", "ARP reply"}},
         "A", "MEDIUM", "Security"},
        {"VPN tunnel encapsulates?",
         {{"A", "IP in IP"}, {"B", "Only TCP"}, {"C", "Only UDP"}, {"D", "ICMP only"}},
         "A", "MEDIUM", "VPN"},
        {"BGP used for?",
         {{"A", "Interior routing"}, {"B", "Exterior routing"}, {"C", "Link discovery"}, {"D", "HTTP proxy"}},
         "B", "HARD", "Routing"},
        {"OSPF uses what metric?",
         {{"A", "Hop count"}, {"B", "Cost/ bandwidth"}, {"C", "Latency only"}, {"D", "Random"}},
         "B", "HARD", "Routing"},
        {"Spanning Tree prevents?",
         {{"A", "Loops at L2"}, {"B", "BGP oscillation"}, {"C", "DHCP starvation"}, {"D", "SYN flood"}},
         "A", "HARD", "Switching"},
        {"What is MPLS label used for?",
         {{"A", "Routing decision instead of IP lookup"}, {"B", "DNS caching"}, {"C", "TLS negotiation"}, {"D", "DHCP relay"}},
         "A", "HARD", "MPLS"},
        {"HTTP/2 key feature?",
         {{"A", "Head-of-line blocking"}, {"B", "Multiplexing over one TCP"}, {"C", "Only text format"}, {"D", "No TLS"}},
         "B", "HARD", "HTTP"},
        {"QUIC built on?",
         {{"A", "TCP"}, {"B", "UDP"}, {"C", "ICMP"}, {"D", "SCTP"}},
         "B", "HARD", "Transport"},
        {"Which is stateful firewall tracking?",
         {{"A", "Connection table"}, {"B", "MAC learning"}, {"C", "DNS cache"}, {"D", "NAT pool"}},
         "A", "HARD", "Security"},
        {"JWT used for?",
         {{"A", "Session token"}, {"B", "Routing"}, {"C", "ARP"}, {"D", "DNS"}},
         "A", "MEDIUM", "Web"},
        {"CSRF mitigated by?",
         {{"A", "SameSite cookies"}, {"B", "ARP cache"}, {"C", "TTL"}, {"D", "RST packet"}},
         "A", "MEDIUM", "Security"},
        {"XSS mitigated by?",
         {{"A", "Input validation + output encoding"}, {"B", "BGP"}, {"C", "SYN cookies"}, {"D", "MAC filtering"}},
         "A", "MEDIUM", "Security"},
        {"FTP active mode uses?",
         {{"A", "PORT command, server connects back"}, {"B", "PASV only"}, {"C", "UDP"}, {"D", "SSH tunnel mandatory"}},
         "A", "MEDIUM", "FTP"},
        {"SMTP default port?",
         {{"A", "25"}, {"B", "110"}, {"C", "143"}, {"D", "993"}},
         "A", "EASY", "SMTP"},
        {"IMAP secure port?",
         {{"A", "433"}, {"B", "993"}, {"C", "995"}, {"D", "25"}},
         "B", "EASY", "Mail"},
        {"POP3 secure port?",
         {{"A", "995"}, {"B", "993"}, {"C", "110"}, {"D", "25"}},
         "A", "EASY", "Mail"},
        {"DHCP assigns?",
         {{"A", "MAC"}, {"B", "IP + mask + gateway + DNS"}, {"C", "TLS cert"}, {"D", "BGP ASN"}},
         "B", "EASY", "DHCP"},
        {"Traceroute uses?",
         {{"A", "TTL expiration"}, {"B", "MAC flooding"}, {"C", "RST"}, {"D", "HSTS"}},
         "A", "EASY", "ICMP"},
        {"Ping uses?",
         {{"A", "ICMP Echo"}, {"B", "UDP"}, {"C", "TCP SYN"}, {"D", "HTTP GET"}},
         "A", "EASY", "ICMP"},
        {"Link-local IPv6 prefix?",
         {{"A", "fe80::/10"}, {"B", "ff00::/8"}, {"C", "2001::/16"}, {"D", "fc00::/7"}},
         "A", "MEDIUM", "IPv6"},
        {"IPv6 multicast prefix?",
         {{"A", "ff00::/8"}, {"B", "fe80::/10"}, {"C", "2001::/16"}, {"D", "fc00::/7"}},
         "A", "MEDIUM", "IPv6"},
        {"Private IPv4 ranges?",
         {{"A", "10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16"}, {"B", "8.8.8.0/24"}, {"C", "1.1.1.0/24"}, {"D", "100.64.0.0/10"}},
         "A", "EASY", "IP"},
        {"What is MTU?",
         {{"A", "Max Transmission Unit"}, {"B", "Min Transfer Unit"}, {"C", "Media Type Unit"}, {"D", "Multi Transfer Unit"}},
         "A", "EASY", "Link"},
        {"Jumbo frame size approx?",
         {{"A", "9000 bytes"}, {"B", "1500 bytes"}, {"C", "64 bytes"}, {"D", "4096 bytes"}},
         "A", "MEDIUM", "Link"},
        {"VLAN trunk uses?",
         {{"A", "802.1Q tag"}, {"B", "ARP"}, {"C", "ICMP"}, {"D", "RST"}},
         "A", "MEDIUM", "VLAN"},
        {"802.11 uses which band?",
         {{"A", "2.4/5 GHz"}, {"B", "900 MHz only"}, {"C", "28 GHz"}, {"D", "60 GHz only"}},
         "A", "EASY", "WiFi"},
        {"Hidden SSID mitigates?",
         {{"A", "Nothing significant"}, {"B", "All attacks"}, {"C", "WPA3 requirement"}, {"D", "DFS"}},
         "A", "MEDIUM", "WiFi"},
    };

    auto user_count_pre = count_table(db, "users").value_or(-1);
    auto question_count_pre = count_table(db, "questions").value_or(-1);

    if (user_count_pre == 0) {
      seed_users(db, users);
    } else if (user_count_pre > 0) {
      std::cout << "Users already present (" << user_count_pre << "), skipping user seed.\n";
    } else {
      std::cout << "Could not count users (table missing?), skipping user seed.\n";
    }

    if (question_count_pre == 0) {
      seed_questions(db, questions);
    } else if (question_count_pre > 0) {
      std::cout << "Questions already present (" << question_count_pre << "), skipping question seed.\n";
    } else {
      std::cout << "Could not count questions (table missing?), skipping question seed.\n";
    }

    check_sql(sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr), db, "commit tx");

    auto user_count = count_table(db, "users").value_or(-1);
    auto q_count = count_table(db, "questions").value_or(-1);
    std::cout << "Seed completed. users=" << user_count << " questions=" << q_count << "\n";
    print_sample(db, "EASY");
    print_sample(db, "MEDIUM");
    print_sample(db, "HARD");

    sqlite3_close(db);
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "Seed error: " << ex.what() << "\n";
    return 1;
  }
}
