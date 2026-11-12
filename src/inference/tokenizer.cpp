#include "tokenizer.h"
#include <stdexcept>

namespace jeff {

Tokenizer::Tokenizer(const std::string& vocab_path) : vocab_path_(vocab_path) {
    // TODO: load sentencepiece model or tiktoken vocab
}

std::vector<int64_t> Tokenizer::encode(const std::string& /*text*/) const {
    throw std::runtime_error("Tokenizer::encode not yet implemented");
}

std::string Tokenizer::decode(const std::vector<int64_t>& /*ids*/) const {
    throw std::runtime_error("Tokenizer::decode not yet implemented");
}

} // namespace jeff
