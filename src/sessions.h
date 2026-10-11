// src/sessions.h -- the saved session inventory, as data.
//
// One saved connection is a Session; Sessions live in Folders; Folders make a
// Tree, which is one YAML file. Nothing here touches GTK, so every rule about
// what a session may contain is testable without a display.
//
// The file format is PathfinderSSH's sessions.yaml, read as-is:
//
//   version: 1
//   folders:
//     - folder_name: Lab
//       sessions:
//         - name: eng-spine-1
//           transport: ssh
//           host: 172.16.2.5
//
// Folders are one level deep: a folder holds sessions, never folders.
//
// tmssh models only ssh and telnet. Serial sessions in a loaded file are
// skipped and counted; keys tmssh does not model (credential, anti_idle,
// paste pacing, vendor/model/notes, ...) are ignored on load and are not
// written back. tmssh owns its own file, so that loss is by design.
//
// Errors from the editing operations come back as a std::string: empty means
// it worked, anything else is the message to show.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tmssh {

enum class Transport { Ssh, Telnet };

const char *to_string(Transport t);
std::optional<Transport> parse_transport(std::string_view s);
int default_port(Transport t);

// auth_type values. Agent leaves ssh to its defaults (agent, then keys, then
// whatever the server offers).
inline constexpr const char *kAuthAgent = "agent";
inline constexpr const char *kAuthPassword = "password";
inline constexpr const char *kAuthPublicKey = "publickey";

// host_key_policy values. Strict needs the key already known; TOFU accepts a
// first key and remembers it; Insecure does neither and never remembers.
inline constexpr const char *kHostKeyStrict = "strict";
inline constexpr const char *kHostKeyTofu = "tofu";
inline constexpr const char *kHostKeyInsecure = "insecure";

inline constexpr int kFileVersion = 1;
inline constexpr int kDefaultConnectTimeoutSec = 20;
inline constexpr const char *kDefaultTermType = "xterm-256color";

// One bastion hop in front of an ssh session. An empty host means direct.
struct JumpSpec {
    std::string host;
    int port = 0;
    std::string username;
    std::string key_path;

    bool in_use() const { return !host.empty(); }
};

bool operator==(const JumpSpec &a, const JumpSpec &b);
inline bool operator!=(const JumpSpec &a, const JumpSpec &b) { return !(a == b); }

struct Session {
    std::string name;
    Transport transport = Transport::Ssh;

    // ssh and telnet
    std::string host;
    int port = 0;

    // ssh
    std::string username;
    std::string auth_type;
    std::string key_path;
    JumpSpec jump;
    bool legacy_algorithms = false;
    std::string host_key_policy;
    std::string known_hosts;
    int connect_timeout_sec = 0;

    // telnet: expand a lone CR to CR LF. Unset means the default, which is on.
    std::optional<bool> telnet_crlf;

    // terminal. Zero font_size / scrollback_lines and an empty
    // terminal_theme mean "inherit the application setting".
    std::string term_type;
    std::string terminal_theme;
    int font_size = 0;
    int scrollback_lines = 0;

    // Fills in blanks and trims whitespace. Bad values stay bad; validate()
    // reports them rather than this quietly repairing them.
    Session normalized() const;

    // The given name, or the target when there is none.
    std::string label() const;

    // user@host:port, with the port left off when it is the default.
    std::string target() const;

    // Identity for import de-duplication: transport, ssh username, host,
    // port. A session renamed in the tree is still the same login; two
    // accounts on one device are two sessions. Empty when there is no host,
    // and an empty key matches nothing.
    std::string key() const;

    bool crlf() const { return !telnet_crlf.has_value() || *telnet_crlf; }
};

bool operator==(const Session &a, const Session &b);
inline bool operator!=(const Session &a, const Session &b) { return !(a == b); }

struct FieldError {
    std::string field;
    std::string message;
};

// Every problem, each tagged with its field, so a form can mark them all at
// once. Only the fields the session's transport uses are checked.
std::vector<FieldError> validate(const Session &s);

struct Folder {
    std::string name;
    std::vector<Session> sessions;

    // Position of a session by label, case-insensitive; -1 if absent.
    int session_index(std::string_view name) const;
};

struct Tree {
    int version = kFileVersion;
    std::vector<Folder> folders;

    // Position of a folder by name, case-insensitive; -1 if absent.
    int folder_index(std::string_view name) const;

    std::size_t session_count() const;

    [[nodiscard]] std::string add_folder(std::string_view name);
    [[nodiscard]] std::string rename_folder(std::string_view old_name, std::string_view new_name);
    // A folder that still has sessions is refused unless force is set.
    [[nodiscard]] std::string remove_folder(std::string_view name, bool force);

    // Adds to the end of a folder, creating the folder if needed.
    [[nodiscard]] std::string add(std::string_view folder, const Session &s);
    // Overwrites in place, keeping the session's position.
    [[nodiscard]] std::string replace(std::string_view folder, std::string_view name, const Session &s);
    [[nodiscard]] std::string remove(std::string_view folder, std::string_view name);
    // Moves between folders. If the destination refuses (name clash) the
    // session stays where it was.
    [[nodiscard]] std::string move(std::string_view from_folder, std::string_view name,
                                   std::string_view to_folder);
};

struct LoadResult {
    Tree tree;
    // Set when the file could not be read or parsed; tree is then empty.
    std::string error;
    // Serial sessions dropped on load.
    int skipped_serial = 0;
    // Problems that did not stop the load: an unknown transport, a number
    // field holding text, a duplicate folder merged into the first.
    std::vector<std::string> warnings;

    bool ok() const { return error.empty(); }
};

// Parses a session file's text. Accepts the versioned mapping and a bare
// top-level list of folders (the older TetherSSH shape). Empty text is an
// empty tree. Every session comes back normalized.
LoadResult parse_tree(std::string_view yaml);

// Reads and parses a file. A missing file is an empty tree, not an error:
// that is what a first run looks like.
LoadResult load_file(const std::string &path);

// Renders a tree as it is written to disk: normalized, with only the fields
// each session's transport uses.
std::string emit_tree(const Tree &tree);

// Writes through a temporary file in the same directory, then renames it over
// the target, so the file on disk is always either the old tree or the new
// one. Creates the directory if needed; the file is mode 0600.
[[nodiscard]] std::string save_file(const std::string &path, const Tree &tree);

// The same temp-file-and-rename write, for text that is already rendered.
[[nodiscard]] std::string write_text_atomic(const std::string &path, const std::string &text);

// A first session file for a new install: one folder, "Local", holding an ssh
// session to localhost as the given user (left out when empty), with a
// comment block naming every key a session can carry.
std::string seed_sessions_yaml(const std::string &username);

// $XDG_CONFIG_HOME/tmssh/sessions.yaml, else ~/.config/tmssh/sessions.yaml.
std::string default_sessions_path();

// ~/.pathfinderssh/sessions.yaml, the usual import source.
std::string pathfinder_sessions_path();

struct ImportResult {
    int added = 0;
    std::vector<std::string> skipped;  // address already in the tree
    std::vector<std::string> renamed;  // added under a new name to avoid a clash
    std::vector<std::string> rejected; // no host to connect to
};

// Merges src into dst folder by folder (matched case-insensitively), skipping
// any session whose key() is already anywhere in dst. Safe to re-run: what is
// already in dst is never touched.
ImportResult import_tree(Tree &dst, const Tree &src);

} // namespace tmssh
