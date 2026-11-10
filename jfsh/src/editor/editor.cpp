#include <jfsh/editor/editor.hh>
#include <jfsh/editor/completion.hh>
#include <cstdio>
#include <cstring>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

namespace jfsh {

// ---------------------------------------------------------------------------
// LineBuffer helpers
// ---------------------------------------------------------------------------

void LineBuffer::insert(std::string_view s) {
    text.insert(cursor, s);
    cursor += s.size();
}

void LineBuffer::delete_char_at(std::size_t pos) {
    if (pos < text.size()) text.erase(pos, 1);
}

void LineBuffer::move_to(std::size_t pos) {
    cursor = std::min(pos, text.size());
}

std::string_view LineBuffer::before_cursor() const noexcept {
    return std::string_view(text).substr(0, cursor);
}

std::string_view LineBuffer::after_cursor() const noexcept {
    return std::string_view(text).substr(cursor);
}

// ---------------------------------------------------------------------------
// Editor
// ---------------------------------------------------------------------------

Editor::Editor(const OptionState& opts,
               HistoryManager&    hist,
               PromptRenderer&    prompt)
    : opts_(opts), hist_(hist), prompt_(prompt) {
    if (opts_.get(OPT_VI)) mode_ = EditorMode::Vi;
    install_default_bindings();
}

Editor::~Editor() {
    if (raw_mode_active_) exit_raw_mode();
}

static struct termios s_orig_termios;

void Editor::enter_raw_mode() {
    tcgetattr(STDIN_FILENO, &s_orig_termios);
    struct termios raw = s_orig_termios;
    raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | ISIG | IEXTEN));
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | BRKINT | INPCK | ISTRIP));
    raw.c_cflag |= CS8;
    raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_active_ = true;
}

void Editor::exit_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &s_orig_termios);
    raw_mode_active_ = false;
}

std::optional<std::string> Editor::read_line() {
    buf_.text.clear();
    buf_.cursor = 0;
    hist_pos_   = -1;

    // Print primary prompt
    std::string ps1 = prompt_.primary();
    std::fwrite(ps1.c_str(), 1, ps1.size(), stdout);
    std::fflush(stdout);

    if (!isatty(STDIN_FILENO)) {
        // Non-interactive: read a full line
        std::string line;
        int c;
        while ((c = std::fgetc(stdin)) != EOF && c != '\n')
            line += static_cast<char>(c);
        if (c == EOF && line.empty()) return std::nullopt;
        return line;
    }

    enter_raw_mode();
    bool done = false;
    while (!done) {
        std::string key = read_key_sequence();
        if (key == "\x04" && buf_.text.empty()) {  // Ctrl-D on empty line = EOF
            exit_raw_mode();
            std::putchar('\n');
            return std::nullopt;
        }
        if (key == "\r" || key == "\n") {
            done = true;
        } else if (key == "\x7f" || key == "\x08") {  // Backspace
            if (buf_.cursor > 0) {
                --buf_.cursor;
                buf_.text.erase(buf_.cursor, 1);
            }
        } else if (key == "\x1b[A") {  // Up arrow
            widget_up_line_or_history(buf_);
        } else if (key == "\x1b[B") {  // Down arrow
            widget_down_line_or_history(buf_);
        } else if (key == "\x1b[C") {  // Right
            if (buf_.cursor < buf_.text.size()) ++buf_.cursor;
        } else if (key == "\x1b[D") {  // Left
            if (buf_.cursor > 0) --buf_.cursor;
        } else if (key == "\x01") {    // Ctrl-A
            buf_.cursor = 0;
        } else if (key == "\x05") {    // Ctrl-E
            buf_.cursor = buf_.text.size();
        } else if (key == "\x09") {    // Tab
            if (completer_) {
                auto candidates = completer_(buf_.before_cursor());
                if (candidates.size() == 1) buf_.insert(candidates[0].substr(buf_.cursor));
            }
        } else if (!key.empty() && static_cast<unsigned char>(key[0]) >= 0x20) {
            buf_.insert(key);
        }
        refresh_display();
    }
    exit_raw_mode();
    std::putchar('\n');
    std::fflush(stdout);
    return buf_.text;
}

std::string Editor::read_key_sequence() {
    char buf[8] = {};
    int n = static_cast<int>(read(STDIN_FILENO, buf, 1));
    if (n <= 0) return {};
    if (buf[0] == '\x1b') {
        // Try to read more bytes (escape sequence)
        struct timeval tv{0, 50000};
        fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
            n += static_cast<int>(read(STDIN_FILENO, buf + n, sizeof(buf) - static_cast<std::size_t>(n)));
        }
    }
    return std::string(buf, static_cast<std::size_t>(n));
}

void Editor::refresh_display() {
    // Move to start of line, clear, re-print prompt + buffer
    std::string ps1 = prompt_.primary();
    int ps1_width = PromptRenderer::visual_width(ps1);
    // CR + erase line
    write(STDOUT_FILENO, "\r\x1b[K", 4);
    write(STDOUT_FILENO, ps1.c_str(), ps1.size());
    write(STDOUT_FILENO, buf_.text.c_str(), buf_.text.size());
    // Move cursor to position
    int col = ps1_width + static_cast<int>(buf_.cursor);
    if (col > 0) {
        char seq[32];
        int len = std::snprintf(seq, sizeof seq, "\r\x1b[%dC", col);
        if (len > 0) write(STDOUT_FILENO, seq, static_cast<std::size_t>(len));
    }
}

bool Editor::widget_up_line_or_history(LineBuffer& buf) {
    const auto& entries = hist_.entries();
    if (entries.empty()) return false;
    if (hist_pos_ < 0) {
        saved_line_ = buf.text;
        hist_pos_ = static_cast<int>(entries.size()) - 1;
    } else if (hist_pos_ > 0) {
        --hist_pos_;
    } else {
        return false;
    }
    buf.text   = entries[static_cast<std::size_t>(hist_pos_)].text;
    buf.cursor = buf.text.size();
    return true;
}

bool Editor::widget_down_line_or_history(LineBuffer& buf) {
    if (hist_pos_ < 0) return false;
    const auto& entries = hist_.entries();
    if (hist_pos_ < static_cast<int>(entries.size()) - 1) {
        ++hist_pos_;
        buf.text   = entries[static_cast<std::size_t>(hist_pos_)].text;
        buf.cursor = buf.text.size();
    } else {
        hist_pos_ = -1;
        buf.text   = saved_line_;
        buf.cursor = buf.text.size();
    }
    return true;
}

bool Editor::widget_self_insert(LineBuffer& buf)         { return false; }
bool Editor::widget_backward_delete_char(LineBuffer& buf){ return false; }
bool Editor::widget_beginning_of_line(LineBuffer& buf)   { buf.cursor = 0; return true; }
bool Editor::widget_end_of_line(LineBuffer& buf)         { buf.cursor = buf.text.size(); return true; }
bool Editor::widget_accept_line(LineBuffer&)             { return true; }
bool Editor::widget_complete_word(LineBuffer&)           { return false; }
bool Editor::widget_expand_or_complete(LineBuffer&)      { return false; }

void Editor::suspend() { if (raw_mode_active_) exit_raw_mode(); }
void Editor::resume()  { }

void Editor::bind_key(std::string /*km*/, std::string /*seq*/, std::string /*widget*/) {}
void Editor::register_widget(ZleWidget /*w*/) {}
void Editor::set_mode(EditorMode mode) { mode_ = mode; }
void Editor::redraw() { refresh_display(); }

int Editor::terminal_width() const noexcept {
    struct winsize ws{}; ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    return ws.ws_col > 0 ? ws.ws_col : 80;
}

int Editor::terminal_height() const noexcept {
    struct winsize ws{}; ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    return ws.ws_row > 0 ? ws.ws_row : 24;
}

bool Editor::dispatch_widget(std::string_view /*name*/) { return false; }

void Editor::install_default_bindings() {
    if (mode_ == EditorMode::Vi) install_vi_bindings();
    else                          install_emacs_bindings();
}

void Editor::install_vi_bindings()    {}
void Editor::install_emacs_bindings() {}

} // namespace jfsh
