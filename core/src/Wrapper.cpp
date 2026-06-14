#include "Dispatcher.h"
#include "ExchangeEngine.h"
#include "NetworkServer.h"
#include "RunContext.h"
#include "Telemetry.h"
#include <iostream>
#include <thread>
#include <unistd.h>

int main() {
  std::cout << std::unitbuf;
  ExchangeEngine *engine = CreateEngine();
  engine->Init();

  RunContext ctx;

  if (ctx.IsBenchmarkMode()) {

    // One process, many probes: poison pill ends a probe; Clear() + reset
    // between iterations prepares a clean book for the next bracket.
    for (;;) {
      std::thread dispatcher_thread(RunDispatcher, engine, std::ref(ctx));
      std::thread network_thread(RunNetworkServer, std::ref(ctx));
      network_thread.join();
      dispatcher_thread.join();

      if (ctx.IsHealthBreach()) {
        std::cout << "[Wrapper] Health breach detected. Terminating process." << std::endl;
        _exit(0);
      }

      ctx.ResetForNextProbe();
      Telemetry::Reset();
      engine->Clear();
    }
  }

  std::thread dispatcher_thread(RunDispatcher, engine, std::ref(ctx));
  std::thread network_thread(RunNetworkServer, std::ref(ctx));
  network_thread.join();
  dispatcher_thread.join();

  delete engine;
  return 0;
}
