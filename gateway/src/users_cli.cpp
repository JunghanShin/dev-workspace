#include "users_cli.hpp"

#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <string>

#include "auth.hpp"
#include "sys.hpp"
#include "ui_rules.hpp"

namespace gw {

namespace {

const char* const kCliUsage =
    "usage: gateway user add <name> <viewer|operator|maintainer>\n"
    "       gateway user passwd <name>\n"
    "       gateway user del <name>\n"
    "       gateway user list\n"
    "       gateway token add <user> <token-name>\n"
    "       gateway token del <user> <token-name>\n"
    "  (WORKSPACE_DATA=<data folder>; file <data>/config/users.json; passwords are read from the terminal or stdin)\n";

int usage() {
    std::fprintf(stderr, "%s", kCliUsage);
    return 2;
}

int fail(const std::string& msg) {
    std::fprintf(stderr, "gateway: %s\n", msg.c_str());
    return 1;
}

// 한 줄(터미널이면 글자를 보이지 않게)
bool read_secret(const char* prompt, std::string& out) {
    const bool tty = ::isatty(0);
    termios old{};
    if (tty) {
        std::fprintf(stderr, "%s", prompt);
        std::fflush(stderr);
        ::tcgetattr(0, &old);
        termios quiet = old;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        ::tcsetattr(0, TCSANOW, &quiet);
    }
    const bool ok = static_cast<bool>(std::getline(std::cin, out));
    if (tty) {
        ::tcsetattr(0, TCSANOW, &old);
        std::fprintf(stderr, "\n");
    }
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return ok;
}

bool ask_password(std::string& pw, std::string& err) {
    std::string again;
    if (!read_secret("비밀번호: ", pw) || !read_secret("비밀번호 다시: ", again)) {
        err = "password not given";
        return false;
    }
    if (pw != again) {
        err = "passwords do not match";
        return false;
    }
    if (pw.size() < 8 || pw.size() > 1024) {
        err = "password must be 8 to 1024 characters";
        return false;
    }
    return true;
}

uint32_t iterations() {
    long long n = kPbkdf2Iterations;
    const std::string v = env_or("WORKSPACE_AUTH_ITERATIONS", "");
    if (!v.empty() && (!py_int(v, n) || n < kMinIterations || n > kMaxIterations)) {
        std::fprintf(stderr, "gateway: WORKSPACE_AUTH_ITERATIONS ignored (%u..%u)\n", kMinIterations, kMaxIterations);
        n = kPbkdf2Iterations;
    }
    return static_cast<uint32_t>(n);
}

std::string hash_new(const std::string& pw) {
    const double t0 = mono_now();
    const uint32_t it = iterations();
    const std::string h = make_password_hash(pw, random_bytes(kSaltBytes), it);
    std::fprintf(stderr, "PBKDF2-HMAC-SHA256 %u회: %.0f ms\n", it, (mono_now() - t0) * 1000);
    return h;
}

}  // namespace

int users_cli(int argc, char** argv) {
    if (argc < 2) return -1;
    const std::string what = argv[1];
    if (what != "user" && what != "token") return -1;
    if (argc < 3) return usage();
    const std::string sub = argv[2];
    const std::string data = env_or("WORKSPACE_DATA", "");
    if (data.empty()) return fail("WORKSPACE_DATA is not set (the board data folder)");
    const std::string dir = path_join(data, "config");
    const std::string path = path_join(dir, "users.json");
    UsersDb db;
    std::string err;
    if (!load_users(path, db, err)) return fail(err + " (fix or remove the file first)");
    auto save = [&]() {
        ::mkdir(dir.c_str(), 0755);
        if (!write_file_atomic(path, dump_users(db), 0600, err)) return fail(err);
        return 0;
    };

    if (what == "user" && sub == "list" && argc == 3) {
        for (const auto& u : db.users) {
            std::string toks;
            for (const auto& t : u.tokens) toks += (toks.empty() ? "" : ",") + t.name;
            std::printf("%s\t%s\t%s\ttokens: %s\n", u.name.c_str(), role_name(u.role),
                        u.password_hash.empty() ? "no password" : "password", toks.empty() ? "-" : toks.c_str());
        }
        return 0;
    }
    if (what == "user" && sub == "add" && argc == 5) {
        const std::string name = argv[3];
        Role role;
        if (!valid_user_name(name)) return fail("bad name (letters, digits, _ . -; up to 64)");
        if (!parse_role(argv[4], role)) return fail("role must be viewer, operator or maintainer");
        if (db.find(name)) return fail("user " + name + " already exists");
        std::string pw;
        if (!ask_password(pw, err)) return fail(err);
        User u;
        u.name = name;
        u.role = role;
        u.password_hash = hash_new(pw);
        db.users.push_back(u);
        if (int rc = save()) return rc;
        std::printf("added %s (%s)\n", name.c_str(), role_name(role));
        return 0;
    }
    if (what == "user" && sub == "passwd" && argc == 4) {
        User* u = db.find(argv[3]);
        if (!u) return fail(std::string("no user ") + argv[3]);
        std::string pw;
        if (!ask_password(pw, err)) return fail(err);
        u->password_hash = hash_new(pw);  // 그 사용자의 화면 세션은 끝난다(세션은 만들 때의 해시를 봄)
        if (int rc = save()) return rc;
        std::printf("password changed for %s\n", argv[3]);
        return 0;
    }
    if (what == "user" && sub == "del" && argc == 4) {
        const std::string name = argv[3];
        const size_t before = db.users.size();
        for (auto it = db.users.begin(); it != db.users.end(); ++it)
            if (it->name == name) {
                db.users.erase(it);
                break;
            }
        if (db.users.size() == before) return fail("no user " + name);
        if (int rc = save()) return rc;
        std::printf("deleted %s\n", name.c_str());
        return 0;
    }
    if (what == "token" && sub == "add" && argc == 5) {
        User* u = db.find(argv[3]);
        if (!u) return fail(std::string("no user ") + argv[3]);
        const std::string tname = argv[4];
        if (!valid_token_name(tname)) return fail("bad token name (letters, digits, _ . -; up to 64)");
        for (const auto& t : u->tokens)
            if (t.name == tname) return fail("token " + tname + " already exists for " + u->name);
        const std::string token = to_hex(random_bytes(32));
        u->tokens.push_back({tname, sha256_hex(token)});
        if (int rc = save()) return rc;
        // 원문은 여기서 한 번만 보인다(파일에는 sha256만)
        std::fprintf(stderr, "token %s for %s (shown once; use: Authorization: Bearer <token>)\n", tname.c_str(),
                     u->name.c_str());
        std::printf("%s\n", token.c_str());
        return 0;
    }
    if (what == "token" && sub == "del" && argc == 5) {
        User* u = db.find(argv[3]);
        if (!u) return fail(std::string("no user ") + argv[3]);
        const size_t before = u->tokens.size();
        for (auto it = u->tokens.begin(); it != u->tokens.end(); ++it)
            if (it->name == argv[4]) {
                u->tokens.erase(it);
                break;
            }
        if (u->tokens.size() == before) return fail(std::string("no token ") + argv[4] + " for " + u->name);
        if (int rc = save()) return rc;
        std::printf("deleted token %s of %s\n", argv[4], u->name.c_str());
        return 0;
    }
    return usage();
}

}  // namespace gw
