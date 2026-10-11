// src/ssh_command.cpp -- see ssh_command.h.
#include "ssh_command.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>

namespace tmssh {

const std::vector<std::string> kLegacyKex = {
    "diffie-hellman-group14-sha1",
    "diffie-hellman-group1-sha1",
    "diffie-hellman-group-exchange-sha256",
    "diffie-hellman-group-exchange-sha1",
};
const std::vector<std::string> kLegacyCiphers = {"aes128-cbc", "aes192-cbc", "aes256-cbc",
                                                 "3des-cbc"};
const std::vector<std::string> kLegacyMacs = {"hmac-sha1", "hmac-sha1-96", "hmac-md5",
                                              "hmac-md5-96"};
const std::vector<std::string> kLegacyHostKeys = {"ssh-rsa", "ssh-dss"};

namespace {

std::set<std::string> query(const std::string &ssh_program, const char *what, bool &ok) {
    std::set<std::string> out;
    const std::string cmd = shell_quote(ssh_program) + " -Q " + what + " 2>/dev/null";
    FILE *p = ::popen(cmd.c_str(), "r");
    if (p == nullptr) {
        ok = false;
        return out;
    }
    char line[512];
    while (std::fgets(line, sizeof line, p) != nullptr) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
            s.pop_back();
        if (!s.empty()) out.insert(s);
    }
    const int status = ::pclose(p);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || out.empty()) ok = false;
    return out;
}

// "+a,b" for the legacy names this client has; empty when it has none.
std::string plus_list(const std::vector<std::string> &wanted, const std::set<std::string> &have) {
    std::string out;
    for (const auto &w : wanted) {
        if (have.count(w) == 0) continue;
        out += out.empty() ? "+" : ",";
        out += w;
    }
    return out;
}

bool is_ipv6_literal(const std::string &host) {
    return host.find(':') != std::string::npos && host.front() != '[';
}

} // namespace

SshAlgorithms probe_ssh_algorithms(const std::string &ssh_program) {
    SshAlgorithms a;
    bool ok = true;
    a.kex = query(ssh_program, "kex", ok);
    a.ciphers = query(ssh_program, "cipher", ok);
    a.macs = query(ssh_program, "mac", ok);
    a.host_keys = query(ssh_program, "key", ok);
    a.probed = ok;
    if (!ok) a = SshAlgorithms{};
    return a;
}

std::string shell_quote(const std::string &word) {
    if (word.empty()) return "''";
    bool safe = true;
    for (unsigned char c : word) {
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '/' || c == ':' ||
              c == '@' || c == '%' || c == '+' || c == ',' || c == '=')) {
            safe = false;
            break;
        }
    }
    if (safe) return word;
    std::string out = "'";
    for (char c : word) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    return out + "'";
}

std::string expand_home(const std::string &path) {
    if (path == "~" || path.rfind("~/", 0) == 0) {
        const char *home = std::getenv("HOME");
        if (home && *home) return std::string(home) + path.substr(1);
    }
    return path;
}

std::string Command::display() const {
    std::string out;
    for (const auto &e : env) out += shell_quote(e) + " ";
    out += shell_quote(program);
    for (const auto &a : args) out += " " + shell_quote(a);
    return out;
}

Command ssh_command(const Session &in, const SshAlgorithms &algorithms,
                    const std::string &ssh_program) {
    const Session s = in.normalized();
    Command c;
    c.program = ssh_program;
    auto opt = [&](const std::string &o) {
        c.args.push_back("-o");
        c.args.push_back(o);
    };

    c.args.push_back("-p");
    c.args.push_back(std::to_string(s.port));
    if (!s.username.empty()) {
        c.args.push_back("-l");
        c.args.push_back(s.username);
    }

    if (s.auth_type == kAuthPassword) {
        opt("PreferredAuthentications=keyboard-interactive,password");
        opt("PubkeyAuthentication=no");
    } else if (s.auth_type == kAuthPublicKey && !s.key_path.empty()) {
        c.args.push_back("-i");
        c.args.push_back(expand_home(s.key_path));
        opt("IdentitiesOnly=yes");
    }

    if (s.host_key_policy == kHostKeyStrict) {
        opt("StrictHostKeyChecking=yes");
    } else if (s.host_key_policy == kHostKeyInsecure) {
        opt("StrictHostKeyChecking=no");
        opt("UserKnownHostsFile=/dev/null");
    } else {
        opt("StrictHostKeyChecking=accept-new");
    }
    if (!s.known_hosts.empty() && s.host_key_policy != kHostKeyInsecure)
        opt("UserKnownHostsFile=" + expand_home(s.known_hosts));

    opt("ConnectTimeout=" + std::to_string(s.connect_timeout_sec));

    if (s.jump.in_use()) {
        const int jport = s.jump.port == 0 ? 22 : s.jump.port;
        if (s.jump.key_path.empty()) {
            std::string j;
            if (!s.jump.username.empty()) j += s.jump.username + "@";
            j += is_ipv6_literal(s.jump.host) ? "[" + s.jump.host + "]" : s.jump.host;
            if (jport != 22) j += ":" + std::to_string(jport);
            c.args.push_back("-J");
            c.args.push_back(j);
        } else {
            // -J cannot carry a key, so the hop is spelled out. ProxyCommand
            // goes through /bin/sh; every word is quoted for it.
            std::string pc = shell_quote(ssh_program) + " -i " +
                             shell_quote(expand_home(s.jump.key_path)) +
                             " -o IdentitiesOnly=yes -p " + std::to_string(jport);
            if (!s.jump.username.empty()) pc += " -l " + shell_quote(s.jump.username);
            pc += " -W %h:%p -- " + shell_quote(s.jump.host);
            opt("ProxyCommand=" + pc);
        }
    }

    if (s.legacy_algorithms && algorithms.probed) {
        if (auto v = plus_list(kLegacyKex, algorithms.kex); !v.empty()) opt("KexAlgorithms=" + v);
        if (auto v = plus_list(kLegacyCiphers, algorithms.ciphers); !v.empty())
            opt("Ciphers=" + v);
        if (auto v = plus_list(kLegacyMacs, algorithms.macs); !v.empty()) opt("MACs=" + v);
        if (auto v = plus_list(kLegacyHostKeys, algorithms.host_keys); !v.empty())
            opt("HostKeyAlgorithms=" + v);
    }

    // "--" ends ssh's options, so a host that starts with '-' is a host.
    c.args.push_back("--");
    c.args.push_back(s.host);

    c.env.push_back("TERM=" + s.term_type);
    return c;
}

} // namespace tmssh
