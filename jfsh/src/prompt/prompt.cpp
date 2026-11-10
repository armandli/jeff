#include <jfsh/prompt/prompt.hh>
#include <ctime>
#include <cstring>
#include <unistd.h>
#include <pwd.h>

namespace jfsh {

PromptRenderer::PromptRenderer(const ParamTable& params, const OptionState& opts)
    : params_(params), opts_(opts) {
    reload();
}

void PromptRenderer::reload() {
    auto get = [&](const char* name, std::string def) -> std::string {
        const Param* p = params_.lookup(name);
        return (p && p->is_set()) ? p->scalar() : def;
    };
    vars_.PS1    = get("PROMPT",  get("PS1",   "%n@%m %1~ %# "));
    vars_.PS2    = get("PROMPT2", get("PS2",   "> "));
    vars_.PS3    = get("PS3",     "?# ");
    vars_.PS4    = get("PS4",     "+ ");
    vars_.RPS1   = get("RPROMPT", get("RPS1",  ""));
    vars_.RPS2   = get("RPROMPT2",get("RPS2",  ""));
    vars_.SPROMPT= get("SPROMPT", "zsh: correct '%R' to '%r' [nyae]? ");
}

std::string PromptRenderer::render(std::string_view prompt_str, bool strip_escapes) const {
    std::string out;
    out.reserve(prompt_str.size() * 2);
    for (std::size_t i = 0; i < prompt_str.size(); ++i) {
        if (prompt_str[i] != '%') {
            out += prompt_str[i];
            continue;
        }
        ++i;
        if (i >= prompt_str.size()) break;
        auto [expanded, consumed] = expand_percent(prompt_str, static_cast<int>(i));
        if (strip_escapes) {
            // Strip ANSI sequences from expanded
            bool in_esc = false;
            for (char c : expanded) {
                if (c == '\x1b') { in_esc = true; continue; }
                if (in_esc) { if (c == 'm') in_esc = false; continue; }
                out += c;
            }
        } else {
            out += expanded;
        }
        i += static_cast<std::size_t>(consumed - 1);
    }
    return out;
}

std::pair<std::string, int>
PromptRenderer::expand_percent(std::string_view src, int pos) const {
    if (pos >= static_cast<int>(src.size())) return {{}, 0};
    char c = src[static_cast<std::size_t>(pos)];
    switch (c) {
        case 'n': { // username
            struct passwd* pw = getpwuid(getuid());
            return {pw ? pw->pw_name : "user", 1};
        }
        case 'm': { // short hostname
            char host[256]; gethostname(host, sizeof host);
            char* dot = strchr(host, '.'); if (dot) *dot = '\0';
            return {host, 1};
        }
        case 'M': { // full hostname
            char host[256]; gethostname(host, sizeof host);
            return {host, 1};
        }
        case '~': { // current dir with ~ substitution
            char buf[4096];
            const char* cwd = getcwd(buf, sizeof buf);
            if (!cwd) return {"?", 1};
            const char* home = getenv("HOME");
            if (home && strncmp(cwd, home, strlen(home)) == 0)
                return {"~" + std::string(cwd + strlen(home)), 1};
            return {cwd, 1};
        }
        case '/': case '.': { // last dir component
            char buf[4096];
            const char* cwd = getcwd(buf, sizeof buf);
            if (!cwd) return {"?", 1};
            const char* slash = strrchr(cwd, '/');
            return {slash ? slash + 1 : cwd, 1};
        }
        case '#': { // root indicator
            return {geteuid() == 0 ? "#" : "%", 1};
        }
        case 'h': case '!': { // history event number
            return {"0", 1};
        }
        case 'D': { // date
            time_t t = time(nullptr);
            char buf[32];
            strftime(buf, sizeof buf, "%y-%m-%d", localtime(&t));
            return {buf, 1};
        }
        case 'T': { // time HH:MM
            time_t t = time(nullptr);
            char buf[32];
            strftime(buf, sizeof buf, "%H:%M", localtime(&t));
            return {buf, 1};
        }
        case '*': { // time HH:MM:SS
            time_t t = time(nullptr);
            char buf[32];
            strftime(buf, sizeof buf, "%H:%M:%S", localtime(&t));
            return {buf, 1};
        }
        case 'F': { // color: %F{color}
            // Skip {color} argument
            int skip = 1;
            if (pos + 1 < static_cast<int>(src.size()) && src[static_cast<std::size_t>(pos+1)] == '{') {
                auto end = src.find('}', static_cast<std::size_t>(pos + 2));
                if (end != std::string_view::npos)
                    skip = static_cast<int>(end - static_cast<std::size_t>(pos)) + 1;
            }
            return {"\x1b[39m", skip};
        }
        case 'f': return {"\x1b[39m", 1};  // %f reset foreground
        case '%': return {"%", 1};
        case ')': return {"", 1};
        default:  return {std::string(1, c), 1};
    }
}

std::string PromptRenderer::do_subst(std::string_view s) const {
    // TODO(prompt/subst): When PROMPT_SUBST option is set, run a full parameter
    //   and command substitution pass over the prompt string before percent
    //   expansion.  This allows prompts like:
    //     PS1='$(git_branch)%n@%m %~%# '
    //   Implementation: create a StringInputSource, Lexer, and call the
    //   substitution expander from lexer/subst.cpp on the prompt string.
    //   The result replaces s before expand_percent is called.
    return std::string(s);
}

int PromptRenderer::visual_width(std::string_view rendered) noexcept {
    int width = 0;
    bool in_esc = false;
    for (char c : rendered) {
        if (c == '\x1b') { in_esc = true; continue; }
        if (in_esc) { if (c == 'm') in_esc = false; continue; }
        ++width;
    }
    return width;
}

} // namespace jfsh
