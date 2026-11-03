#pragma once

#include <string>
#include <vector>

namespace jeff {

// Thin wrapper; full tokenization happens via the exported TorchScript tokenizer
// or a bundled sentencepiece/tiktoken library.
class Tokenizer {
public:
    explicit Tokenizer(const std::string& vocab_path);

    std::vector<int64_t> encode(const std::string& text) const;
    std::string decode(const std::vector<int64_t>& ids) const;

private:
    // placeholder — swap in sentencepiece or tiktoken bindings
    std::string vocab_path_;
};

} // namespace jeff
