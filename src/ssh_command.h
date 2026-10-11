// src/ssh_command.h -- a Session as an OpenSSH command line.
//
// tmssh does not speak SSH itself: an ssh session is the system `ssh` binary
// run in the tab's pty. That gives ~/.ssh/config, the agent, ProxyJump,
// known_hosts and every prompt (password, passphrase, host key) for free,
// handled in the terminal where the person can see and answer them.
//
// Nothing here touches GTK or runs anything except probe_ssh_algorithms().
#pragma once

#include <set>
#include <string>
#include <vector>

#include "sessions.h"

namespace tmssh {

// What the local ssh client supports, from `ssh -Q`. Legacy algorithms are
// appended with "+" syntax, and ssh refuses to start if a "+" names an
// algorithm its build does not have (OpenSSH 9.8 dropped ssh-dss, for one),
// so the legacy lists are cut down to what this client actually offers.
struct SshAlgorithms {
    std::set<std::string> kex;
    std::set<std::string> ciphers;
    std::set<std::string> macs;
    std::set<std::string> host_keys;
    bool probed = false; // false: nothing known, so no legacy options are added
};

// Runs `ssh -Q kex|cipher|mac|key`. If ssh is missing or fails, probed is
// false and the sets are empty.
SshAlgorithms probe_ssh_algorithms(const std::string &ssh_program = "ssh");

// The legacy set PathfinderSSH appends for the same per-session switch.
extern const std::vector<std::string> kLegacyKex;
extern const std::vector<std::string> kLegacyCiphers;
extern const std::vector<std::string> kLegacyMacs;
extern const std::vector<std::string> kLegacyHostKeys;

struct Command {
    std::string program;
    std::vector<std::string> args; // not including program
    std::vector<std::string> env;  // "NAME=value", added to the child's environment

    // For display and logs: the command as a shell would read it.
    std::string display() const;
};

// The ssh command for a session.
//
// Mapping:
//   port / username           -p / -l
//   auth_type agent           nothing (ssh's own order)
//   auth_type password        PreferredAuthentications=keyboard-interactive,password,
//                             PubkeyAuthentication=no
//   auth_type publickey       -i key_path, IdentitiesOnly=yes
//   host_key_policy           StrictHostKeyChecking yes / accept-new / no
//                             (insecure also uses UserKnownHostsFile=/dev/null)
//   known_hosts               UserKnownHostsFile=<path>
//   connect_timeout_sec       ConnectTimeout=<n>
//   jump (no key)             -J [user@]host[:port]
//   jump (with key_path)      ProxyCommand="ssh -i key -p port -l user -W %h:%p host"
//   legacy_algorithms         +KexAlgorithms/Ciphers/MACs/HostKeyAlgorithms,
//                             each cut to what `algorithms` says ssh supports
//   term_type                 TERM in the environment (ssh forwards it)
//
// A leading "~/" in key paths and known_hosts is expanded against $HOME.
Command ssh_command(const Session &s, const SshAlgorithms &algorithms,
                    const std::string &ssh_program = "ssh");

// Quotes a word for /bin/sh: unchanged if it is plainly safe, otherwise in
// single quotes. ProxyCommand is run through the shell, so its words need it.
std::string shell_quote(const std::string &word);

// "~/x" -> "$HOME/x"; anything else unchanged.
std::string expand_home(const std::string &path);

} // namespace tmssh
