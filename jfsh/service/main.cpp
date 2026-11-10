// jeff-service — System 1 HTTP inference service for jfsh.
//
// Wraps a jeff model checkpoint in a minimal HTTP server so jfsh connects
// via JeffRouter without linking against libtorch itself.  Multiple shell
// sessions share one model process; the model stays hot between commands.
//
// Protocol:
//   POST /classify  {"line":"<shell input>"}
//     → {"verdict":"run"|"agent","confidence":<0-1>,"model":"jeff"}
//   GET  /health
//     → {"status":"ok","model":"jeff"}
//
// Usage:
//   jeff-service [--model DIR] [--host H] [--port P]
//   Defaults: host=127.0.0.1  port=8765

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <atomic>
#include <csignal>
#include <string>
#include <string_view>

#include <httplib.h>
#include <nlohmann/json.hpp>

// ---------------------------------------------------------------------------
// TorchClassifier — wraps the jeff TorchScript model.
//
// TODO(service/load): Implement model loading and inference.
//   Steps:
//   1. Load tokenizer vocab from model_dir/tokenizer.json.
//      ModernBERT uses a byte-level BPE tokenizer; load it via the
//      HuggingFace tokenizers C++ port (tokenizers-cpp) or implement
//      WordPiece directly from the vocab file.
//   2. Load the encoder: torch::jit::load(model_dir + "/encoder.pt")
//      and move to device (CPU or CUDA if available).
//   3. Load the decision head: torch::load(head, model_dir + "/head.pt")
//   4. Set both to eval mode.
//   5. In classify():
//      a. Build the System 1 context string — see make_context() in
//         python/experiments/exp_006_zsh_router/infer.py for the exact
//         format (prepends shell type prefix, appends few-shot context).
//      b. Tokenize to input_ids / attention_mask tensors (int64, shape
//         [1, seq_len]).  Truncate to the model's max_length (512).
//      c. Run the encoder: auto out = encoder.forward({input_ids, mask})
//         and extract the [CLS] embedding: out.toTensor()[0][0].
//      d. Apply head: auto logits = head->forward(cls_emb).squeeze();
//      e. Softmax over dim 0, return {argmax label, max probability}.
//         Label order: 0 = "run", 1 = "agent" (matches training data).
// ---------------------------------------------------------------------------
struct TorchClassifier {
  std::string model_dir;
  bool        loaded = false;

  struct Result {
    std::string verdict    = "run";
    float       confidence = 0.9f;
  };

  void load(std::string_view dir) {
    model_dir = std::string(dir);
    // TODO(service/load): implement — see block comment above
    std::fprintf(stderr,
                 "jeff-service: model loading not yet implemented; "
                 "falling back to heuristic (zsh -n)\n");
  }

  Result classify(std::string_view line) const {
    if (not loaded) return heuristic(line);
    // TODO(service/infer): run tokenise → encoder → head → softmax
    return heuristic(line);
  }

private:
  // Heuristic fallback: valid zsh syntax → run, else → agent.
  static Result heuristic(std::string_view line) {
    std::string cmd = "zsh -n -c '";
    for (char c : line) {
      if (c == '\'') cmd += "'\\''";
      else           cmd += c;
    }
    cmd += "' 2>/dev/null";
    int rc = std::system(cmd.c_str());
    return {rc == 0 ? "run" : "agent", 0.9f};
  }
};

// ---------------------------------------------------------------------------
// Signal handling — clean shutdown on SIGINT / SIGTERM
// ---------------------------------------------------------------------------
static std::atomic<httplib::Server*> g_server{nullptr};

static void on_signal(int) {
  if (auto* svr = g_server.load()) svr->stop();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  std::string host      = "127.0.0.1";
  int         port      = 8765;
  std::string model_dir;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--model" and i + 1 < argc) {
      model_dir = argv[++i];
    } else if (arg == "--host" and i + 1 < argc) {
      host = argv[++i];
    } else if (arg == "--port" and i + 1 < argc) {
      port = std::atoi(argv[++i]);
    } else if (arg == "--help" or arg == "-h") {
      std::fprintf(stdout,
          "Usage: %s [--model DIR] [--host H] [--port P]\n"
          "  --model DIR  path to jeff checkpoint directory\n"
          "  --host H     bind address (default: 127.0.0.1)\n"
          "  --port P     listen port  (default: 8765)\n",
          argv[0]);
      return 0;
    }
  }

  TorchClassifier classifier;
  if (not model_dir.empty()) classifier.load(model_dir);

  httplib::Server svr;
  g_server.store(&svr);
  std::signal(SIGINT,  on_signal);
  std::signal(SIGTERM, on_signal);

  svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
    nlohmann::json j = {{"status", "ok"}, {"model", "jeff"}};
    res.set_content(j.dump(), "application/json");
  });

  svr.Post("/classify",
      [&classifier](const httplib::Request& req, httplib::Response& res) {
    try {
      auto body = nlohmann::json::parse(req.body);
      auto line = body.at("line").get<std::string>();
      auto r    = classifier.classify(line);
      nlohmann::json out = {
        {"verdict",    r.verdict},
        {"confidence", r.confidence},
        {"model",      "jeff"},
      };
      res.set_content(out.dump(), "application/json");
    } catch (const std::exception& e) {
      res.status = 400;
      nlohmann::json err = {{"error", e.what()}};
      res.set_content(err.dump(), "application/json");
    }
  });

  std::fprintf(stderr,
               "jeff-service: listening on %s:%d  (model: %s)\n",
               host.c_str(), port,
               model_dir.empty() ? "heuristic fallback" : model_dir.c_str());

  svr.listen(host, port);
  return 0;
}
