// jeff-server — System 1 HTTP inference service.
//
// Wraps a jeff TorchScript model and exposes two endpoints so jfsh connects
// via JeffRouter without linking against libtorch:
//   POST /classify  {"line":"<shell input>"}
//     → {"verdict":"run"|"agent","confidence":<0-1>,"model":"jeff"}
//   GET  /health
//     → {"status":"ok","model":"jeff"}
//
// Usage:
//   jeff-server --model <model.pt> [--host 127.0.0.1] [--port 8765]

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include <httplib.h>
#include "json.h"
#include "model.h"

static std::atomic<httplib::Server*> g_server{nullptr};

static void on_signal(int) {
  if (auto* svr = g_server.load()) svr->stop();
}

int main(int argc, char* argv[]) {
  std::string host;
  int         port       = 8765;
  std::string model_path;
  host = "127.0.0.1";

  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--model" and i + 1 < argc) {
      model_path = argv[++i];
    } else if (arg == "--host" and i + 1 < argc) {
      host = argv[++i];
    } else if (arg == "--port" and i + 1 < argc) {
      port = std::atoi(argv[++i]);
    } else if (arg == "--help" or arg == "-h") {
      std::fprintf(stdout,
          "Usage: %s --model PATH [--host H] [--port P]\n"
          "  --model PATH  TorchScript model file (.pt)\n"
          "  --host H      bind address (default: 127.0.0.1)\n"
          "  --port P      listen port  (default: 8765)\n",
          argv[0]);
      return 0;
    }
  }

  if (model_path.empty()) {
    std::fprintf(stderr, "jeff-server: --model PATH is required\n");
    return 1;
  }

  jeff::DecisionModel model(model_path);

  httplib::Server svr;
  g_server.store(&svr);
  std::signal(SIGINT,  on_signal);
  std::signal(SIGTERM, on_signal);

  svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
    jeff::json::Writer w;
    w.begin_object().field("status", "ok").field("model", "jeff").end_object();
    res.set_content(w.str(), "application/json");
  });

  svr.Post("/classify",
      [&model](const httplib::Request& req, httplib::Response& res) {
    auto fail = [&res](std::string_view message) {
      jeff::json::Writer w;
      w.begin_object().field("error", message).end_object();
      res.status = 400;
      res.set_content(w.str(), "application/json");
    };

    try {
      auto body = jeff::json::parse(req.body);
      if (not body) return fail(body.error);
      const jeff::json::Value* line_v = body.value.find("line");
      if (not line_v or not line_v->is_string())
        return fail("missing string field \"line\"");
      const std::string& line = *line_v->as_string();

      jeff::DecisionInput input;
      input.context = line;
      input.options = {"run", "agent"};

      auto out = model.decide(input);

      // option 0 = "run", option 1 = "agent" (matches training label order)
      std::string verdict =
          (out.chosen_index == 1) ? "agent" : "run";
      float confidence =
          (out.chosen_index < static_cast<int>(out.scores.size()))
              ? out.scores[static_cast<std::size_t>(out.chosen_index)]
              : 0.9f;

      jeff::json::Writer w;
      w.begin_object()
          .field("verdict",    verdict)
          .field("confidence", confidence)
          .field("model",      "jeff")
          .end_object();
      res.set_content(w.str(), "application/json");
    } catch (const std::exception& e) {
      fail(e.what());
    }
  });

  std::fprintf(stderr,
               "jeff-server: listening on %s:%d  (model: %s)\n",
               host.c_str(), port, model_path.c_str());

  svr.listen(host, port);
  return 0;
}
